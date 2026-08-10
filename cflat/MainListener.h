#pragma once
// ============================================================
// MainListener.h - CFlat front-end: ForwardRefScanner + MainListener
// ============================================================
// SECTION   DESCRIPTION                       JUMP TO (grep a function name; line numbers drift)
// ────────────────────────────────────────────────────────────────────────────────────────────
// §1   File-level helpers                IsReturnBlockFunction, getOperatorName
// §2   ForwardRefScanner class (pre-pass)  class ForwardRefScanner
// §3   MainListener class (codegen)        class MainListener
//   §3.1  ParseDeclarationSpecifiers (codegen)  ParseDeclarationSpecifiers
//   §3.2  Interface/generic instantiation       InstantiateGenericInterface, InstantiateGenericFunction
//   §3.3  Top-level declarations                ParseUsingDeclaration, ParseExternalDeclaration
//   §3.4  Statement parsing                     ParseBlockItemList, ParseStatement
//   §3.5  Function/parameter declarations       ParseFunctionDefinition, ParseDeclaration
//   §3.6  Expression parsing                    ParseAssignmentExpressionNamed, ParseConditionalExpression
//   §3.7  ParsePostfixExpression (~3000 lines)  ParsePostfixExpression, ParseLambdaExpression
//         grep "[PFX-n]" inside: 1 member-access op . -> ?. | 2 member name (field/defer) |
//         3 subscript [] | 4 call (args) | 5 indirect fn-ptr call | 6 arg assembly + implicit this |
//         7 call lowering (winrt vtable / null-conditional / overloaded)
//   §3.8  Generic instantiation queue           QueueInstantiateGenericType
//   §3.9  Struct/Class definitions              ParseStructDefinition, ParseClassDefinition
//   §3.10 Constructor/Destructor                ParseConstructorDefinition, ParseDestructorDefinition
//   §3.11 Utilities                             ParseParameterTypeList, ParseNumberConstant
// ============================================================

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <format>
#include <variant>
#include <optional>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <charconv>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "LspSymbolIndex.h"

// Returns true when a function's entire body is a single 'return { ... };' statement,
// marking it as a return-block function (to be inlined at every call site).
static bool IsReturnBlockFunction(CFlatParser::FunctionDefinitionContext* func)
{
    auto* cs = func->compoundStatement();
    if (!cs) return false;
    auto* bil = cs->blockItemList();
    if (!bil) return false;
    auto items = bil->blockItem();
    if (items.size() != 1) return false;
    auto* stmt = items[0]->statement();
    if (!stmt) return false;
    auto* jump = stmt->jumpStatement();
    if (!jump) return false;
    return jump->Return() != nullptr && jump->compoundStatement() != nullptr;
}

// The grammar nests `arrayDimSpec` and the illegal trailing-'*' `arrayPtrSuffix` under a
// shared `arrayTypeSuffix` wrapper (so the declaration and cast/sizeof paths cannot drift).
// These unwrap it; both declarationSpecifier and abstractDeclarator expose arrayTypeSuffix().
// A null wrapper means no brackets at all; arrayDimSpec is required when the wrapper exists.
template <class Ctx>
static CFlatParser::ArrayDimSpecContext* ArrayDimsOf(Ctx* c)
{
    return c->arrayTypeSuffix() ? c->arrayTypeSuffix()->arrayDimSpec() : nullptr;
}
template <class Ctx>
static CFlatParser::ArrayPtrSuffixContext* ArrayPtrOf(Ctx* c)
{
    return c->arrayTypeSuffix() ? c->arrayTypeSuffix()->arrayPtrSuffix() : nullptr;
}
// `long long` reaches the listener as two separate `long` typeSpecifiers (the grammar has no
// combined rule), and the parse loops stop at the first one. Count them so the spelling can be
// canonicalized to "i64": `long long` is 64-bit on every target, while bare `long` is the
// target's native C long (i32 on Windows/LLP64, i64 on LP64) and keeps its own type name.
// Returns the type name to use for a "long" typeSpecifier given the whole specifier list.
static const char* LongSpellingTypeName(int longSpecifierCount)
{
    return longSpecifierCount >= 2 ? "i64" : "long";
}
// An empty `[]` is representable ONLY as the sole dimension: the `T[]` array-view is a thin
// `ptr` and carries no row stride, so `T[][]`, `T[][M]` and `T[N][]` have no lowering. The
// grammar folds every bracket pair into one arrayDimSpec and drops the empty ones from
// assignmentExpression(), so an unsized dimension is visible only as a bracket-count mismatch.
static bool DimSpecIsUnsizedMultiDim(CFlatParser::ArrayDimSpecContext* dims)
{
    return dims != nullptr && dims->LeftBracket().size() > 1
        && dims->assignmentExpression().size() < dims->LeftBracket().size();
}
template <class Ctx>
static bool HasUnsizedMultiDim(Ctx* c)
{
    return DimSpecIsUnsizedMultiDim(ArrayDimsOf(c));
}
// One wording for every site that rejects an unsized multi-dimensional bracket form.
static std::string UnsizedMultiDimMessage(const std::string& typeName)
{
    return std::format("'{0}' with an empty '[]' among several dimensions ('{0}[][]', '{0}[][M]', "
        "'{0}[N][]') is not a valid type - a '{0}[]' array view is a thin pointer and carries no "
        "row stride. Write a single '{0}[]' for a flat view of the whole allocation, or size "
        "every dimension ('{0}[N][M]'); a row of a sized 2-D array binds to a '{0}[]' one at a "
        "time.", typeName);
}
// A type-argument position (tuple element, generic type arg) admits exactly the bare `T[]`
// array-view among the bracket forms. Returns true for `T[]` (one empty pair, no trailing '*').
template <class Ctx>
static bool IsArrayViewArg(Ctx* c)
{
    auto* dims = ArrayDimsOf(c);
    return dims != nullptr && dims->assignmentExpression().empty() && ArrayPtrOf(c) == nullptr
        && dims->LeftBracket().size() == 1;
}
// The bracket forms that are NOT valid in a type-argument position: a sized `T[N]`, an unsized
// multi-dim `T[][]`, or a pointer-to-array `T[]*` / `T[N]*`. One diagnostic covers them.
template <class Ctx>
static bool IsBadArrayArg(Ctx* c)
{
    if (ArrayPtrOf(c) != nullptr) return true;
    auto* dims = ArrayDimsOf(c);
    if (DimSpecIsUnsizedMultiDim(dims)) return true;
    return dims != nullptr && !dims->assignmentExpression().empty();
}
// Type-arg string for one tuple element: the bare type plus its reference-kind suffix
// ("*" pointer, "[]" noalias array-view). Bad bracket forms (`[N]`, `[]*`) contribute no
// suffix here - the authoritative tuple-sugar path rejects them with a diagnostic.
// `compiler` namespace-resolves the element type the same way a generic type ARGUMENT is
// resolved, so the `(Item, int)` sugar and an explicit `tuple<Item, int>` keep mangling to one
// name inside a namespace that declares its own Item.
static std::string TupleEntryArgName(const LLVMBackend* compiler, CFlatParser::TupleTypeEntryContext* entry)
{
    std::string argName = compiler != nullptr
        ? compiler->ResolveTypeArgBaseName(entry->typeSpecifier()->getText())
        : entry->typeSpecifier()->getText();
    if (entry->pointer() != nullptr) argName += "*";
    else if (IsArrayViewArg(entry)) argName += "[]";
    return argName;
}

inline std::string getOperatorName(CFlatParser::OperatorFunctionIdContext* opId)
{
    if (opId->New())          return "operator new";
    if (opId->Delete())       return "operator delete";
    if (opId->String())       return "operator string";
    if (opId->Plus())         return "operator+";
    if (opId->Minus())        return "operator-";
    if (opId->Star())         return "operator*";
    if (opId->Div())          return "operator/";
    if (opId->Mod())          return "operator%";
    if (opId->Equal())        return "operator==";
    if (opId->NotEqual())     return "operator!=";
    if (opId->Less())         return "operator<";
    if (opId->LessEqual())    return "operator<=";
    if (opId->GreaterEqual()) return "operator>=";
    if (opId->LeftShift())    return "operator<<";
    // '>>' is two Greater tokens; '>' is one. Check before single-Greater.
    if (opId->Greater().size() == 2) return "operator>>";
    if (opId->Greater().size() == 1) return "operator>";
    if (opId->LeftBracket())  return "operator[]";
    if (opId->Arrow())        return "operator->";
    if (opId->Not())          return "operator!";
    if (opId->Tilde())        return "operator~";
    return "";
}

inline std::string getInterfaceMethodName(CFlatParser::InterfaceMethodContext* m)
{
    if (auto* opId = m->operatorFunctionId())
        return getOperatorName(opId);
    return m->directDeclarator()->getText();
}

// The compiler's ENTIRE lock vocabulary: the capability interfaces it knows how to lower a
// `lock (...)` statement against, and the acquire/release method roles within each. No lock
// TYPE is ever named here - a type opts in with [Capability(...)] in source (see
// core/interfaces.cb). Mode is the `.read` / `.write` suffix on the lock argument; "" and
// "write" both mean exclusive. Renaming acquire()/release() across core is a one-row edit.
//
// IOptimisticLockable is deliberately NOT a row here: an optimistic read lowers to nothing
// (an ordinary method call taking a lambda), so it has no acquire/release pair for a scoped
// block to bracket. It is a CHECKING capability - see CapabilityForLockMode.
struct CapabilitySpec
{
    const char* Iface;
    const char* Acquire;
    const char* Release;
    const char* Mode;
};
static constexpr CapabilitySpec kCapabilities[] = {
    { "ILockable",       "acquire",      "release",      ""     },
    { "ISharedLockable", "acquire_read", "release_read", "read" },
};

// Quote-stripped text of one annotation argument. A string literal loses its surrounding
// quotes; any other token (Constant, Identifier) is returned verbatim.
inline std::string AnnotationArgOne(CFlatParser::AnnotationArgContext* arg)
{
    std::string raw = arg->getText();
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        return raw.substr(1, raw.size() - 2);
    return raw;
}

// Every quote-stripped argument of an annotation, in source order (empty when it has none).
inline std::vector<std::string> AnnotationArgTexts(CFlatParser::AnnotationContext* ann)
{
    std::vector<std::string> out;
    if (!ann->annotationArgList()) return out;
    for (auto* arg : ann->annotationArgList()->annotationArg())
        out.push_back(AnnotationArgOne(arg));
    return out;
}

// The FIRST argument of an annotation (empty if it has none). Single-arg annotations
// ([uuid], [JsonName], [MaxLength]) read through this; multi-arg ones read Values.
inline std::string AnnotationArgText(CFlatParser::AnnotationContext* ann)
{
    auto args = AnnotationArgTexts(ann);
    return args.empty() ? std::string{} : args.front();
}

// Extract every [Name(arg, ...)] as a raw annotation value, WITHOUT registry validation. Used by
// the forward scan to record type-level annotations (e.g. [uuid]) before the main pass validates.
inline std::vector<LLVMBackend::AnnotationValue> ExtractAnnotations(CFlatParser::AnnotationListContext* annList)
{
    std::vector<LLVMBackend::AnnotationValue> result;
    if (!annList) return result;
    for (auto* ann : annList->annotation())
    {
        if (!ann->Identifier()) continue;
        auto args = AnnotationArgTexts(ann);
        LLVMBackend::AnnotationValue av;
        av.Name = ann->Identifier()->getText();
        av.Value = args.empty() ? std::string{} : args.front();
        av.Values = std::move(args);
        result.push_back(std::move(av));
    }
    return result;
}

// Extract the plain identifier from a directDeclarator (handles both bare names and C-style array syntax).
static std::string getDirectDeclName(CFlatParser::DirectDeclaratorContext* d)
{
    if (!d) return "";
    // C-style array form: (Identifier | Move) '[' assignmentExpression ']'
    if (d->assignmentExpression())
        return d->Identifier() ? d->Identifier()->getText() : "move";
    return d->getText();
}

// Normalize a generic type argument for use in mangled names (e.g. "Employee*" -> "Employeeptr",
// "int[]" -> "intview"). A type-arg string carries its reference kind as a suffix: "*" for a
// pointer, "[]" for a noalias array-view (see PeelTypeArgSuffix). Both must fold to symbol-safe
// text so the mangled name stays a valid identifier.
// The `unique` ownership qualifier is carried as a leading "unique " prefix on a resolved
// type-argument string (decision D10: single space, star attached, e.g. "unique Circle*").
static const char kUniqueQualifierPrefix[] = "unique ";
static const size_t kUniqueQualifierPrefixLen = sizeof(kUniqueQualifierPrefix) - 1;

// True if a type-parameter entry carries the `unique` qualifier: a leading Identifier
// (grammar: `Identifier? typeSpecifier ...`) whose text is exactly "unique".
static bool TypeArgHasUnique(CFlatParser::TypeParameterEntryContext* entry)
{
    return entry != nullptr && entry->Identifier() != nullptr
        && entry->Identifier()->getText() == "unique";
}

// Strip a leading "unique " qualifier off a resolved type-arg string, returning whether it
// was present. The bare base (e.g. "Circle*") remains so it resolves to the same LLVM type
// as the unqualified spelling; is_unique consults the un-stripped substitution string.
static bool StripUniqueQualifier(std::string& name)
{
    if (name.compare(0, kUniqueQualifierPrefixLen, kUniqueQualifierPrefix) == 0)
    {
        name.erase(0, kUniqueQualifierPrefixLen);
        return true;
    }
    return false;
}

// The `alias` borrow qualifier on a generic type argument is carried the same way as `unique`:
// a leading "alias " prefix on the resolved type-arg string. It denotes a pure borrow element
// (list<alias T*>) - behaviorally identical to a bare pointer element, but instantiated
// distinctly so accessor returns can be tagged for the delete-of-borrow check.
static const char kAliasQualifierPrefix[] = "alias ";
static const size_t kAliasQualifierPrefixLen = sizeof(kAliasQualifierPrefix) - 1;

// True if a type-parameter entry carries the `alias` qualifier: a leading Identifier
// (grammar: `Identifier? typeSpecifier ...`) whose text is exactly "alias".
static bool TypeArgHasAlias(CFlatParser::TypeParameterEntryContext* entry)
{
    return entry != nullptr && entry->Identifier() != nullptr
        && entry->Identifier()->getText() == "alias";
}

// Strip a leading "alias " qualifier off a resolved type-arg string, returning whether it was
// present. The bare base (e.g. "Circle*") remains so it resolves to the same LLVM type as the
// unqualified spelling; the alias prefix only drives distinct monomorphization + borrow tagging.
static bool StripAliasQualifier(std::string& name)
{
    if (name.compare(0, kAliasQualifierPrefixLen, kAliasQualifierPrefix) == 0)
    {
        name.erase(0, kAliasQualifierPrefixLen);
        return true;
    }
    return false;
}

// Strip both leading ownership qualifiers (`unique `, `alias `) off a resolved type-arg string.
// Neither is a signature/LLVM type - they resolve to the same underlying type - so callers that
// only want the bare base drop both. Order is irrelevant (the two are mutually exclusive).
static void StripOwnershipQualifiers(std::string& name)
{
    StripUniqueQualifier(name);
    StripAliasQualifier(name);
}

// CanonicalPrimitiveSpelling lives in LLVMBackend.h - the SAME map has to serve both type
// identities (generic instantiation here, overload symbol names in ToUniqueString) or one
// spelling of a type monomorphizes differently from how it overloads.
// `compiler` is REQUIRED (no default): it supplies the pure-rename `using` alias fold, and a site
// that silently skipped it would mangle a name the other pass spells differently. Every current
// call site reaches one; a nullptr is only correct where the string cannot name a source alias.
static std::string MangleTypeArg(const LLVMBackend* compiler, const std::string& typeName)
{
    std::string result;
    size_t start = 0;
    // The `unique` qualifier (D10) mangles to a "unique_" token so list<unique Circle*>
    // monomorphizes distinctly from list<Circle*>.
    if (typeName.compare(0, kUniqueQualifierPrefixLen, kUniqueQualifierPrefix) == 0)
    {
        result += "unique_";
        start = kUniqueQualifierPrefixLen;
    }
    // The `alias` qualifier mangles to an "alias_" token so list<alias Circle*>
    // monomorphizes distinctly from list<Circle*> (mutually exclusive with unique).
    else if (typeName.compare(0, kAliasQualifierPrefixLen, kAliasQualifierPrefix) == 0)
    {
        result += "alias_";
        start = kAliasQualifierPrefixLen;
    }
    // Canonicalize the BASE only - everything before the first reference-kind suffix - and by
    // exact match, so a struct named `Int` or a type named `integer` is never rewritten. A
    // nested generic arrives already mangled (list<int> -> "list__i32"), since the inner name
    // was built through this same funnel.
    size_t baseEnd = start;
    while (baseEnd < typeName.size() && typeName[baseEnd] != '*' && typeName[baseEnd] != '[')
        baseEnd++;
    // A pure-rename `using` alias is transparent, so fold it BEFORE canonicalizing: MyInt -> int
    // -> i32, which is what makes list<MyInt> and list<int> one instantiation. Only the BASE is
    // folded; the suffix walk below owns the reference-kind decoration either way.
    std::string base = typeName.substr(start, baseEnd - start);
    if (compiler != nullptr) base = compiler->ResolveManglingAlias(base);
    result += CanonicalPrimitiveSpelling(base);

    for (size_t i = baseEnd; i < typeName.size(); i++)
    {
        if (typeName[i] == '*') result += "ptr";
        else if (typeName[i] == '[' && i + 1 < typeName.size() && typeName[i + 1] == ']')
        {
            result += "view";
            i++;  // consume the matching ']'
        }
        else result += typeName[i];
    }
    return result;
}

// A `using` target that is nothing but a (possibly dotted) identifier - the only shape that is a
// PURE RENAME and so may be folded by MangleTypeArg. Anything carrying '*', '[', '<' or '(' stores
// that structure in the alias string and is unfolded separately at GetType /
// ParseDeclarationSpecifiers; folding it into the base would double the mangler's suffix walk.
static bool IsBareTypeName(const std::string& s)
{
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '.')) return false;
    return true;
}

// Namespace-resolve a type-argument string that carries a reference-kind suffix ("Item*",
// "Item[]"): only the bare core participates in the walk. For the two scanner paths that build the
// arg string from a raw getText(); paths that keep the core separate call
// LLVMBackend::ResolveTypeArgBaseName directly. No ownership-prefix handling: getText() has no
// whitespace, so a `unique`/`alias` arg arrives as "uniqueItem*" and never reaches here as a
// prefixed spelling (those two are routed through ResolveForwardTypeArg instead).
static std::string ResolveTypeArgSpelling(const LLVMBackend* compiler, const std::string& arg)
{
    if (compiler == nullptr || arg.empty()) return arg;
    size_t end = arg.size();
    while (end > 0 && (arg[end - 1] == '*' || arg[end - 1] == '[' || arg[end - 1] == ']'))
        end--;
    if (end == 0) return arg;
    std::string core = arg.substr(0, end);
    std::string resolved = compiler->ResolveTypeArgBaseName(core);
    if (resolved == core) return arg;
    return resolved + arg.substr(end);
}

// The generic instantiation written at `ts`, covering BOTH spellings: the bare `Box<int>`
// (genericIdentifier) and the namespace-qualified `Windows.Foundation.IReference<int>`
// (qualifiedGenericIdentifier). `base` receives the base name, dotted when it was written dotted.
// Returns the <...> argument list, or nullptr when `ts` is not a generic instantiation (in which
// case callers fall through to their plain-type path, which reads getText()).
static CFlatParser::GenericTypeParametersContext* GenericSpecOf(
    CFlatParser::TypeSpecifierContext* ts, std::string& base)
{
    base.clear();
    if (ts == nullptr) return nullptr;
    if (auto* q = ts->qualifiedGenericIdentifier())
    {
        for (auto* id : q->Identifier())
        {
            if (!base.empty()) base += ".";
            base += id->getText();
        }
        return q->genericTypeParameters();
    }
    if (auto* g = ts->genericIdentifier())
    {
        if (g->Identifier() == nullptr) return nullptr;
        base = g->Identifier()->getText();
        return g->genericTypeParameters();
    }
    return nullptr;
}

// Build the symbol-safe encoded name for a closure type used as a generic argument (gap a).
// Length-prefixed components keep it collision-free; the result contains only [A-Za-z0-9_].
// Number of '*' written on a pointer() context. The grammar is ('*' typeQualifierList?)+, so the
// depth is Star().size(); the funcptr signature sites used to collapse it to a bool.
static int PointerDepthOf(CFlatParser::PointerContext* p)
{
    return p == nullptr ? 0 : (int)p->Star().size();
}

// Reconcile a written '*' count with the pointer flag ResolveSigComponent* may have CHANGED - a
// `string` component resolves to char* with no '*' written. The FLAG wins; the count only adds
// precision when the two agree.
static int ReconcilePointerDepth(bool pointer, int stars)
{
    if (!pointer) return 0;
    return stars > 0 ? stars : 1;
}

// Each component folds its pointer DEPTH in via MangleTypeArg ("int*" -> "intptr", "int**" ->
// "intptrptr"); isThin selects the thin/fat prefix. Examples: Lambda<int(int)> ->
// "__fatfn_1_3_i32_3_i32"; function<void(UiTest*)> -> "__thinfn_1_4_void_9_UiTestptr";
// Lambda<list<string>()> -> "__fatfn_0_12_list__string". Both compiler passes MUST call this
// identically. Depths are ints, so a caller that only knows "is a pointer" passes 0 or 1 and gets
// the string this produced before depth existed.
static std::string BuildEncodedClosureName(const LLVMBackend* compiler, bool isThin,
    const std::string& ret, int retDepth,
    const std::vector<std::pair<std::string, int>>& params)
{
    auto comp = [compiler](const std::string& name, int depth) {
        std::string c = MangleTypeArg(compiler, name + std::string(depth < 0 ? 0 : depth, '*'));
        return std::to_string(c.size()) + "_" + c;
    };
    std::string s = isThin ? "__thinfn" : "__fatfn";
    s += "_" + std::to_string(params.size());
    s += "_" + comp(ret, retDepth);
    for (const auto& p : params) s += "_" + comp(p.first, p.second);
    return s;
}


// Peel the reference-kind suffixes off a (possibly substituted) type-arg string, recording them
// as flags: a trailing "[]" is a noalias array-view (which is also a pointer repr), a trailing "*"
// is a plain pointer. They never combine ("T[]*" is rejected at the grammar/listener). Suffixes
// were appended in source order, so peel from the right until none remain. Returns the bare base
// type name in `name`.
static void PeelTypeArgSuffix(std::string& name, bool& pointer, bool& arrayView,
    bool* unique = nullptr, bool* aliasOut = nullptr)
{
    // A `unique` or `alias` qualifier is a leading prefix; peel it first so the suffix loop and
    // any re-mangling see the bare type. Callers resolving to an LLVM type discard it; callers
    // that re-mangle a nested arg re-prepend it from *unique / *aliasOut. The two are exclusive.
    bool hadUnique = StripUniqueQualifier(name);
    if (unique != nullptr) *unique = hadUnique;
    bool hadAlias = StripAliasQualifier(name);
    if (aliasOut != nullptr) *aliasOut = hadAlias;
    for (;;)
    {
        if (name.size() >= 2 && name.compare(name.size() - 2, 2, "[]") == 0)
        {
            arrayView = true;
            pointer = true;
            name.erase(name.size() - 2);
        }
        else if (!name.empty() && name.back() == '*')
        {
            pointer = true;
            name.pop_back();
        }
        else
            break;
    }
}

// Peel trailing '*' characters off a resolved type-alias string (e.g. "void*" -> "void"),
// returning the pointer depth removed. A pointer alias keeps its stars in the stored string
// (no type-descriptor struct); this re-derives the depth at the resolution site so it can be
// OR'd onto the flat TypeAndValue.Pointer / .ElemPointer flags.
static int PeelAliasPointerStars(std::string& name)
{
    int depth = 0;
    while (!name.empty() && name.back() == '*') { name.pop_back(); depth++; }
    return depth;
}

// Strip a trailing fixed-array suffix ("[3]" / "[3][4]") off a resolved alias string into `dims`
// (outer dimension first). The sizes were constant-folded into digit literals at registration,
// so only `[<digits>]` is accepted. Returns true if any bracket was peeled. Call BEFORE
// PeelAliasPointerStars so "int*[3]" yields dims {3} over a base "int*".
static bool PeelAliasArrayDims(std::string& name, std::vector<uint64_t>& dims)
{
    std::vector<uint64_t> rev;  // collected inner-to-outer (rightmost bracket first)
    while (!name.empty() && name.back() == ']')
    {
        size_t open = name.rfind('[');
        if (open == std::string::npos) break;
        std::string num = name.substr(open + 1, name.size() - open - 2);
        if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) break;
        rev.push_back(std::strtoull(num.c_str(), nullptr, 10));
        name.erase(open);
    }
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) dims.push_back(*it);
    return !dims.empty();
}

// Struct/class bodies parse through the named `aggregateMember` rule (see CFlat.g4), so the
// per-kind children (declarations, methods, ...) are nested one level under aggregateMember
// rather than being direct children of the struct/class context. These helpers flatten an
// aggregate's members back into the per-kind vectors the listener expects, preserving source
// Collect all members of an already-resolved member list for which the getter returns non-null.
// The list is resolved (member-scope `if const` branches spliced in) by each pass's own
// ResolveAggregateMembers, since the two passes use different branch-selection policies.
template <typename Result, typename Member, typename Getter>
static std::vector<Result*> MemberFilter(const std::vector<Member*>& members, Getter g)
{
    std::vector<Result*> out;
    for (auto* m : members)
        if (auto* x = g(m)) out.push_back(x);
    return out;
}

// The per-kind flatteners (MemberDeclarations, MemberFunctionDefinitions, ...) are defined as
// member functions of ForwardRefScanner and of MainListener via this macro, so unqualified call
// sites inside each class pick up that pass's own ResolveAggregateMembers.
#define CFLAT_DEFINE_MEMBER_FLATTENERS()                                                                                                                                              \
    template <typename TCtx> auto MemberDeclarations(TCtx* ctx)        { return MemberFilter<CFlatParser::DeclarationContext>         (ResolveAggregateMembers(ctx), [](auto* m){ return m->declaration();          }); } \
    template <typename TCtx> auto MemberFunctionDefinitions(TCtx* ctx) { return MemberFilter<CFlatParser::FunctionDefinitionContext>  (ResolveAggregateMembers(ctx), [](auto* m){ return m->functionDefinition();   }); } \
    template <typename TCtx> auto MemberDestructorDefinitions(TCtx* ctx){ return MemberFilter<CFlatParser::DestructorDefinitionContext>(ResolveAggregateMembers(ctx), [](auto* m){ return m->destructorDefinition(); }); } \
    template <typename TCtx> auto MemberStructDefinitions(TCtx* ctx)   { return MemberFilter<CFlatParser::StructDefinitionContext>    (ResolveAggregateMembers(ctx), [](auto* m){ return m->structDefinition();     }); } \
    template <typename TCtx> auto MemberClassDefinitions(TCtx* ctx)    { return MemberFilter<CFlatParser::ClassDefinitionContext>     (ResolveAggregateMembers(ctx), [](auto* m){ return m->classDefinition();      }); } \
    template <typename TCtx> auto MemberLockFieldGroups(TCtx* ctx)     { return MemberFilter<CFlatParser::LockFieldGroupContext>      (ResolveAggregateMembers(ctx), [](auto* m){ return m->lockFieldGroup();       }); }

// Base-clause interface identifiers of a class definition. Only `classDefinition` carries a base
// clause in the grammar, so the struct overload answers empty and keeps the scan templated.
inline std::vector<CFlatParser::BaseSpecifierContext*> BaseClauseIdentifiers(CFlatParser::ClassDefinitionContext* ctx) { return ctx->baseSpecifier(); }
inline std::vector<CFlatParser::BaseSpecifierContext*> BaseClauseIdentifiers(CFlatParser::StructDefinitionContext*)    { return {}; }

// The dotted name a base-clause entry spells, without its generic type arguments:
// `IS` -> "IS", `shapes.IS` -> "shapes.IS". Empty when the entry has no identifier.
inline std::string BaseSpecifierName(CFlatParser::BaseSpecifierContext* spec)
{
    if (spec == nullptr) return {};
    std::string name;
    for (auto* id : spec->Identifier())
        name += (name.empty() ? "" : ".") + id->getText();
    return name;
}

/*
 * Every name a base-clause spelling could be registered under, appended to `out`. The resolver
 * answers one name once the interface is registered; during the forward scan an interface
 * declared later in the same namespace is not yet visible, so the enclosing-namespace
 * candidates are added too. The static conversion check only ever PROVES impossibility, so a
 * surplus candidate can weaken a proof but can never cause a false rejection.
 */
inline void AppendInterfaceNameCandidates(LLVMBackend* compiler, const std::string& namespaceName,
                                          const std::string& spelled, std::vector<std::string>& out)
{
    if (spelled.empty()) return;
    std::string resolved = compiler->ResolveInterfaceName(spelled);
    out.push_back(resolved);
    if (spelled.find('.') != std::string::npos || namespaceName.empty()) return;
    std::string prefix = namespaceName;
    while (true)
    {
        std::string candidate = prefix + "." + spelled;
        if (candidate != resolved) out.push_back(candidate);
        auto dot = prefix.rfind('.');
        if (dot == std::string::npos) break;
        prefix = prefix.substr(0, dot);
    }
}

// "path(line,col)" of a definition, used both as the IDENTITY key that tells a genuine
// redefinition apart from the same definition being registered twice (forward scan then codegen
// walk, or a re-imported file reached via a different path spelling) and, reformatted for
// display, as the location named in the "already defined at" diagnostic. The path here is the
// canonical full path (DefinitionSitePath()), not the basename: two co-imported files sharing a
// basename (e.g. "da/common.cb" and "db/common.cb") must compare unequal, while the SAME file
// reached via two spellings (relative vs absolute, a different -i dir, ../) must compare equal -
// import resolution already canonicalizes every import target before this runs, so one file has
// exactly one canonical path regardless of spelling. See ShortenDefSiteForDisplay() for the
// user-facing form.
inline std::string DefinitionSiteText(LLVMBackend* compiler, antlr4::ParserRuleContext* ctx)
{
    if (compiler == nullptr || ctx == nullptr || ctx->getStart() == nullptr) return {};
    return std::format("{}({},{})", compiler->DefinitionSitePath(),
                       (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine());
}

// Source text of a parse subtree with runs of whitespace collapsed to one space, so a condition
// reads as written instead of as getText()'s token concatenation. Length-bounded per condition;
// the composed nesting chain is bounded again where the diagnostic assembles it.
inline std::string CollapsedSourceText(antlr4::ParserRuleContext* ctx)
{
    if (ctx == nullptr) return {};
    auto* startTok = ctx->getStart();
    auto* stopTok = ctx->getStop();
    if (startTok == nullptr || stopTok == nullptr) return {};
    auto* input = startTok->getInputStream();
    if (input == nullptr) return ctx->getText();
    std::string raw = input->getText(
        antlr4::misc::Interval(startTok->getStartIndex(), stopTok->getStopIndex()));
    std::string out;
    bool pendingSpace = false;
    for (char c : raw)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pendingSpace = true; continue; }
        if (pendingSpace && !out.empty()) out += ' ';
        pendingSpace = false;
        out += c;
    }
    if (out.empty()) return ctx->getText();
    return TruncateDiagnosticText(std::move(out), kIfConstConditionTextLimit);
}

// The `if const (...)` condition governing this node, if the node is itself an `if const` at file,
// member, or interface scope. Empty when it is any other kind of node.
inline std::string IfConstConditionText(antlr4::tree::ParseTree* node)
{
    if (auto* d = dynamic_cast<CFlatParser::IfConstDeclarationContext*>(node))
        return CollapsedSourceText(d->expression());
    if (auto* m = dynamic_cast<CFlatParser::IfConstMemberContext*>(node))
        return CollapsedSourceText(m->expression());
    if (auto* i = dynamic_cast<CFlatParser::IfConstInterfaceMemberContext*>(node))
        return CollapsedSourceText(i->expression());
    return {};
}

// True for the brace block of one `if const` arm. Both the then arm and a plain `else` arm are
// children of the SAME `if const` node, so the arms can only be told apart by their order here.
// A chained `else if const` is not a block - it is a nested node that carries its own condition.
inline bool IsIfConstArmBlock(antlr4::tree::ParseTree* node)
{
    return dynamic_cast<CFlatParser::IfConstBlockContext*>(node) != nullptr
        || dynamic_cast<CFlatParser::IfConstMemberBlockContext*>(node) != nullptr
        || dynamic_cast<CFlatParser::IfConstInterfaceBlockContext*>(node) != nullptr;
}

// Dotted name of a namespace definition, used to qualify class names recorded from inside one.
inline std::string NamespaceDefinitionName(CFlatParser::NamespaceDefinitionContext* ctx)
{
    std::string name;
    for (auto* id : ctx->Identifier())
        name += (name.empty() ? "" : ".") + id->getText();
    return name;
}

inline std::string JoinQualified(const std::string& outer, const std::string& inner)
{
    if (inner.empty()) return outer;
    return outer.empty() ? inner : outer + "." + inner;
}

/*
 * A class inside an `if const` block is invisible to ForwardRefScanner - the taken branch is a
 * MainListener fact - so it never reaches scannedInterfaceImpls and no conversion may be proven
 * impossible against the interfaces it names. Walk the whole subtree (nested namespaces, nested
 * `if const`, else arms) and mark every base-clause interface uncertain instead. The qualified
 * class name, its parse node, and the CHAIN of levels it sits under (outermost first) are recorded
 * alongside, purely so the diagnostic can explain itself. A level is an arm block, or the else path
 * of a chained `else if const`. No level is known to be untaken here - MainListener retracts or
 * peels the entry (RetractIfConstArmGuardedImpls) as it takes each one.
 */
inline void MarkIfConstClassImplsUncertain(LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                           const std::string& namespaceName = {},
                                           const std::vector<std::string>& guardChain = {},
                                           const std::string& qualifier = {},
                                           bool inGenericTemplate = false)
{
    if (compiler == nullptr || node == nullptr) return;
    std::string scope = qualifier;
    std::string nsScope = namespaceName;
    if (auto* ns = dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(node))
    {
        scope = JoinQualified(scope, NamespaceDefinitionName(ns));
        nsScope = JoinQualified(nsScope, NamespaceDefinitionName(ns));
    }
    bool childGeneric = inGenericTemplate;
    if (auto* cls = dynamic_cast<CFlatParser::ClassDefinitionContext*>(node))
    {
        // Suppression takes EVERY candidate (a surplus one only weakens a proof), while blame keeps
        // them grouped per base entry so the blame site can pick the one name each entry resolves to.
        std::vector<std::vector<std::string>> named;
        for (auto* spec : BaseClauseIdentifiers(cls))
        {
            std::string spelled = BaseSpecifierName(spec);
            if (spelled.empty()) continue;
            // Candidates come from the NAMESPACE-only scope: the resolver never walks class-name
            // prefixes, so a class-rung candidate is a name the class could never register against.
            std::vector<std::string> candidates;
            AppendInterfaceNameCandidates(compiler, nsScope, spelled, candidates);
            for (const auto& c : candidates)
                compiler->RecordUncertainInterfaceImpl(c);
            // Last component of a qualified spelling: SUPPRESSION only - resolving blame through
            // it fabricates an implements-claim against an unrelated same-named interface.
            if (auto dot = spelled.rfind('.'); dot != std::string::npos)
                compiler->RecordUncertainInterfaceImpl(spelled.substr(dot + 1));
            named.push_back(std::move(candidates));
        }
        // A class inside a generic TEMPLATE body is reconciled once per instantiation - zero times
        // when the template is never used - and the peel below is not idempotent, so never blame it.
        if (auto* dd = cls->directDeclarator())
        {
            scope = JoinQualified(scope, dd->getText());
            if (!inGenericTemplate)
                compiler->RecordIfConstGuardedInterfaceImpl(scope, guardChain, (const void*)cls,
                                                            std::move(named));
        }
        if (cls->genericTypeParameters() != nullptr) childGeneric = true;
    }
    else if (auto* st = dynamic_cast<CFlatParser::StructDefinitionContext*>(node))
    {
        if (auto* dd = st->directDeclarator()) scope = JoinQualified(scope, dd->getText());
        if (st->genericTypeParameters() != nullptr) childGeneric = true;
    }

    std::string cond = IfConstConditionText(node);
    size_t armIndex = 0;
    for (auto* child : node->children)
    {
        // APPEND, never replace: a class under `if const (A) { if const (B) { ... } }` sits under
        // both arms, and dropping A would leave the message blaming a guard that may well hold.
        std::vector<std::string> childChain = guardChain;
        if (!cond.empty() && IsIfConstArmBlock(child))
            childChain.push_back(armIndex++ == 0
                ? std::format("an 'if const ({})' branch", cond)
                : std::format("the else arm of an 'if const ({})'", cond));
        // A chained `else if const` is a nested node, not an arm block, but it is still a LEVEL:
        // reaching it means this node's own arm was rejected, and that fact must stay visible.
        else if (!cond.empty() && !IfConstConditionText(child).empty())
            childChain.push_back(std::format("the else path of an 'if const ({})'", cond));
        MarkIfConstClassImplsUncertain(compiler, child, nsScope, childChain, scope, childGeneric);
    }
}

/*
 * MainListener decided to TAKE this arm, so the diagnostic must never claim its classes are absent
 * because of it. A class sitting directly in the arm is live - retract it outright. A class behind
 * a FURTHER nested `if const` only loses this one level: that inner arm gets its own decision, and
 * is retracted or peeled in turn when MainListener reaches it. Callers that descend into a chained
 * `else if const` pass nested=true, since taking the else PATH is one level and nothing more.
 */
inline void RetractIfConstArmGuardedImpls(LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                          bool nested = false)
{
    if (compiler == nullptr || node == nullptr) return;
    if (auto* cls = dynamic_cast<CFlatParser::ClassDefinitionContext*>(node))
    {
        if (nested) compiler->PeelIfConstGuardedInterfaceImpl((const void*)cls);
        else        compiler->RetractIfConstGuardedInterfaceImpl((const void*)cls);
    }
    for (auto* child : node->children)
        RetractIfConstArmGuardedImpls(compiler, child,
                                      nested || !IfConstConditionText(child).empty());
}

/*
 * The walk of this subtree was ABANDONED - LogError threw and an `expect_error` block swallowed it -
 * so no `if const` inside it was ever decided. Nothing here may be claimed untaken: drop every entry
 * outright rather than peel, since a peel would leave a front level the walk never even reached.
 */
inline void ForgetIfConstGuardedImpls(LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (compiler == nullptr || node == nullptr) return;
    if (auto* cls = dynamic_cast<CFlatParser::ClassDefinitionContext*>(node))
        compiler->RetractIfConstGuardedInterfaceImpl((const void*)cls);
    for (auto* child : node->children)
        ForgetIfConstGuardedImpls(compiler, child);
}

// Interface bodies parse through the named `interfaceMember` rule, so methods and fields are nested
// one level under it. Same macro shape as the aggregate flatteners: each pass gets its own copy so
// the unqualified ResolveInterfaceMembers call binds to that pass's branch-selection policy.
#define CFLAT_DEFINE_INTERFACE_FLATTENERS()                                                                                                                                           \
    std::vector<CFlatParser::InterfaceMethodContext*> InterfaceMethods(CFlatParser::InterfaceDefinitionContext* ctx) { return MemberFilter<CFlatParser::InterfaceMethodContext>(ResolveInterfaceMembers(ctx), [](auto* m){ return m->interfaceMethod(); }); } \
    std::vector<CFlatParser::InterfaceFieldContext*>  InterfaceFields(CFlatParser::InterfaceDefinitionContext* ctx)  { return MemberFilter<CFlatParser::InterfaceFieldContext> (ResolveInterfaceMembers(ctx), [](auto* m){ return m->interfaceField();  }); }

// simd<T,N>: validate the lane count N. It must be a plain power-of-2 integer literal in [2,64]
// (the explicit SIMD type is the hardware-control escape hatch, so a non-power-of-2 that would
// silently waste a lane is rejected rather than padded). Returns true and sets lanesOut on
// success; otherwise sets errorOut and returns false.
static bool TryParseSimdLaneCount(const std::string& text, uint64_t& lanesOut, std::string& errorOut)
{
    uint64_t v = 0;
    try
    {
        size_t pos = 0;
        v = std::stoull(text, &pos, 0);   // base 0 also accepts 0x.. forms
        if (pos != text.size())
            throw std::invalid_argument("trailing characters");
    }
    catch (...)
    {
        errorOut = std::format("simd lane count must be an integer literal (got '{}')", text);
        return false;
    }
    if (v < 2 || v > 64 || (v & (v - 1)) != 0)
    {
        uint64_t up = 2; while (up < v && up < 64) up <<= 1;
        uint64_t down = (up > 2) ? (up >> 1) : 2;
        if (down == up)
            errorOut = std::format("simd lane count must be a power of 2 in [2,64] (got {}); did you mean simd<...,{}>?", v, up);
        else
            errorOut = std::format("simd lane count must be a power of 2 in [2,64] (got {}); did you mean simd<...,{}> or simd<...,{}>?", v, down, up);
        return false;
    }
    lanesOut = v;
    return true;
}

/*
 * Record the declarator's pointer depth and array dimensions onto a simd<T,N> declared type.
 * The simd branch breaks out of the specifier loop before the common tail that does this for
 * every other type, so without it `simd<T,N>**` lost its second level and `simd<T,N>[N]`
 * silently lost its dimension (allocating one vector and turning `a[i]` into a LANE read).
 * Shared by BOTH ParseDeclarationSpecifiers copies (ForwardRefScanner and MainListener).
 */
static void RecordSimdPointerAndDims(LLVMBackend::DeclTypeAndValue& declType,
                                     CFlatParser::DeclarationSpecifierContext* declSpec)
{
    declType.Pointer = declSpec->pointer() != nullptr;
    if (declType.Pointer && declSpec->pointer()->Star().size() >= 2)
        declType.ElemPointer = true;
    // An empty '[]' on a simd type is left alone: simd array views are unimplemented, and
    // deducing one here would change a shape that currently compiles.
    if (auto* dimSpec = ArrayDimsOf(declSpec))
    {
        auto dims = dimSpec->assignmentExpression();
        if (!dims.empty())
        {
            declType.ArraySize = dims[0];
            for (size_t di = 1; di < dims.size(); di++)
                declType.ExtraArrayDims.push_back(dims[di]);
        }
    }
}

// Extract function name from FunctionDefinitionContext (handles operator overloads).
static std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx)
{
    if (auto* opId = ctx->operatorFunctionId())
        return ::getOperatorName(opId);
    auto directDecl = ctx->directDeclarator();
    return directDecl->getText();
}

// Build a readable one-line signature for a function definition by slicing the source
// from the function's start up to its body, then collapsing whitespace. Used for the
// symbol index of generic-template methods, which would otherwise record only the bare
// name (the monomorphized instance is what carries fully-resolved parameter types).
static std::string getFunctionSignatureText(CFlatParser::FunctionDefinitionContext* ctx)
{
    auto* body = ctx->compoundStatement();
    auto* startTok = ctx->getStart();
    if (!body || !startTok) return getFunctionName(ctx);
    auto* input = startTok->getInputStream();
    size_t a = startTok->getStartIndex();
    size_t b = body->getStart()->getStartIndex();
    if (!input || b <= a) return getFunctionName(ctx);

    std::string raw = input->getText(antlr4::misc::Interval(a, b - 1));
    std::string out;
    bool pendingSpace = false;
    for (char c : raw)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pendingSpace = true; continue; }
        if (pendingSpace && !out.empty()) out += ' ';
        pendingSpace = false;
        out += c;
    }
    return out.empty() ? getFunctionName(ctx) : out;
}

// Extract the leading doc comment block above the given declaration's start token.
// Tier 1 (preferred): contiguous /// or /** ... */ doc comments anchored to the declaration.
// Tier 2 (fallback): contiguous // or /* ... */ plain comments anchored to the declaration.
// "Anchored" means no blank line between the comment block and the declaration, and no
// blank line between adjacent comments in the chain.
// Returns markdown-ready text with comment markers stripped, or empty string on no match.
static std::string ExtractLeadingDoc(antlr4::BufferedTokenStream* tokens, antlr4::Token* declStart)
{
    if (!tokens || !declStart) return "";
    size_t idx = declStart->getTokenIndex();
    auto hidden = tokens->getHiddenTokensToLeft(idx);
    if (hidden.empty()) return "";

    struct Item { antlr4::Token* tok; int startLine; int endLine; bool isDoc; bool isLine; };
    auto countNewlines = [](const std::string& s) {
        int n = 0;
        for (char c : s) if (c == '\n') ++n;
        return n;
    };
    auto classify = [&](antlr4::Token* t) -> std::optional<Item> {
        const std::string& text = t->getText();
        if (text.size() < 2) return std::nullopt;
        Item it{};
        it.tok = t;
        it.startLine = (int)t->getLine();
        it.endLine = it.startLine + countNewlines(text);
        if (text.starts_with("//"))
        {
            it.isLine = true;
            // /// is a doc comment; //// or longer slash run is not.
            it.isDoc = text.size() >= 3 && text[2] == '/' && (text.size() < 4 || text[3] != '/');
            return it;
        }
        if (text.starts_with("/*"))
        {
            it.isLine = false;
            // /** ... */ is a doc comment; bare /**/ is not.
            it.isDoc = text.size() >= 3 && text[2] == '*' && text != "/**/";
            return it;
        }
        return std::nullopt;
    };

    // Walk hidden tokens right-to-left and collect comments that chain up to the
    // declaration with no blank line gap.
    std::vector<Item> chain;
    int nextExpectedEnd = (int)declStart->getLine() - 1;
    for (auto it = hidden.rbegin(); it != hidden.rend(); ++it)
    {
        auto opt = classify(*it);
        if (!opt) continue;
        Item item = *opt;
        if (item.endLine < nextExpectedEnd) break;     // blank-line gap broke the chain
        if (item.endLine > nextExpectedEnd) continue;  // comment trailing other code on this line; skip
        chain.push_back(item);
        nextExpectedEnd = item.startLine - 1;
    }
    if (chain.empty()) return "";
    std::reverse(chain.begin(), chain.end());

    // Tier 1: keep only the trailing contiguous run of doc comments (closest to decl).
    // Tier 2 (fallback): if no doc comments at all, keep the whole chain as plain.
    bool anyDoc = false;
    for (const auto& it : chain) if (it.isDoc) { anyDoc = true; break; }

    std::vector<Item> selected;
    if (anyDoc)
    {
        size_t first = chain.size();
        for (size_t i = chain.size(); i-- > 0; )
        {
            if (!chain[i].isDoc) break;
            first = i;
        }
        selected.assign(chain.begin() + first, chain.end());
    }
    else
    {
        selected = std::move(chain);
    }

    auto trimRight = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    };

    std::vector<std::string> outLines;
    for (const auto& it : selected)
    {
        const std::string& text = it.tok->getText();
        if (it.isLine)
        {
            size_t off = it.isDoc ? 3 : 2;  // strip /// or //
            std::string ln = (text.size() > off) ? text.substr(off) : std::string();
            if (!ln.empty() && ln.front() == ' ') ln.erase(0, 1);
            trimRight(ln);
            outLines.push_back(ln);
        }
        else
        {
            // Strip leading /* or /** and trailing */.
            size_t startOff = it.isDoc ? 3 : 2;
            size_t endOff = (text.size() >= 2 && text.compare(text.size() - 2, 2, "*/") == 0) ? 2 : 0;
            if (text.size() <= startOff + endOff) continue;
            std::string body = text.substr(startOff, text.size() - startOff - endOff);
            std::vector<std::string> lines;
            std::string cur;
            for (char c : body)
            {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            lines.push_back(cur);
            for (auto& ln : lines)
            {
                size_t j = 0;
                while (j < ln.size() && (ln[j] == ' ' || ln[j] == '\t')) ++j;
                if (j < ln.size() && ln[j] == '*')
                {
                    ++j;
                    if (j < ln.size() && ln[j] == ' ') ++j;
                }
                std::string clean = (j < ln.size()) ? ln.substr(j) : std::string();
                trimRight(clean);
                outLines.push_back(clean);
            }
        }
    }

    // Drop decorative divider lines (e.g. "-----", "=====", box-drawing rules) so a
    // description framed by dividers does not render as an oversized, contentless hover.
    auto isSeparatorLine = [](const std::string& s) {
        int visible = 0, sep = 0;
        for (unsigned char c : s)
        {
            if (c == ' ' || c == '\t') continue;
            ++visible;
            if (std::isalnum(c)) return false;  // any real text -> keep the line
            if (c == '-' || c == '=' || c == '_' || c == '~' || c == '*' || c == '#' || c >= 0x80) ++sep;
        }
        return visible >= 3 && sep >= 3;
    };
    std::erase_if(outLines, [&](const std::string& ln) { return isSeparatorLine(ln); });

    while (!outLines.empty() && outLines.front().empty()) outLines.erase(outLines.begin());
    while (!outLines.empty() && outLines.back().empty()) outLines.pop_back();
    if (outLines.empty()) return "";

    std::string result;
    for (size_t i = 0; i < outLines.size(); ++i)
    {
        if (i > 0) result += '\n';
        result += outLines[i];
    }
    return result;
}

// Check if a function definition has the 'static' storage class.
static bool isFunctionStatic(CFlatParser::FunctionDefinitionContext* func)
{
    if (!func->declarationSpecifiers()) return false;
    for (auto* ds : func->declarationSpecifiers()->declarationSpecifier())
        if (ds->storageClassSpecifier() && ds->storageClassSpecifier()->Static() != nullptr)
            return true;
    return false;
}

/*
 * True when a parameter list is callable with ZERO arguments: every parameter carries an
 * '= initializer' default and there is no '...' pack. Such a constructor IS the type's no-arg
 * constructor (its cutoff-0 default-parameter wrapper mangles to the same symbol), so the
 * synthetic one must not also be emitted. A null list means "no parameters at all" and is
 * NOT this case - callers test that separately.
 */
static bool AllParametersDefaulted(CFlatParser::ParameterTypeListContext* paramTypeList)
{
    if (paramTypeList == nullptr) return false;
    if (paramTypeList->Ellipsis() != nullptr) return false;
    auto* list = paramTypeList->parameterList();
    if (list == nullptr) return false;
    auto decls = list->parameterDeclaration();
    if (decls.empty()) return false;
    for (auto* d : decls)
        if (d->initializer() == nullptr)
            return false;
    return true;
}

// The source text of `node` with REDUNDANT PARENTHESES peeled off, so a parenthesized whole-value
// source `(p)` / `((p))` records the bare name `p` and matches a parameter name. Only the
// `'(' expression ')'` primaryExpression alternative is peeled - descending through single-child
// nodes never changes the text, so a non-bare source (`v.f`, `v + 1`, `(a, b)`, `f(1)`) is
// returned unchanged and keeps failing every name comparison, as it must.
// When `outWrapperTypes` is supplied, a `(T)x` cast and an `x as T` are peeled too and each
// peeled target type's SPELLING is appended. The peel is unconditional here because the scanner
// cannot resolve the operand's type; the CALLER makes it type-aware by demanding every recorded
// spelling name the type it is matching against (see AllWrapperTypesName).
inline std::string BareSourceText(antlr4::tree::ParseTree* node,
                                  std::vector<std::string>* outWrapperTypes = nullptr)
{
    while (node != nullptr)
    {
        if (node->children.size() == 1)
        {
            node = node->children[0];
            continue;
        }
        auto* prim = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
        // 3 children with a leading '(' is exactly `'(' expression ')'` - nameof/typeof/sizeof
        // spell 4, and a tuple `(a, b)` is its own context reached as a single child.
        if (prim != nullptr && prim->children.size() == 3 && prim->expression() != nullptr
            && prim->children[0]->getText() == "(")
        {
            node = prim->expression();
            continue;
        }
        if (outWrapperTypes != nullptr)
        {
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
                if (cast->typeName() != nullptr && cast->castExpression() != nullptr)
                {
                    outWrapperTypes->push_back(cast->typeName()->getText());
                    node = cast->castExpression();
                    continue;
                }
            // `x as T` - a single trailing operator, and it must be 'as' (an `is` yields a bool).
            if (auto* tc = dynamic_cast<CFlatParser::TypeCheckExpressionContext*>(node))
                if (tc->typeSpecifier().size() == 1 && tc->children.size() == 3
                    && tc->children[1]->getText() == "as" && tc->relationalExpression() != nullptr)
                {
                    outWrapperTypes->push_back(tc->typeSpecifier(0)->getText());
                    node = tc->relationalExpression();
                    continue;
                }
        }
        break;
    }
    return node == nullptr ? std::string() : node->getText();
}

// Bare consume-source names that were reached only by peeling a CAST / `as` wrapper, mapped to
// every peeled wrapper type spelling. Kept apart from the plain name set so the wrapped spelling
// is admitted only where the wrapper is proven REDUNDANT.
using WrappedSourceNames = std::unordered_map<std::string, std::vector<std::string>>;

// Every peeled wrapper names exactly `typeName`, so the whole wrapper chain is redundant and the
// source is a whole-value consume of the bare name. Exact spellings: an alias or a decorated
// target ("UBox*") does not match, which is the conservative direction (no new consume inferred).
inline bool AllWrapperTypesName(const std::vector<std::string>& wrapperTypes,
                                const std::string& typeName)
{
    if (wrapperTypes.empty() || typeName.empty()) return false;
    for (const auto& t : wrapperTypes)
        if (t != typeName) return false;
    return true;
}

// Record one consume source: the plain set when only parentheses were peeled, the wrapped map
// when a cast / `as` had to be peeled as well.
inline void RecordConsumeSourceName(antlr4::tree::ParseTree* node,
                                    std::unordered_set<std::string>& out,
                                    WrappedSourceNames* wrapped)
{
    if (wrapped == nullptr)
    {
        out.insert(BareSourceText(node));
        return;
    }
    std::vector<std::string> wrapperTypes;
    std::string name = BareSourceText(node, &wrapperTypes);
    if (wrapperTypes.empty())
        out.insert(name);
    else
    {
        auto& slot = (*wrapped)[name];
        slot.insert(slot.end(), wrapperTypes.begin(), wrapperTypes.end());
    }
}

// Detect functions that heap-allocate and return a new owned value, so call sites
// register the result as an owned temp and free it. operator+ always allocates;
// operator string(i32) uses malloc; user functions opt in via 'move string' /
// 'move T*' / 'move <interface>'. Shared by the forward-decl, definition, and
// interface-method-decl paths so the three copies cannot drift apart.
static bool ComputeReturnsOwned(const LLVMBackend::DeclTypeAndValue& returnType,
                                const std::string& name,
                                const std::vector<LLVMBackend::TypeAndValue>& allParams)
{
    if (returnType.TypeName == "string")
    {
        if (name == "operator+")
            return true;
        if (name == "operator string" && allParams.size() == 1 && allParams[0].TypeName == "i32")
            return true;
        if (returnType.IsMove)
            return true;
        return false;
    }
    // 'move <interface>' transfers ownership of the boxed heap object to the caller (the
    // caller is then responsible for 'delete'). An interface return type is not Pointer
    // (its LLVM type is a { vtable, data } fat pointer), so it needs its own case.
    if (returnType.IsMove && (returnType.Pointer || returnType.IsInterface))
        return true;
    // NOTE: a 'move S' struct-VALUE return is deliberately NOT returns-owned. The returned
    // aggregate already carries its owning fields' bits, so the receiving local frees them;
    // also registering it as an owned temp at the call site double-frees (e.g. list<T>.copy()).
    return false;
}

// Collect the 'return <expr>;' expressions a function body owns itself. A nested lambda's
// returns belong to the lambda, so its subtree is skipped.
static void CollectOwnReturnExpressions(antlr4::tree::ParseTree* node,
                                        std::vector<CFlatParser::ExpressionContext*>& out)
{
    if (node == nullptr) return;
    if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr) return;
    if (auto* jump = dynamic_cast<CFlatParser::JumpStatementContext*>(node))
        if (jump->expression() != nullptr) out.push_back(jump->expression());
    for (auto* child : node->children)
        CollectOwnReturnExpressions(child, out);
}

enum class ValueStructReturnKind { NotApplicable, AllBorrowedParam, Mixed };

// Classify the by-value STRUCT returns of a function whose return type carries no ownership
// annotation: does every 'return' hand back a borrowed by-value parameter of the return type,
// or do the paths disagree? The unique-ownership gate lives in the callers - a copyable struct
// has nothing to duplicate, so mixed returns stay legal for it.
static ValueStructReturnKind ClassifyValueStructReturns(
    CFlatParser::FunctionDefinitionContext* func,
    const LLVMBackend::DeclTypeAndValue& returnType,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    std::string* outBorrowedParamName)
{
    if (func == nullptr || func->compoundStatement() == nullptr) return ValueStructReturnKind::NotApplicable;
    if (returnType.TypeName.empty() || returnType.Pointer || returnType.ElemPointer) return ValueStructReturnKind::NotApplicable;
    if (returnType.IsMove || returnType.IsAlias || returnType.external) return ValueStructReturnKind::NotApplicable;

    std::vector<CFlatParser::ExpressionContext*> returns;
    CollectOwnReturnExpressions(func->compoundStatement(), returns);
    if (returns.empty()) return ValueStructReturnKind::NotApplicable;

    int borrowed = 0;
    int other = 0;
    for (auto* expr : returns)
    {
        // `return (p);` / `return (T)p;` hand back the same borrow `return p;` does. Peel the
        // wrappers; a peeled CAST counts only when it names the return type itself (below).
        std::vector<std::string> wrapperTypes;
        const std::string text = BareSourceText(expr, &wrapperTypes);
        if (!wrapperTypes.empty() && !AllWrapperTypesName(wrapperTypes, returnType.TypeName))
        {
            ++other;
            continue;
        }
        bool isBorrowedParam = false;
        for (const auto& p : allParams)
        {
            if (p.VariableName != text) continue;
            if (p.TypeName != returnType.TypeName) continue;
            if (p.Pointer || p.ElemPointer || p.IsMove || p.IsAlias) continue;
            isBorrowedParam = true;
            if (outBorrowedParamName && outBorrowedParamName->empty()) *outBorrowedParamName = p.VariableName;
            break;
        }
        if (isBorrowedParam) ++borrowed; else ++other;
    }
    if (borrowed == 0) return ValueStructReturnKind::NotApplicable;
    return other == 0 ? ValueStructReturnKind::AllBorrowedParam : ValueStructReturnKind::Mixed;
}

// Arrow-normalized form of a lock expression's source text: 'p->m' becomes 'p.m'. The mode
// suffix (if any) is left in place - see NormalizeLockText for the canonical, suffix-free form.
inline std::string ArrowNormalizeLockText(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); )
    {
        if (text[i] == '-' && i + 1 < text.size() && text[i + 1] == '>')
        {
            out += '.';
            i += 2;
        }
        else
        {
            out += text[i];
            i++;
        }
    }
    return out;
}

// Canonical form of a lock expression's source text. Lock-set membership is compared as
// strings, so 'p->m' and 'p.m' must canonicalize identically: arrow becomes dot, then the
// trailing '.read' / '.write' / '.optimistic' mode suffix is stripped. Used by every lock
// capture site (lock statement, lock clause, positional lock group) in BOTH passes so they agree.
inline std::string NormalizeLockText(const std::string& text)
{
    std::string out = ArrowNormalizeLockText(text);
    if (out.ends_with(".read"))
        out.resize(out.size() - 5);
    else if (out.ends_with(".write"))
        out.resize(out.size() - 6);
    else if (out.ends_with(".optimistic"))
        out.resize(out.size() - 11);
    return out;
}

// Returns "read", "write", "optimistic", or "" (exclusive) for a raw lock expression's text.
inline std::string LockTextMode(const std::string& text)
{
    // Arrow-normalize first so 'p->rw.read' (and 'rw->read') still detect the mode suffix.
    std::string dotted = ArrowNormalizeLockText(text);
    if (dotted.ends_with(".read"))
        return "read";
    if (dotted.ends_with(".write"))
        return "write";
    if (dotted.ends_with(".optimistic"))
        return "optimistic";
    return "";
}

// The held-mode a `.read` / `.write` / `.optimistic` suffix confers. "" and "write" are both
// exclusive - the bare form `lock (m)` must never be demoted to Shared.
inline LockMode LockModeFromSuffix(const std::string& mode)
{
    if (mode == "read")       return LockMode::Shared;
    if (mode == "optimistic") return LockMode::Optimistic;
    return LockMode::Exclusive;
}

// The capability interface a mode suffix demands of the lock's type. IOptimisticLockable is a
// CHECKING capability - it has no kCapabilities row because an optimistic read lowers to
// nothing (it is a method call taking a lambda, not an acquire/release pair).
inline const char* CapabilityForLockMode(const std::string& mode)
{
    if (mode == "read")       return "ISharedLockable";
    if (mode == "optimistic") return "IOptimisticLockable";
    return "ILockable";
}

// True if executing `node` can RETURN from the enclosing function - a `return` (value,
// return-block, or `return default`) that is not inside a nested lambda. `break`/`continue`
// exit a loop, not the function, so they do not count (jump->Return() is null for them).
inline bool SubtreeContainsFunctionReturn(antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return false;
    if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node)) return false;
    if (auto* jump = dynamic_cast<CFlatParser::JumpStatementContext*>(node))
        if (jump->Return() != nullptr) return true;
    for (auto* child : node->children)
        if (SubtreeContainsFunctionReturn(child)) return true;
    return false;
}


// Tri-state evaluator for an `if const` condition under the CURRENT monomorphization:
// returns 1 (branch taken / live), 0 (not taken / dead), or -1 (cannot decide at all).
// Supplied by the main pass (where type substitutions are active); empty for the scanner.
using IfConstEvaluator = std::function<int(CFlatParser::ExpressionContext*)>;

// Collect names UNCONDITIONALLY moved by the function body: a top-level `move <bare-param>`
// not nested inside any runtime conditional/loop/labeled construct, ternary, or lambda, AND not
// preceded by a statement that can return early. Drives the owning-value move-sink inference;
// a conditional, nested, or possibly-skipped move must NOT mark a param a sink. An `if const`
// whose condition is a compile-time constant for this instantiation is NOT runtime-conditional:
// its live branch is straight-line code, so `evalIfConst` lets us descend into it.
inline void CollectUnconditionalMovedNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out,
                                           const IfConstEvaluator& evalIfConst = {})
{
    if (node == nullptr) return;
    if (auto* sel = dynamic_cast<CFlatParser::SelectionStatementContext*>(node))
    {
        // `if const (COND)`: the live branch (COND-true, or the else on COND-false) executes
        // unconditionally for this instantiation, so descend into it. A dead branch is skipped;
        // an undecidable condition (-1) is treated conservatively as runtime-conditional.
        if (evalIfConst && sel->If() && sel->Const())
        {
            int taken = evalIfConst(sel->expression());
            auto inner = sel->statement();
            if (taken == 1 && inner.size() > 0)
                CollectUnconditionalMovedNames(inner[0], out, evalIfConst);
            else if (taken == 0 && inner.size() > 1)
                CollectUnconditionalMovedNames(inner[1], out, evalIfConst);
        }
        // A runtime `if`/`switch` (or an undecidable if-const) is conditional - stop descending.
        return;
    }
    // A move reached only through one of these executes conditionally (or in a captured
    // scope), so it cannot make the parameter a sink - stop descending here.
    if (dynamic_cast<CFlatParser::IterationStatementContext*>(node)
        || dynamic_cast<CFlatParser::LabeledStatementContext*>(node)
        || dynamic_cast<CFlatParser::LambdaExpressionContext*>(node))
        return;
    // A ternary `a ? b : c` (or `a ?? b`) makes its branches conditional.
    if (auto* cond = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
        if (cond->children.size() > 1) return;
    if (auto* mv = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
        if (auto* u = mv->unaryExpression())
            out.insert(BareSourceText(u));
    for (auto* child : node->children)
    {
        CollectUnconditionalMovedNames(child, out, evalIfConst);
        // A statement that can return from the function makes every LATER sibling conditional,
        // so a `move` after an early exit (e.g. `if (b) return; g = move v;`) is not a sink -
        // consuming the caller there would leak on the not-moved path.
        if (SubtreeContainsFunctionReturn(child)) break;
    }
}

// Collect the bare source name of each POSITIONAL element of a brace list (`{ p, q }`). The
// fixed-array lowering consumes an owning element source exactly as a slot store does
// (ConsumeOwningBraceElementSource), so those names are consumed too. Named (`f = v`) and
// dictionary (`k: v`) forms are skipped - neither reaches that arm.
inline void CollectPositionalBraceElementNames(CFlatParser::InitializerListContext* list,
                                               std::unordered_set<std::string>& out,
                                               WrappedSourceNames* wrapped = nullptr)
{
    if (list == nullptr) return;
    for (auto* fi : list->fieldInit())
        if (fi != nullptr && fi->Identifier() == nullptr && fi->assignmentExpression().size() == 1)
            RecordConsumeSourceName(fi->assignmentExpression(0), out, wrapped);
}

// Collect bare source names CONSUMED by a plain store ANYWHERE in the body (at-least-one-path):
// the RHS of a plain `=` assignment (`X = value`, incl. a slot store `_data[i] = value`), a decl
// initializer (`T x = value`, incl. its brace-list form `T[N] d = { p };` / `T[N] d { p };`), or a
// `move <name>` (conditional too). Unlike
// CollectUnconditionalMovedNames this DESCENDS into runtime conditionals/loops - 8a's total
// scope-exit drop makes a not-taken path sound. Lambdas / nested functions are a different scope
// and are not descended into. The caller intersects with the param list; a non-bare RHS (`v.f`,
// `v + 1`) never equals a param name, so it is naturally excluded (only a whole-value move counts).
inline void CollectConsumedStoreNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out,
                                      WrappedSourceNames* wrapped = nullptr)
{
    if (node == nullptr) return;
    if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node)) return;
    if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(node)) return;
    if (auto* asn = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        if (asn->assignmentOperator() != nullptr && asn->assignmentOperator()->getText() == "="
            && asn->assignmentExpression() != nullptr)
            RecordConsumeSourceName(asn->assignmentExpression(), out, wrapped);
    if (auto* init = dynamic_cast<CFlatParser::InitDeclaratorContext*>(node))
    {
        if (auto* iz = init->initializer(); iz != nullptr)
        {
            if (iz->assignmentExpression() != nullptr)
                RecordConsumeSourceName(iz->assignmentExpression(), out, wrapped);
            CollectPositionalBraceElementNames(iz->initializerList(), out, wrapped);
        }
        // `T[N] d { p };` - the brace-ctor alternative hangs the list off the declarator itself.
        CollectPositionalBraceElementNames(init->initializerList(), out, wrapped);
    }
    if (auto* mv = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
        if (auto* u = mv->unaryExpression())
            out.insert(BareSourceText(u));
    for (auto* child : node->children)
        CollectConsumedStoreNames(child, out, wrapped);
}

// A parameter shape that can carry an owning VALUE (a value struct or `string`). Excludes
// borrows (pointers, interface values, array-views), function pointers, arrays, and params
// already carrying a move/alias/unique qualifier. The definitive "type owns a resource" test
// runs at the call site on the concrete (monomorphized) type, so this only rejects shapes
// that can never be an owning sink.
inline bool ParamIsOwningSinkEligible(const LLVMBackend::TypeAndValue& p)
{
    if (p.Pointer || p.ElemPointer) return false;
    if (p.IsInterface || p.IsInterfacePointer) return false;
    // A THIN C function pointer (function<...> / __c_fn_ptr, or an encoded __thinfn) owns NOTHING,
    // so it can never be an owning sink. A FAT closure (Lambda<...> / __closure_fat_ptr, or an
    // encoded __fatfn) is an OWNING VALUE like string - it owns its captured env - so it IS
    // sink-eligible: moved into the slot on insert, freed on teardown. The concrete
    // owns-a-resource gate still runs at the call site on the monomorphized type.
    if (p.IsThinFnPtr() || p.TypeName.rfind("__thinfn", 0) == 0) return false;
    if (p.IsArrayView || p.IsSimd) return false;
    if (p.ConstArraySize > 0) return false;
    if (p.IsMove || p.IsUniqueTypeArg || p.IsAlias) return false;
    if (p.IsBorrowOfUniqueElement || p.IsBorrowOfAliasElement) return false;
    return true;
}

// Owning-value move-sink inference: a plain by-value parameter is a synthesized sink (IsOwningSink)
// so the caller's owning source is nulled at the call site. Two triggers:
//  (1) an UNCONDITIONAL top-level `move <param>` - the explicit opt-out; consumes any owner.
//  (2) STEP R1 (8b): the body CONSUMES the param via a plain `=`/slot store or a conditional `move`
//      on at least one path. This is flagged STRUCTURALLY (IsConsumeInferredSink) because the
//      scanner cannot resolve a struct's copyability during the forward pass; the concrete
//      NON-COPYABLE-owner gate runs later (OwningSinkConsumesConcrete) at the call site / definition
//      where the monomorphized type is fully known. A copyable owner's store is a COPY, so it stays
//      a borrow there. Both scans are structural (independent of T), so this runs on generic-class
//      instantiations too - ParseFunctionDefinition calls it on the monomorphized param list.
// Body-taking core. A lambda literal has no FunctionDefinitionContext but is a function all the
// same, so its parameter list runs the identical inference against its own body.
inline void ApplyOwningSinkInferenceToBody(antlr4::tree::ParseTree* body,
                                     std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst = {})
{
    if (body == nullptr) return;
    std::unordered_set<std::string> movedNames;
    CollectUnconditionalMovedNames(body, movedNames, evalIfConst);
    std::unordered_set<std::string> consumedNames;
    WrappedSourceNames wrappedConsumed;
    CollectConsumedStoreNames(body, consumedNames, &wrappedConsumed);
    for (auto& p : allParams)
    {
        if (p.VariableName.empty() || !ParamIsOwningSinkEligible(p)) continue;
        // A cast-wrapped source counts only when every peeled wrapper names the parameter's own
        // type - a TYPE-CHANGING cast is not a whole-value consume of the parameter.
        bool wrappedConsume = false;
        if (auto it = wrappedConsumed.find(p.VariableName); it != wrappedConsumed.end())
            wrappedConsume = AllWrapperTypesName(it->second, p.TypeName);
        if (movedNames.count(p.VariableName))
            p.IsOwningSink = true;
        else if (consumedNames.count(p.VariableName) || wrappedConsume)
        {
            p.IsOwningSink = true;
            p.IsConsumeInferredSink = true;
        }
    }
}

// A declared `Lambda<...>` / `function<...>` spelling cannot express an INFERRED owning sink, so a
// funcptr destination adopts the flags of the lambda literal (or funcptr value) it is bound to.
// Union, never clear: over-approximating a sink leaks at worst, missing one double-frees, and a
// destination rebound to a differently-inferred value must keep the stronger claim.
inline void AdoptInferredParamSinks(LLVMBackend::TypeAndValue& dest,
                                    const std::vector<LLVMBackend::TypeAndValue::FuncPtrParam>& src)
{
    if (dest.FuncPtrParams.size() != src.size()) return;
    for (size_t i = 0; i < src.size(); i++)
    {
        dest.FuncPtrParams[i].IsOwningSink =
            dest.FuncPtrParams[i].IsOwningSink || src[i].IsOwningSink;
        dest.FuncPtrParams[i].IsConsumeInferredSink =
            dest.FuncPtrParams[i].IsConsumeInferredSink || src[i].IsConsumeInferredSink;
    }
}

inline void ApplyOwningSinkInference(CFlatParser::FunctionDefinitionContext* func,
                                     std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst = {})
{
    ApplyOwningSinkInferenceToBody(func->compoundStatement(), allParams, evalIfConst);
}

// True when the function declares a RETURN TYPE, which is what separates an ordinary member
// from a constructor. Not the same as having declarationSpecifiers: per CFlat.g4:783 a ctor
// may carry 'inline'/'static'/'const'/'extern'/'stdcall' - those are functionSpecifier,
// storageClassSpecifier and typeQualifier, never typeSpecifier.
// KNOWN IMPRECISION, message wording only: 'move' IS a typeSpecifier (CFlat.g4:323), and
// 'unique'/'alias'/'bond' are not grammar keywords at all - they parse as genericIdentifier,
// also a typeSpecifier. So a ctor carrying one of those four reads as "member" in the
// duplicate diagnostic. The duplicate is still caught; only the noun is wrong.
inline bool FunctionDeclaresReturnType(CFlatParser::FunctionDefinitionContext* func)
{
    auto* specs = func->declarationSpecifiers();
    if (specs == nullptr) return false;
    for (auto* spec : specs->declarationSpecifier())
        if (spec->typeSpecifier() != nullptr) return true;
    return false;
}

// Render a parameter type list for a diagnostic, e.g. "int, string*". Types only:
// parameter names are not part of a function signature.
inline std::string DescribeParameterTypes(const std::vector<LLVMBackend::TypeAndValue>& params)
{
    std::string text;
    for (const auto& p : params)
    {
        if (!text.empty()) text += ", ";
        text += p.TypeName;
        // An array-view param carries Pointer too, but spells as 'T[]', never 'T[]*'.
        if (p.IsArrayView) text += "[]";
        else if (p.Pointer) text += p.ElemPointer ? "**" : "*";
    }
    return text;
}

// ForwardRefScanner performs a lightweight pre-pass over the AST to register
// all function signatures and struct type shells before the main code-gen walk.
// This allows functions and types to be used before their definition in source.
class ForwardRefScanner
{
private:
    LLVMBackend* compilerLLVM;
    antlr4::BufferedTokenStream* tokens_ = nullptr;

    LLVMBackend* Compiler(antlr4::ParserRuleContext* ctx);

    // The scanner cannot evaluate an `if const` condition (no active type substitutions, no
    // expression codegen), so it skips those members rather than register a not-taken signature.
    template <typename TCtx>
    std::vector<CFlatParser::AggregateMemberContext*> ResolveAggregateMembers(TCtx* ctx)
    {
        std::vector<CFlatParser::AggregateMemberContext*> out;
        for (auto* m : ctx->aggregateMember())
            if (!m->ifConstMember()) out.push_back(m);
        return out;
    }

    // Same reasoning as ResolveAggregateMembers: the scanner cannot evaluate the condition, so it
    // skips if-const interface members rather than register a not-taken method into the contract.
    std::vector<CFlatParser::InterfaceMemberContext*> ResolveInterfaceMembers(CFlatParser::InterfaceDefinitionContext* ctx);

    CFLAT_DEFINE_MEMBER_FLATTENERS()
    CFLAT_DEFINE_INTERFACE_FLATTENERS()

    LLVMBackend::DeclTypeAndValue ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs);

    std::vector<LLVMBackend::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList);

    void ScanFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName = {}, const std::string& namespaceName = {}, const std::vector<std::string>& extraRequiredLocks = {});

    void ScanInterfaceDefinition(CFlatParser::InterfaceDefinitionContext* ctx,
                                 const std::string& namespaceName = {});

    // Pre-declare a struct or class type shell, member functions, and destructor.
    // Templated to handle both StructDefinitionContext and ClassDefinitionContext.
    template<typename TCtx>
    void ScanStructOrClassDefinition(TCtx* ctx, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        // Generic template definitions are not pre-declared; they are instantiated on demand.
        // However, we still register the template name and its method names in the LSP
        // symbol index so hover / go-to-definition can resolve uses through a variable
        // of the instantiated type (e.g. `g_explosions.getPtr` -> `array.getPtr`).
        if (ctx->genericTypeParameters() != nullptr)
        {
            if (auto* s = compiler->GetSymbolSink())
            {
                std::string genBaseTypeName = ctx->directDeclarator()->getText();
                std::string typeName = genBaseTypeName;
                if (!namespaceName.empty())
                    typeName = namespaceName + "." + typeName;
                std::string keyword = ctx->getStart()->getText();
                std::string doc = ExtractLeadingDoc(tokens_, ctx->getStart());
                s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            keyword + " " + typeName, {}, doc);
                for (auto* func : MemberFunctionDefinitions(ctx))
                {
                    std::string funcName = getFunctionName(func);
                    // Ctors use the bare name (same fix as the non-generic path below).
                    if (funcName == genBaseTypeName || funcName.empty()) continue;
                    std::string qualName = typeName + "." + funcName;
                    std::string fdoc = ExtractLeadingDoc(tokens_, func->getStart());
                    s->Register(SymbolKind::Function, qualName, compiler->GetSourceFilePath(),
                                (int)func->getStart()->getLine(),
                                (int)func->getStart()->getCharPositionInLine(),
                                getFunctionSignatureText(func), {}, fdoc);
                }
            }
            // A generic class's real base clause needs main-pass type-arg substitution, and its
            // instances appear only on demand: never prove anything about the interfaces it names.
            for (auto* spec : BaseClauseIdentifiers(ctx))
            {
                std::vector<std::string> candidates;
                AppendInterfaceNameCandidates(compiler, namespaceName, BaseSpecifierName(spec), candidates);
                for (const auto& c : candidates)
                    compiler->RecordUncertainInterfaceImpl(c);
            }
            return;
        }

        std::string baseTypeName = ctx->directDeclarator()->getText();
        std::string typeName = baseTypeName;
        if (!namespaceName.empty())
            typeName = namespaceName + "." + typeName;

        // Record the declared interface list before any codegen, so a conversion site can see
        // implementors declared LATER in the file. Static-check only - see scannedInterfaceImpls.
        {
            std::vector<std::string> scannedIfaces;
            for (auto* spec : BaseClauseIdentifiers(ctx))
            {
                std::string ifaceBaseName = BaseSpecifierName(spec);
                if (ifaceBaseName.empty()) continue;
                // A generic interface spelling only mangles after substitution; record the
                // template as uncertain rather than guessing the instance name.
                if (spec->genericTypeParameters() != nullptr)
                    compiler->RecordUncertainInterfaceImpl(ifaceBaseName);
                else
                    AppendInterfaceNameCandidates(compiler, namespaceName, ifaceBaseName, scannedIfaces);
            }
            compiler->RecordScannedStructInterfaces(typeName, scannedIfaces);
        }
        // Member-scope `if const` is skipped by ResolveAggregateMembers for the same reason, so a
        // nested class inside one is invisible too - mark what it names uncertain.
        for (auto* m : ctx->aggregateMember())
            if (auto* icm = m->ifConstMember())
                MarkIfConstClassImplsUncertain(compiler, icm, namespaceName, {}, typeName);

        // Register opaque struct so the type is known for pointer/field use
        compiler->CreateStructType(typeName, {});

        if (auto* s = compiler->GetSymbolSink())
        {
            std::string keyword = ctx->getStart()->getText();
            std::string doc = ExtractLeadingDoc(tokens_, ctx->getStart());
            s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        keyword + " " + typeName, {}, doc);
            // Also register under the unqualified name so Lookup("Point") finds "Geometry.Point".
            size_t dot = typeName.rfind('.');
            if (dot != std::string::npos)
                s->Register(SymbolKind::Struct, typeName.substr(dot + 1), compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            keyword + " " + typeName, {}, doc);
        }

        // Pre-declare default constructor
        LLVMBackend::TypeAndValue returnType{ .TypeName = typeName };
        compiler->CreateFunctionDeclaration(typeName, returnType, {});

        // Pre-declare member functions (and detect constructor overloads).
        // Ctor signatures seen so far in this body, keyed by mangled name -> declaring line.
        std::map<std::string, size_t> seenCtorSignatures;
        for (auto func : MemberFunctionDefinitions(ctx))
        {
            // Ctors are written with the bare type name - match baseTypeName, not the
            // namespace-qualified typeName, or a namespaced class's ctor falls through below.
            if (getFunctionName(func) == baseTypeName)
            {
                // Constructor overload - no implicit this* parameter, returns the type
                std::vector<LLVMBackend::TypeAndValue> allCtorParams;
                if (func->parameterTypeList() != nullptr)
                {
                    auto ctorParams = ParseParameterTypeList(func->parameterTypeList());
                    allCtorParams.assign(ctorParams.begin(), ctorParams.end());
                }

                // Two members whose parameter TYPES match collide on one mangled name, and the
                // main pass then re-emits a body into an already-finished function. Reject here.
                // varargs is not part of the mangled key - ComputeMangledName ignores it.
                std::string ctorKey = compiler->ComputeMangledName(typeName, returnType, allCtorParams);
                auto seen = seenCtorSignatures.find(ctorKey);
                if (seen != seenCtorSignatures.end())
                {
                    // A same-named member with a return type is not a constructor - say "member".
                    const char* noun = FunctionDeclaresReturnType(func) ? "member" : "constructor";
                    Compiler(func)->LogError(std::format(
                        "duplicate {} '{}({})' - a {} with the same parameter types is "
                        "already defined on line {}. Parameter names are not part of the signature.",
                        noun, baseTypeName, DescribeParameterTypes(allCtorParams), noun, seen->second));
                }
                seenCtorSignatures[ctorKey] = func->getStart()->getLine();

                if (func->parameterTypeList() == nullptr) continue; // no-arg already declared above
                compiler->CreateFunctionDeclaration(typeName, returnType, allCtorParams);
            }
            else
            {
                ScanFunctionDefinition(func, typeName);
            }
        }

        // Pre-declare destructor
        for (auto dtor : MemberDestructorDefinitions(ctx))
        {
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = typeName;
            thisParam.VariableName = typeName + "__";
            thisParam.Pointer = true;
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            compiler->CreateFunctionDeclaration("~" + typeName, voidReturn, { thisParam });
        }

        // Recursively pre-declare nested struct/class definitions
        for (auto* nestedStruct : MemberStructDefinitions(ctx))
            ScanStructDefinition(nestedStruct, typeName);
        for (auto* nestedClass : MemberClassDefinitions(ctx))
            ScanClassDefinition(nestedClass, typeName);

        // Pre-declare functions inside positional lock groups and mark their RequiredLocks.
        for (auto* lfg : MemberLockFieldGroups(ctx))
        {
            auto groupArgs = lfg->lockClause()->lockArgList()->expression();
            if (groupArgs.empty()) continue;
            // Same canonicalization as GetLockArgCanonical in MainListener (both passes must agree).
            std::string guardianName = NormalizeLockText(groupArgs[0]->getText());
            // Qualify bare names to "this.<name>" so call-site substitution works.
            std::string qualifiedLock = (guardianName.find('.') == std::string::npos)
                                        ? ("this." + guardianName) : guardianName;
            std::vector<std::string> groupLocks = { qualifiedLock };
            for (auto* func : lfg->functionDefinition())
            {
                // A constructor guards nothing yet (no instance exists until it returns),
                // so it has no place inside a lock field group - reject it cleanly here.
                // A constructor is ONLY a function with no declarationSpecifiers - an ordinary
                // method that happens to share the class's name (e.g. `int C()`) is NOT one.
                if (func->declarationSpecifiers() == nullptr && getFunctionName(func) == baseTypeName)
                {
                    Compiler(func)->LogError(std::format(
                        "constructor '{}' is not allowed inside a lock field group", baseTypeName));
                }
                else
                    ScanFunctionDefinition(func, typeName, {}, groupLocks);
            }
        }
    }

    void ScanStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& namespaceName = {});

    void ScanClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& namespaceName = {});

    // File-scope lock group: pre-declare the free functions inside the group with the group's
    // guardian as a RequiredLock. Unlike the struct case the guardian is NOT prefixed with
    // "this." - a global guard has no receiver, so its canonical name is the bare global name
    // (CheckCallSiteLocks then falls through to "global lock - no substitution").
    // The group's declarations are globals, which the scanner does not pre-scan; ParseGlobalLockGroup
    // stamps their GuardedBy during codegen.
    void ScanGlobalLockGroup(CFlatParser::LockFieldGroupContext* ctx, const std::string& namespaceName = {});

    // One `using` declaration considered as a mangling-visible pure rename (see
    // PreRegisterRenameAliases). Registration is name-only: whether the target really names a type
    // is the authoritative ParseUsingDeclaration's call, and an alias to a non-type still mangles
    // to a name that fails the same way its target does.
    void RegisterRenameAlias(CFlatParser::UsingDeclarationContext* ctx);

public:
    ForwardRefScanner(LLVMBackend* compiler);
    void SetTokens(antlr4::BufferedTokenStream* t);

    /*
     * Record every file-scope pure-rename `using` alias BEFORE either pass walks the file, so
     * MangleTypeArg folds the same alias set in both. It cannot be left to the walk: ScanGenericTypeUses
     * mangles every generic use in the file BEFORE ScanExternalDeclaration reaches the first `using`,
     * while codegen sees the alias already registered - the two would build a struct shell under
     * `list__MyInt` and look it up as `list__i32`.
     *
     * Only a bare-name target is a pure rename (IsBareTypeName); `using Handle = void*;` and
     * `using Vec3 = float[3];` keep their suffix in the alias string and stay opaque here.
     * Deliberately does NOT descend into `if const` arms or function bodies: the scan cannot tell
     * which arm is taken, and the two arms legitimately bind one alias to different types
     * (core/os.posix.cb binds `win_size` to i64 or i32). Those aliases stay opaque to the mangler,
     * which is what both passes did before this existed.
     */
    void PreRegisterRenameAliases(antlr4::RuleContext* ctx);

    // Resolve a type-argument entry to the same string MainListener::ResolveTypeArgEntry
    // would produce at the top level (no active substitutions during the forward scan),
    // recursing into nested generics so list<int> inside list<list<int>> mangles to
    // "list__int" rather than the literal "list<int>". A trailing pointer becomes "*"
    // (MangleTypeArg later turns it into "ptr"), keeping shell names consistent with
    // the main pass.
    std::string ResolveForwardTypeArg(CFlatParser::TypeParameterEntryContext* entry);

    // Scanner counterpart of ResolveSigComponentCodegen: resolve a closure signature type. The scan
    // pass has no active substitutions, so a plain type stays getText(); a nested generic mangles to
    // match MangledGenericName; a nested closure encodes recursively. Names only (no queue/register).
    std::string ResolveSigComponentScanner(CFlatParser::TypeSpecifierContext* ts, bool& outPointer);

    /*
     * Scanner counterpart of MainListener::SigComponentResolvedKey, kept identical the way the two
     * ParseDeclarationSpecifiers copies are. This copy is the load-bearing one: a call site reads
     * the parameter signature the SCANNER registered into the function table, not the main pass's.
     * A type not yet scanned resolves to nothing, which records "" - no proof, today's behaviour.
     */
    std::string SigComponentResolvedKeyScanner(const std::string& name);

    // Scanner counterpart of EncodeClosureCodegen: builds the encoded name only (the main pass owns
    // registration, so the copy-overload is not lost to RegisterEncodedClosureType's idempotency).
    std::string EncodeClosureScanner(CFlatParser::FunctionPointerSpecifierContext* fpSpec);

    // ---- Scanner-side `if const` folding -------------------------------------------------------
    // The main pass decides an `if const` with DecideIfConstCondition -> EvalIfConstConstant, whose
    // leaves bottom out in EmitAndFoldIfConstLeaf (real IR emission into a scratch function). That
    // leaf cannot be hoisted out of MainListener, so the scanner cannot literally call it. What IS
    // shared is the part that decided bf1/bf2/bf3/bf5: the STRUCTURAL recursion. The two functions
    // below mirror EvalIfConstConstant's tree walk arm for arm (?:, ||, &&, expression /
    // assignmentExpression unwrap, and the same "any known-false wins" partial-knowledge rules) and
    // only substitute a constant-folding leaf for the emitting one. Any change to
    // EvalIfConstConstant's structure must be mirrored here.
    // Whatever still folds to nullopt is handled by the provisional-routing machinery below - the
    // scanner never guesses a routing that the main pass could contradict silently.
    std::optional<int64_t> ScannerFoldIfConst(antlr4::tree::ParseTree* node);

    // The leaf half: constant-folds the integer operator chain without emitting IR. Covers what an
    // `if const` platform guard is actually written with - a compile-time macro, an integer literal,
    // redundant parens, unary !/-/~/+, and the comparison / bitwise / arithmetic chain over those
    // (`__PLATFORM__ == 32`, `__MACOS__ != 0`). Anything else folds to nullopt.
    std::optional<int64_t> ScannerFoldIfConstLeaf(antlr4::tree::ParseTree* node);

    // True when a value is representable as codegen's default `int` (i32). Every value this folder
    // produces is kept inside this range, so no fold can silently differ from codegen by a 32-bit
    // wraparound - the alternative reproduces codegen's exact literal typing, which the scanner
    // cannot see (suffixes, u32/long promotion), so out-of-range folds to nullopt instead.
    static bool InScannerInt32Range(int64_t v);

    // Portable stand-ins for __builtin_*_overflow, which MSVC does not provide. Each computes in
    // unsigned (well-defined wraparound) and reports whether the signed result overflowed.
    static bool ScannerAddOverflow(int64_t a, int64_t b, int64_t* out);

    static bool ScannerSubOverflow(int64_t a, int64_t b, int64_t* out);

    static bool ScannerMulOverflow(int64_t a, int64_t b, int64_t* out);

    // An integer literal this pass can type EXACTLY the way codegen does, else nullopt. Deliberately
    // narrow, because a literal typed differently from codegen is a silent scanner/main-pass
    // disagreement, which surfaces as a raw LLVM verifier failure with no source location:
    //   - a u/U/l/L suffix changes the type (unsigned / 64-bit) -> refuse
    //   - a C OCTAL literal (leading 0) is base 8 to codegen -> refuse rather than misread as decimal
    //   - a value outside i32 (0xFFFFFFFF, 0x80000000, 2147483648) is typed and wrapped by codegen
    //     in a way this pass cannot reproduce -> refuse
    // Refusing only makes the condition undecidable, which the if-const walk already handles safely.
    std::optional<int64_t> ParseScannerIntegerLiteral(const std::string& textIn);

    // Tri-state, same contract as the main pass's DecideIfConstCondition: 1 taken, 0 not taken,
    // -1 undecidable at scan time.
    int ScannerDecideIfConst(CFlatParser::ExpressionContext* expr);

    // Recurse the `if const` chain. A DECIDABLE condition visits exactly the taken arm, so a
    // provably-dead declaration is invisible. An UNDECIDABLE one visits every arm with
    // certain=false: the interface name still registers (suppressing it is what made
    // `if const ((__MACOS__))` reach the LLVM verifier - the main pass's folder decides that
    // condition and routes the local to a fat pointer while the scan had built an opaque struct
    // signature), while the struct name does NOT, because a struct name only ever VETOES the
    // interface routing and a guess there silently disables the fix.
    // No typePath parameter: the grammar puts `ifConstDeclaration` only in `externalDeclaration`
    // and `ifConstBlock`, so this arm is reachable only at file/namespace scope. A struct-body
    // `if const` parses as `ifConstMember`, which is the overload below.
    void CollectGenericTemplateDeclsIfConst(CFlatParser::IfConstDeclarationContext* ctx, bool certain,
                                            bool ifConstUnfoldable = false, const std::string& ns = {});

    // Same walk for a member-scope `if const` (a nested generic type inside a class). Member scope
    // is inside a type body, so `typePath`/`unkeyable` ride along unchanged - a type declared in
    // one of these arms is still keyed under its enclosing struct.
    void CollectGenericTemplateDeclsIfConstMember(CFlatParser::IfConstMemberContext* ctx, bool certain,
                                                 bool ifConstUnfoldable, const std::string& ns,
                                                 const std::string& typePath, bool unkeyable);

    // Record every generic template declared in the tree: struct/class bare names into
    // scannedGenericStructNames, generic interface bare names into scannedGenericInterfaceNames.
    // `certain` is false only inside an if-const branch this pass could not fold; see
    // CollectGenericTemplateDeclsIfConst for why that gates the struct half and not the interface
    // half. An interface routed from an unfoldable branch that turns out to be DEAD is caught by
    // LLVMBackend::RejectUnroutedGenericInterface at the points a value of that type is
    // materialised or converted. A method call on such a value is already a clean compile error,
    // but that is NOT sufficient on its own: the name still lowers to a fat pointer, so it can
    // LAUNDER one real interface's vtable into another real interface - see that function.
    void CollectGenericTemplateDecls(antlr4::RuleContext* ctx, bool certain, bool ifConstUnfoldable = false,
                                     const std::string& ns = {}, const std::string& typePath = {},
                                     bool unkeyable = false);

    // "NS" + "Box" -> "NS.Box"; the one place the generic template key is assembled in the scan.
    static std::string QualifyName(const std::string& ns, const std::string& name);

    // The dotted name of `ctx` as seen from an enclosing namespace `ns` (handles both the
    // `namespace A.B` and the nested `namespace A { namespace B` spellings).
    static std::string NestedNamespaceName(const std::string& ns, CFlatParser::NamespaceDefinitionContext* ctx);

    // A generic struct/class of this key exists, so the key is not interface-only. Revoke
    // any instance an earlier file already routed to a fat pointer.
    void RecordScannedGenericStructName(const std::string& name);

    // Pre-pass that runs before ScanGenericTypeUses: records which generic templates this file
    // declares, so a use of 'Container<int>' is routed as a fat-pointer interface instead of being
    // pre-declared as an opaque struct shell. Accumulates across files (imports scan first), and
    // the struct role always wins a collision - see IsGenericInterfaceTemplateName.
    void ScanGenericInterfaceTemplateNames(antlr4::RuleContext* ctx);

    // Walk every typeSpecifier in the entire parse tree and pre-declare an opaque
    // struct type + default constructor for each generic instantiation found.
    // This ensures that Box<MyType> references inside function bodies resolve
    // correctly during the main pass, before ProcessPendingInstantiations runs.
    void ScanGenericTypeUses(antlr4::RuleContext* ctx);

    // Build the function-pointer TypeAndValue for a `function<R(Args)>` spec - the resolved form
    // a `using Cb = function<R(Args)>;` alias stores and that ParseDeclarationSpecifiers expands.
    // The closure value rides IsFunctionPointer + the signature; TypeName is the backing fat type.
    LLVMBackend::TypeAndValue BuildFuncPtrAliasType(CFlatParser::FunctionPointerSpecifierContext* fpSpec);

    void ScanUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx);

    void ScanProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx);

    void ScanImportedProgramDefinition(const std::string& name);

    void ScanAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx);

    void ScanExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {});

    void ScanNamespace(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {});
};




class MainListener : public CFlatBaseListener
{
private:
    CFlatParser* parser;
    LLVMBackend* compilerLLVM;
    std::string sourceFileName;
    std::string importNamespace_;

    LLVMBackend* Compiler(antlr4::ParserRuleContext* ctx);
    inline LLVMBackend* Compiler() { return compilerLLVM; }

public:
    // What the CALLER of an expression parse will do with the result. Threaded down the pure
    // single-child passthrough chain only; any operator context resets it to Value.
    //  Value        - the result is consumed (stored, passed, joined, tested). A void call here
    //                 produces nothing, so consuming it is an error.
    //  Discard      - an expression statement / for-increment / void `=> expr` body. Nothing is
    //                 consumed, so a void call is exactly right.
    //  ReturnOperand- the WHOLE operand of a `return`. EmitReturnExpression owns the void-ness
    //                 decision there (a void crossing is legal out of a void function), so the
    //                 call site defers to it rather than rejecting first.
    enum class ResultUse { Value, Discard, ReturnOperand };

private:

    // amount, pointer-stride element type, and (union member only) the type to load/store the
    // storage as - a union member's Storage is the union alloca, so the inferred type is wrong.
    struct IncrementWork { int Amount = 0; llvm::Type* ElemType = nullptr; llvm::Type* LoadType = nullptr; };
    std::unordered_map<llvm::Value*, IncrementWork> PlusPlus;
    bool global_scope = true; // true when parsing an entity in the global scope.

    // RAII guard: sets global_scope to false on entry and restores the saved value on exit.
    struct GlobalScopeGuard
    {
        bool& scope;
        bool  saved;
        explicit GlobalScopeGuard(bool& s) : scope(s), saved(s) { scope = false; }
        ~GlobalScopeGuard() { scope = saved; }
        GlobalScopeGuard(const GlobalScopeGuard&)            = delete;
        GlobalScopeGuard& operator=(const GlobalScopeGuard&) = delete;
    };

    // Lambda state: expected type (set by ParseDeclaration before evaluating RHS)
    // and the last lambda's TypeAndValue (side-channel from ParsePrimaryExpression to ParsePostfixExpression).
    LLVMBackend::TypeAndValue lambdaExpectedType;
    LLVMBackend::TypeAndValue lastLambdaType;
    // Non-empty when the matched function parameter is declared lock(this): holds the canonical
    // receiver name (e.g. "d->ready") to seed currentLockSet during lambda body analysis.
    std::string lambdaLockThisReceiver;
    // The mode lambdaLockThisReceiver is granted in - lock(this.optimistic) grants read-only.
    LockMode lambdaLockThisMode = LockMode::Exclusive;
    // True while emitting the body of a lambda whose "void" return type is the INFERRED
    // fallback (no function<>/Lambda<> context reached it), not a declared one. A `return
    // <expr>;` there is an inference failure, not a return-type violation - see
    // EmitReturnExpression. Saved/restored per lambda so nesting works.
    // The invoker it applies to: a function emitted mid-body (a generic instantiation) has its
    // own declared return type and must not read this lambda's inference state.
    llvm::Function* lambdaReturnInferredFn_ = nullptr;
    // The lambda whose body is being emitted, for that diagnostic. Empty in a named function.
    std::string lambdaReturnInferredName_;

    // Side-channel from ParsePrimaryExpression to ParsePostfixExpression:
    // carries the cast TypeAndValue when the primary is a parenthesized cast expression,
    // enabling ((Struct*)ptr)->field member access without a named intermediate variable.
    LLVMBackend::TypeAndValue lastParenExprType;
    // Companion to lastParenExprType: the storage (lvalue address) of a parenthesized
    // expression, so postfix ++/-- on a parenthesized lvalue (e.g. '(*p)++') can write back.
    llvm::Value* lastParenExprStorage = nullptr;
    // Third companion: the owning-TEMP field provenance of a parenthesized expression, so
    // `(makeBox().t)` keeps the flags the persist-site escape gates read.
    bool lastParenExprFromOwningTempField = false;
    bool lastParenExprOwningTempParent = false;
    std::string lastParenExprOwningStructName;
    std::string lastParenExprFieldName;
    std::string lastParenExprCallerName;
    // Fourth companion: the whole inner NamedVariable. Parentheses are not an operator, so
    // `(x)` must reach the ownership arms with the SAME provenance `x` does - see AdoptWrapperProvenance.
    LLVMBackend::NamedVariable lastParenExprNamed;
    // Set by ParseTypeCheckExpression when the chain was a single REDUNDANT `as` (target type ==
    // operand type); the operand binding it names, so the whole-expression consumer can adopt it.
    CFlatParser::TypeCheckExpressionContext* lastRedundantAsCtx_ = nullptr;
    LLVMBackend::NamedVariable lastRedundantAsNamed_;

    // Variadic forwarding: true when the current function being codegen'd accepts '...'
    bool currentFunctionIsVariadic = false;

    // Lock-set analysis: the canonical lock expressions currently held, and HOW each is held.
    // Cleared at function entry, seeded from the function's lock clause, and pushed/popped
    // around lock statement bodies. Membership grants a read of a guarded field; only
    // LockMode::Exclusive grants a write (see CheckGuardedWrite).
    std::unordered_map<std::string, LockMode> currentLockSet;

    // Generic template state lives on the backend so each LLVMBackend instance
    // owns its own copy. References below alias compilerLLVM->gts.<field> and are
    // bound in the constructor's member initializer list.
    using PendingInstantiation = GenericTemplateState::PendingInstantiation;

    std::unordered_map<std::string, CFlatParser::StructDefinitionContext*>&     genericStructTemplates;
    std::unordered_map<std::string, CFlatParser::ClassDefinitionContext*>&      genericClassTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericStructTypeParams;
    std::unordered_set<std::string>&                                            instantiatedGenerics;
    // Constraints: templateName -> { typeParamName -> [requiredInterface, ...] }
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericStructConstraints;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericClassConstraints;
    // Active type parameter substitutions during generic instantiation (e.g. "T" -> "int")
    std::unordered_map<std::string, std::string> activeTypeSubstitutions;

    // Pack param index per template: index of the variadic param, or npos if not variadic
    std::unordered_map<std::string, size_t>& genericStructPackIndex;
    std::unordered_map<std::string, size_t>& genericClassPackIndex;
    std::unordered_map<std::string, size_t>& genericFunctionPackIndex;
    std::unordered_map<std::string, size_t>& genericInterfacePackIndex;

    // Active pack substitutions during instantiation: pack-param-name -> ["int", "float", "string"]
    std::unordered_map<std::string, std::vector<std::string>> activePackSubstitutions;

    // True while monomorphizing a generic template body. The body's tokens come from the
    // template's own file (e.g. core/list.cb), but currentSourceFilePath_ is the use-site
    // file that triggered the instantiation, so a token's line number does not map onto it.
    // Unused-decl candidates must not be recorded here: the faded "never used" hint would
    // land at a bogus location in the instantiating file (e.g. on an import line).
    bool InGenericInstantiation() const;

    // Struct scope stack: pushed when parsing fields/methods of a struct/class so that
    // unqualified nested type names (e.g. "Inner") resolve to "Outer.Inner".
    std::vector<std::string> structScopeStack;

    // Set only while ParseDeclarationList parses a field's declarationSpecifiers. The
    // 'unique' soft keyword is field-only; every other position must reject it.
    bool inStructFieldDecl_ = false;
    // Saves and restores rather than clearing: if a specifier parse ever nests (a generic
    // instantiation parsing its own fields), clearing would spuriously reject the outer field.
    struct StructFieldDeclGuard
    {
        bool& flag;
        bool  prev;
        explicit StructFieldDeclGuard(bool& f) : flag(f), prev(f) { flag = true; }
        ~StructFieldDeclGuard() { flag = prev; }
    };

    // Set while ParseStructDefinition parses the members of a UNION body. `unique` is rejected
    // there (ValidateUniqueField): a synthesized destructor cannot know which member is active.
    bool inUnionFieldDecl_ = false;
    // Carries this body's own kind and restores on exit, so a struct nested in a union - or a
    // generic instantiation parsed mid-body - is judged by its own definition, not the enclosing one.
    struct UnionFieldDeclGuard
    {
        bool& flag;
        bool  prev;
        UnionFieldDeclGuard(bool& f, bool isUnion) : flag(f), prev(f) { flag = isUnion; }
        ~UnionFieldDeclGuard() { flag = prev; }
    };

    std::unordered_map<std::string, CFlatParser::InterfaceDefinitionContext*>&  genericInterfaceTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericInterfaceTypeParams;
    std::unordered_set<std::string>&                                            instantiatedInterfaces;

    std::unordered_map<std::string, CFlatParser::FunctionDefinitionContext*>&   genericFunctionTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericFunctionTypeParams;
    std::unordered_set<std::string>&                                            instantiatedGenericFunctions;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericFunctionConstraints;

    // Queue for pending generic instantiations (delayed until safe to emit code)
    std::vector<PendingInstantiation>& pendingInstantiations;

    // Mangled tuple name -> element type args (see GenericTemplateState::tupleTypeArgs)
    std::unordered_map<std::string, std::vector<std::string>>& tupleTypeArgs;

    struct SwitchCaseEntry
    {
        llvm::ConstantInt* value = nullptr;       // non-null for integer cases
        llvm::Constant* strLiteral = nullptr;     // non-null for string cases (i8* global)
        llvm::BasicBlock* block;
        bool isTypeCase = false;                  // non-null for type cases (struct or interface)
        std::string typeCaseName;                 // struct or interface name for type cases
        std::string boundVarName;                 // optional bound variable for arm-style type pointer cases
        bool isArmStyle = false;                  // true if this case uses => syntax
    };

    struct SwitchContext
    {
        std::unordered_map<CFlatParser::LabeledStatementContext*, SwitchCaseEntry> caseMap;
        llvm::BasicBlock* defaultBlock = nullptr;
        llvm::BasicBlock* resumeBlock = nullptr;
        bool isStringSwitch = false;
        bool isTypeSwitch = false;                // true if this switch contains type cases
        bool isArmStyle = false;                  // true if any case uses => syntax
        llvm::Value* condValue = nullptr;         // switch condition value (needed for bound var extraction)
    };

    std::vector<SwitchContext> switchStack;

    // Set while emitting the loop wrapped by a `vectorize` statement. Consumed
    // (and cleared) at the start of the iteration-statement emit so a nested
    // non-vectorize loop in the body does not inherit the request, and a nested
    // `vectorize` loop re-arms it for itself.
    bool vectorizeActive_ = false;
    int  vectorizeLine_ = 0;
    // FP-math relaxation tier requested by `vectorize(contract)` / `vectorize(reassoc)`.
    // Latched with vectorizeActive_ and consumed at the iteration-statement emit so a
    // nested loop does not inherit it. Drives the builder fast-math flags on the loop body.
    enum class VectorizeFpTier { None, Contract, Reassoc };
    VectorizeFpTier vectorizeFpTier_ = VectorizeFpTier::None;

    // True when the current straight-line statement sequence has hit a `return` (making any
    // following statements dead). The if/else move-merge samples this per branch to decide
    // whether a branch's moves are on a dead path - immune to dead code after a return, which
    // reopens a live block and hides the ret terminator. Set only by the return handler (not
    // break/continue - see straightLineJumped_); loops/switch restore it (they fall through).
    bool straightLineReturned_ = false;

    // Set by the break/continue handlers. Like a return, a break/continue branch does NOT reach
    // the if's resume block, so its moves must not leak onto the fall-through path. Unlike a
    // return the moves are not dead: they are collected in loopBreakMovedStates_ and merged
    // back at loop exit, so a use-after-move past the loop is still diagnosed.
    bool straightLineJumped_ = false;

    // Per-loop stack of moved-state snapshots taken at each break/continue inside the body.
    std::vector<std::vector<LLVMBackend::MovedStateSnapshot>> loopBreakMovedStates_;

    // Save/restore straightLineReturned_ across a construct that falls through (loop, switch):
    // a return in its body must not mark the enclosing straight-line as returned.
    struct ReturnFlagGuard {
        bool* flag; bool saved;
        explicit ReturnFlagGuard(bool* f) : flag(f), saved(*f) {}
        ~ReturnFlagGuard() { *flag = saved; }
    };

    // Increments suppressExplicitNullDerefGuard_ for the ctor->dtor lifetime, unwinding even if
    // lowering a '?:' arm throws (LogError) or returns early - see the call site's comment.
    struct SuppressExplicitNullDerefGuardScope {
        LLVMBackend* compiler;
        explicit SuppressExplicitNullDerefGuardScope(LLVMBackend* c) : compiler(c)
            { compiler->suppressExplicitNullDerefGuard_++; }
        ~SuppressExplicitNullDerefGuardScope() { compiler->suppressExplicitNullDerefGuard_--; }
    };

    // Snapshot the moved state at a break/continue so the innermost loop can merge it back
    // at loop exit. No-op outside a loop (a switch break has no such rejoin bookkeeping).
    void RecordLoopExitMovedState();

    // Scopes one loop's break/continue snapshot list, then ORs them into the live state on
    // exit so moves performed on a breaking path stay visible to code after the loop.
    struct LoopMovedStateGuard {
        MainListener* self; LLVMBackend* compiler;
        LoopMovedStateGuard(MainListener* s, LLVMBackend* c) : self(s), compiler(c)
            { self->loopBreakMovedStates_.emplace_back(); }
        ~LoopMovedStateGuard() {
            for (const auto& snap : self->loopBreakMovedStates_.back())
                compiler->MergeMovedStateInto(snap);
            self->loopBreakMovedStates_.pop_back();
        }
    };

    // Set the builder's fast-math flags for the loop body according to the tier.
    // contract lets mul+add fuse to fma; reassoc additionally lets the optimizer
    // regroup FP expressions and split reduction accumulators (implies contract).
    static void ApplyVectorizeFpTier(LLVMBackend* compiler, VectorizeFpTier tier);
    // Line of the `vectorize` keyword whose loop body is currently being emitted (0 = none).
    // Set across the body codegen so span-accessor lowering can attribute a noalias-defeating
    // accessor to the enclosing vectorize loop (Detection A). Saved/restored to survive nesting.
    int  currentVectorizeBodyLine_ = 0;

    // Recursively resolve a typeParameterEntry to its mangled string,
    // applying activeTypeSubstitutions and handling nested generics like Box<Box<T>>.
    std::string ResolveTypeArgEntry(CFlatParser::TypeParameterEntryContext* entry);

    // Queue a generic instantiation for a known template (dedup via instantiatedGenerics).
    // For struct/class templates the struct shell + default-ctor declaration are created
    // immediately so the mangled type name resolves even before the body is emitted;
    // interface templates are instantiated later by ProcessPendingInstantiations.
    // No-op for unknown base names (e.g. an unresolved type parameter like "T").
    void QueueGenericInstantiation(const std::string& baseName,
                                   const std::vector<std::string>& typeArgs,
                                   const std::string& mangledName);

    // Resolve a type in a function-pointer signature position (return or param) the way an ordinary
    // type position resolves: apply active substitutions, and if it names a generic instantiation
    // (e.g. list<string>) mangle it + queue the instantiation. A nested closure encodes recursively.
    // A trailing '*' from a substitution folds into outPointer. Non-generic scalar types stay
    // byte-identical to the pre-existing raw-getText() behavior (no alias resolution added).
    std::string ResolveSigComponentCodegen(CFlatParser::TypeSpecifierContext* ts, bool& outPointer);

    /*
     * The declaring-scope-resolved key for a signature component, recorded ALONGSIDE the raw
     * spelling (which still owns the mangled name). Empty = not recorded -> no proof. Inside a
     * namespace a bare spelling the walk could NOT qualify records nothing: recording the bare
     * form there would be a false rejection if the namespaced type is not registered yet, while
     * recording nothing just falls back to the broad candidate set.
     */
    std::string SigComponentResolvedKey(const std::string& name);

    // A generic type argument `Lambda<...>*` is the substitution-path twin of the declarator guard
    // in ParseDeclarationSpecifiers: a fat closure is a by-value struct with no working
    // pointer lowering. The THIN `function<...>*` spelling is a real machine pointer and is kept.
    void RejectFatClosurePointerArg(antlr4::ParserRuleContext* ctx, bool isThin,
                                    const std::string& spelling);

    /*
     * Rebuild a readable source spelling ("Lambda<int(int)>") from a registered encoded closure
     * name, so a diagnostic raised on the substitution path can name the closure the way it was
     * written rather than the mangled symbol. A signature component that is itself a generic
     * instantiation arrives mangled ("list__i32"), so each one goes through
     * DisplayNameOfMangledType - and if ANY component is not provably writable source, the whole
     * spelling falls back to the raw encoded name rather than emitting a half-demangled hybrid
     * ("Lambda<int(list__list__i32*)>") that names no type the user can write.
     */
    static std::string ClosureArgSpelling(LLVMBackend* compiler, const std::string& encodedName);

    // Substitution-path twin of RejectFatClosurePointerArg: fires only when `baseName` is a
    // REGISTERED FAT closure type, so every non-closure type argument is left untouched.
    void RejectFatEncodedClosurePointerArg(antlr4::ParserRuleContext* ctx, const std::string& baseName);

    // Encode a closure type (Lambda<...>/function<...>) that appears as a generic argument or in a
    // nested signature position into a symbol-safe name (BuildEncodedClosureName), resolving its
    // signature component types, and register the call descriptor. Returns the encoded name.
    std::string EncodeClosureCodegen(CFlatParser::FunctionPointerSpecifierContext* fpSpec);

    // Encode + register a closure type from an already-resolved signature (used when a function-type
    // alias `using IntFn = Lambda<int(int)>` appears as a generic arg, so it unifies with the direct
    // spelling). Produces the same name BuildEncodedClosureName gives for the direct form.
    std::string EncodeClosureFromSig(LLVMBackend* compiler, const LLVMBackend::TypeAndValue& sig);

    LLVMBackend::DeclTypeAndValue ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs);

    // Validate a folded alignment byte count: power of two, 1 <= N <= 4096. Returns 0 on error
    // (and logs). `what` names the clause ("alignas" / "alignas allocation alignment").
    uint64_t ValidateAlignValue(antlr4::ParserRuleContext* ctx, int64_t signedVal, const char* what);

    // Evaluate arg1 of `alignas(slot[, alloc])`: the SLOT / type alignment. `alignas(T)` folds
    // the type's ABI alignment; `alignas(N)` folds a constant. Returns 0 on error (and logs) or
    // when arg1 is the natural-slot sentinel `0`.
    uint64_t ParseAlignmentSpecifier(CFlatParser::AlignmentSpecifierContext* alignSpec);

    // Evaluate the optional arg2 of `alignas(slot, alloc)`: the ALLOCATION alignment of the owned
    // heap BLOCK. Positional for now - arg2 is the trailing constantExpression (index 0 when arg1 is
    // a type, else index 1). Returns 0 when absent or on error. A future named form (`alignas(alloc:
    // 64)`) can be recognized here without touching any caller. Shared by declarations and `new`.
    uint64_t ParseAllocAlignArg(CFlatParser::AlignmentSpecifierContext* alignSpec);

    // Return the `new` expression IFF the initializer/RHS is SYNTACTICALLY exactly a `new` (a single
    // pass-through chain assignmentExpression -> ... -> unaryExpression -> newExpression, every node
    // having exactly one child). Any wrapping - a ternary, a binary op, a cast, a call argument -
    // yields a multi-child node and returns nullptr. This is the safety gate for the inbound
    // alloc-align channel: alignment is inherited only when the context provably reaches the `new`.
    CFlatParser::NewExpressionContext* AsDirectNew(antlr4::tree::ParseTree* node);

    LLVMBackend::DeclTypeAndValue getFunctionReturnType(CFlatParser::FunctionDefinitionContext* ctx);

    // Returns the default value for a type:
    //   - struct types (local scope): calls the default constructor.
    //   - struct types (global scope): the constant value of that same construction, if it has one.
    //   - everything else: zero-initializes.
    llvm::Value* GenerateDefaultValue(const LLVMBackend::DeclTypeAndValue& typeValue);

    // Constant value of a type's default construction, or nullptr when it is not constant.
    llvm::Constant* TryFoldGlobalDefaultConstruction(const LLVMBackend::DeclTypeAndValue& typeValue);
    llvm::Constant* SplatConstantOverFixedArray(llvm::Constant* elemConst, llvm::Type* arrType);

public:
    MainListener(CFlatParser* parser, LLVMBackend* compilerLLVM, const std::string& filename);

    void SetImportNamespace(const std::string& ns);

    // Returns the BufferedTokenStream that backs this listener's parser, so that
    // helpers like ExtractLeadingDoc can reach the hidden-channel comment tokens.
    antlr4::BufferedTokenStream* GetTokens() const;

    void ParseInterfaceDefinition(CFlatParser::InterfaceDefinitionContext* ctx,
                                  const std::string& namespaceName = {});

    /*
     * A variadic interface method has no working lowering: the vtable slot's FunctionType is
     * built from the DECLARED parameters only (InterfaceMethod::Parameters excludes the '...'),
     * so the call site dropped every surplus argument - and, before the signature-based slot
     * selection, even the fixed ones. Reject the declaration honestly instead of miscompiling it.
     */
    bool RejectVariadicInterfaceMethod(const std::string& ifaceName,
                                       CFlatParser::InterfaceMethodContext* method);

    /*
     * Field declarations in an interface body (`string title;`). The field name/type must be
     * matched by every implementor; the vtable carries its per-implementor byte offset.
     * An interface field is a field position, so `unique` is spellable here and joins the
     * contract every implementor must match (VerifyInterfaceFields). It has to live in the
     * contract: through dynamic dispatch the concrete type is unknown, so the access site can
     * only learn that the slot is owning from the interface itself.
     */
    std::vector<LLVMBackend::TypeAndValue> ParseInterfaceFields(CFlatParser::InterfaceDefinitionContext* ctx);

    // The namespace a generic template was declared in, as RECORDED at registration.
    // Never re-derive this from the key: "Outer.Box" is equally the key of a template in
    // `namespace Outer` and of one nested in `struct Outer`, and guessing picks the namespace.
    static std::string DeclaringNamespaceOf(LLVMBackend* compiler, const std::string& key);

    // Installs a template's declaring namespace for the duration of an instantiation: the body is
    // re-walked long after that scope closed, so without this a bare sibling name resolves globally.
    struct TemplateNamespaceScope : LLVMBackend::NamespaceScope
    {
        TemplateNamespaceScope(LLVMBackend* c, const std::string& key)
            : LLVMBackend::NamespaceScope(c, DeclaringNamespaceOf(c, key)) {}
    };

    void InstantiateGenericInterface(const std::string& baseName, const std::string& mangledName,
                                     const std::unordered_map<std::string, std::string>& substitutions,
                                     const std::unordered_map<std::string, std::vector<std::string>>& packSubstitutions = {});

    // Owner struct of a generic template keyed "Owner.method", or "" when the template is a
    // free function or a static member (a static has no receiver, so it stays free-shaped).
    std::string GenericMethodOwner(const std::string& baseName, CFlatParser::FunctionDefinitionContext* tmplCtx);

    // Template key for a generic method called on a receiver ("Holder" + "get" -> "Holder.get"),
    // or "" when the receiver's type declares no such template.
    std::string GenericMethodTemplateKey(const std::string& receiverType, const std::string& methodName);

    // True when the child right after `child` is a '.' - i.e. the primary is used as a member
    // qualifier (`Holder<i64>.sget(...)`) rather than being called or indexed.
    bool IsFollowedByDot(CFlatParser::PostfixExpressionContext* ctx, antlr4::tree::ParseTree* child);

    // Instantiate a generic function template with concrete type arguments.
    // Returns the mangled name of the instantiated function, or empty string on failure.
    std::string InstantiateGenericFunction(const std::string& baseName, const std::vector<std::string>& typeArgs);

    // Infer the type arguments for a generic function template from the receiver's
    // interface type name (e.g. "Container__int" -> T="int") and instantiate it.
    // Returns the mangled name of the instantiated function, or empty string on failure.
    std::string InferAndInstantiateGenericFunction(const std::string& funcName, const std::string& receiverType);

    // Infer type arguments for a generic function from call argument types and instantiate it.
    // Handles simple positional matching: where parameter type == type param name, bind to arg TypeName.
    std::string TryInferAndInstantiateFromArgs(const std::string& funcName,
                                               const std::vector<LLVMBackend::NamedVariable>& args);

    // Build the function-pointer TypeAndValue for a `function<R(Args)>` spec (the resolved form a
    // `using Cb = function<R(Args)>;` alias stores). Duplicated from the ForwardRefScanner copy -
    // the two passes are separate classes, mirroring the duplicated ParseDeclarationSpecifiers.
    LLVMBackend::TypeAndValue BuildFuncPtrAliasType(CFlatParser::FunctionPointerSpecifierContext* fpSpec);

    void ParseUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx);

    void ParseAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx);

    void ParseExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {});

    // File-scope lock group: `lock(g_mtx) { <globals> <functions> }`. The globals get
    // GuardedBy = g_mtx (stamped in CreateGlobalVariable via pendingGlobalGuardedBy); the
    // functions were already given RequiredLocks = {g_mtx} by ScanGlobalLockGroup.
    void ParseGlobalLockGroup(CFlatParser::LockFieldGroupContext* ctx, const std::string& namespaceName = {});

    // Cache of resolved aggregate member lists, keyed on the struct/class context. Cleared for a
    // given ctx at the start of its walk so each generic instantiation re-evaluates conditions.
    // LLVM names of globals whose initializer is a true compile-time constant for `if const`:
    // const-qualified global scalars and enum members. A plain mutable global is excluded, so
    // its initializer is never folded into an if-const decision (it stays a runtime value).
    std::unordered_set<std::string> constFoldableGlobals_;

    // True when a global declaration carries the `const` type qualifier.
    static bool DeclSpecHasConst(CFlatParser::DeclarationSpecifiersContext* declSpec);

    std::map<const void*, std::vector<CFlatParser::AggregateMemberContext*>> resolvedMembers_;

    // RAII scope for one struct/class body walk. Drops the cached entry for ctx so this walk
    // re-resolves under its own type substitutions, and restores the previous entry on exit so a
    // re-entrant walk of the same template ctx (a nested type pulling in another instantiation)
    // cannot leave the outer walk looking at the inner instantiation's branch selection.
    struct ResolvedMembersScope
    {
        std::map<const void*, std::vector<CFlatParser::AggregateMemberContext*>>* map = nullptr;
        const void* key = nullptr;
        bool had = false;
        std::vector<CFlatParser::AggregateMemberContext*> saved;

        ResolvedMembersScope(std::map<const void*, std::vector<CFlatParser::AggregateMemberContext*>>& m, const void* k)
            : map(&m), key(k)
        {
            auto it = map->find(key);
            if (it != map->end())
            {
                had = true;
                saved = std::move(it->second);
                map->erase(it);
            }
        }
        ~ResolvedMembersScope()
        {
            map->erase(key);
            if (had) map->emplace(key, std::move(saved));
        }
    };

    void AppendResolvedMembers(const std::vector<CFlatParser::AggregateMemberContext*>& members,
                               std::vector<CFlatParser::AggregateMemberContext*>& out);

    // Member-scope `if const`: same semantics as the file-scope ParseIfConstDeclaration, but the
    // selected branch's members are spliced into the enclosing aggregate's member list.
    void AppendIfConstMembers(CFlatParser::IfConstMemberContext* ctx,
                              std::vector<CFlatParser::AggregateMemberContext*>& out);

    template <typename TCtx>
    const std::vector<CFlatParser::AggregateMemberContext*>& ResolveAggregateMembers(TCtx* ctx)
    {
        auto it = resolvedMembers_.find((const void*)ctx);
        if (it != resolvedMembers_.end()) return it->second;

        std::vector<CFlatParser::AggregateMemberContext*> out;
        AppendResolvedMembers(ctx->aggregateMember(), out);
        return resolvedMembers_.emplace((const void*)ctx, std::move(out)).first->second;
    }

    void AppendResolvedInterfaceMembers(const std::vector<CFlatParser::InterfaceMemberContext*>& members,
                                        std::vector<CFlatParser::InterfaceMemberContext*>& out);

    // Interface-scope `if const`: same semantics as AppendIfConstMembers, but splicing the selected
    // branch's methods/fields into the enclosing interface's member list.
    void AppendIfConstInterfaceMembers(CFlatParser::IfConstInterfaceMemberContext* ctx,
                                       std::vector<CFlatParser::InterfaceMemberContext*>& out);

    // Cache of resolved interface member lists, keyed on the interface context. Dropped for the
    // duration of a walk by ResolvedInterfaceMembersScope so each instantiation re-evaluates.
    std::map<const void*, std::vector<CFlatParser::InterfaceMemberContext*>> resolvedInterfaceMembers_;

    const std::vector<CFlatParser::InterfaceMemberContext*>& ResolveInterfaceMembers(CFlatParser::InterfaceDefinitionContext* ctx);

    // RAII counterpart to ResolvedMembersScope for one interface body walk.
    struct ResolvedInterfaceMembersScope
    {
        std::map<const void*, std::vector<CFlatParser::InterfaceMemberContext*>>* map = nullptr;
        const void* key = nullptr;
        bool had = false;
        std::vector<CFlatParser::InterfaceMemberContext*> saved;

        ResolvedInterfaceMembersScope(std::map<const void*, std::vector<CFlatParser::InterfaceMemberContext*>>& m, const void* k)
            : map(&m), key(k)
        {
            auto it = map->find(key);
            if (it != map->end())
            {
                had = true;
                saved = std::move(it->second);
                map->erase(it);
            }
        }
        ~ResolvedInterfaceMembersScope()
        {
            map->erase(key);
            if (had) map->emplace(key, std::move(saved));
        }
    };

    CFLAT_DEFINE_MEMBER_FLATTENERS()
    CFLAT_DEFINE_INTERFACE_FLATTENERS()

    void ParseIfConstDeclaration(CFlatParser::IfConstDeclarationContext* ctx, const std::string& namespaceName = {});

    void ParseNamespaceDefinition(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {});

    void enterExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx) override;

    void ParseBlockItemList(CFlatParser::BlockItemListContext* ctx);

    // Force a tuple's field layout to be emitted now. Used when a tuple type is reached only
    // through a return type whose producing function has not been code-generated yet, so its
    // fields are still empty. Looks up the element type args recorded when the shell was named.
    void EnsureTupleInstantiated(const std::string& mangledName);

    void ParseDestructuringDeclaration(CFlatParser::DestructuringDeclarationContext* ctx);

    // Recursively collects case/default labels from a statement (to handle `case 1: case 2: stmt` nesting).
    void CollectCasesFromStatement(CFlatParser::StatementContext* stmt, SwitchContext& ctx);

    // Returns the first call-like postfix expression (one carrying an argument
    // list) in the subtree, or nullptr. Used to point a `vectorize` diagnostic at
    // a non-inlinable call in the loop body.
    CFlatParser::PostfixExpressionContext* FindFirstCall(antlr4::tree::ParseTree* node);

    void ScanComparisons(antlr4::tree::ParseTree* node, bool& hasRelational, bool& hasEquality);

    // True when a loop condition compares with == / != but carries no relational
    // bound (< <= > >=): the classic non-countable sentinel / pointer-chase form
    // (e.g. `while (p != nullptr)`).
    bool ConditionIsSentinel(antlr4::tree::ParseTree* node);

    // Parse the body of a controlled statement (if/else/for/while/do/foreach) and free owned
    // temporaries at its end. A braced compound body already flushes per block-item (the
    // FlushOwnedTemps at the block-item boundary in ParseCompoundStatement); an UNBRACED single
    // statement bypasses that loop, so a chained-concat intermediate like the `a + b` of
    // `j = j + p + ","` would leak. Flush only for the non-compound case (a no-op otherwise).
    // Must run while still in the body block: FlushOwnedTemps' dominance guard frees only temps
    // registered in the current block, so the caller branches to the loop latch / resume after.
    void ParseControlledBody(CFlatParser::StatementContext* body);

    // Trace one candidate DATA pointer to the frame alloca it addresses, or nullptr when it
    // outlives the frame (heap, global, by-reference parameter, or anything loaded).
    llvm::AllocaInst* FrameLocalDataPointer(llvm::Value* data,
                                            std::unordered_set<const llvm::Value*>& seen);

    // Collect the values inserted at `index` of a fat pointer, following the '?:' joins that can
    // sit between the boxing site and here. One entry per reachable boxing site.
    void CollectFatValueFields(llvm::Value* fatValue, unsigned index,
                               std::vector<llvm::Value*>& out,
                               std::unordered_set<const llvm::Value*>& seen);

    // The data half of an already-built interface fat pointer, when it is provably THIS frame's
    // own storage. Returns the alloca, or nullptr when the pointer outlives the frame. A '?:'
    // that is frame-local on ONE arm still dangles, so any frame-local arm answers.
    llvm::AllocaInst* FrameLocalDataOfFatValue(llvm::Value* fatValue);

    // The class that owns a vtable global, or "" if it is not a registered vtable.
    std::string VTableOwnerName(llvm::GlobalVariable* vtable, LLVMBackend* compiler);

    // The class a fat pointer was boxed from, read off the VTABLE it carries. The vtable is the
    // only operand that still names the class once the cast erased the operand's type; the data
    // pointer's storage does not (a `Square` field of a `Wrap` local allocas as `Wrap`, and an
    // element of a `Square[3]` local allocas as an array). Empty when no single class answers -
    // e.g. a '?:' joining two different implementors.
    std::string BoxedStructNameOfFatValue(llvm::Value* fatValue, LLVMBackend* compiler);

    // The concrete class the interface-return upcast keys off: the binding's declared TypeName,
    // or the LLVM struct name when the binding carries only a type. Empty for a '?:' join, which
    // has no binding at all - that is what routes the join to the per-arm boxing instead.
    std::string ReturnUpcastStructName(const LLVMBackend::NamedVariable& nv);

    // One wording for both spellings that reach it: `return sq;` and `return sq as IShape;`.
    // An empty structName means the boxed class could not be pinned down - say nothing about it
    // rather than naming the wrong type and advising a 'new' that would not compile.
    void LogInterfaceReturnDangle(antlr4::ParserRuleContext* ctx, const std::string& structName,
                                  const std::string& ifaceName);

    // The shared `return <expr>;` lowering, used by the return statement and by an
    // expression-body lambda (`=> expr`, which is `=> { return expr; }`).
    void EmitReturnExpression(antlr4::ParserRuleContext* errCtx,
                              CFlatParser::AssignmentExpressionContext* assignExpr,
                              const std::string& retText);

    void ParseStatement(CFlatParser::StatementContext* statement);

    void GenerateDefaultParamOverloads(
        const std::string& name,
        const LLVMBackend::DeclTypeAndValue& returnType,
        const std::vector<LLVMBackend::DeclTypeAndValue>& params,
        bool varargs,
        size_t line);

    // Tri-state evaluate an `if const` condition for the active instantiation WITHOUT emitting
    // code: 1 (taken), 0 (not taken), -1 (not a compile-time constant / cannot decide). A valid
    // if-const condition folds to a ConstantInt, so nothing is inserted; the insert point is
    // saved/restored defensively. Feeds owning-sink inference (CollectUnconditionalMovedNames).
    int EvaluateIfConstForSink(CFlatParser::ExpressionContext* expr);

    // Bind EvaluateIfConstForSink as the if-const evaluator passed to ApplyOwningSinkInference.
    IfConstEvaluator SinkIfConstEvaluator();

    // Emit a non-short-circuiting `if const` leaf (a comparison / arithmetic / primary / call
    // subtree, no `&&` `||` `?:` at this level) into a THROWAWAY function and fold it. The scratch
    // function gives the builder a live insert block, so a global/enum load or any other emitting
    // shape cannot dereference a null insert block at declaration scope. Returns the folded integer,
    // or nullopt when the subtree does not fold to a compile-time constant. Diagnostics are
    // suppressed: a non-constant condition is reported once by the caller, not here.
    std::optional<int64_t> EmitAndFoldIfConstLeaf(antlr4::tree::ParseTree* node, bool forceScratch, bool suppress);

    // Dispatch a leaf `if const` subtree to the matching typed Parse method for emission. The
    // recursive evaluator only ever bottoms out at an inclusiveOrExpression (the level just below
    // the short-circuit operators), but the other wrapper types are handled defensively.
    llvm::Value* EmitIfConstLeafValue(antlr4::tree::ParseTree* node);

    // Recursively fold an `if const` condition subtree to a compile-time integer WITHOUT
    // committing IR to the live function. Short-circuit `&&` / `||` / ternary `?:` are evaluated
    // here, BEFORE emission, so their branch+phi lowering never blocks folding; every remaining
    // leaf is routed through a discarded scratch function (EmitAndFoldIfConstLeaf) so a global or
    // enum load cannot crash on a null insert block. Returns nullopt when the subtree is not a
    // compile-time constant.
    std::optional<int64_t> EvalIfConstConstant(antlr4::tree::ParseTree* node, bool forceScratch, bool suppress);

    // Tri-state decision for an `if const` condition: 1 (taken), 0 (not taken), -1 (not a
    // compile-time constant). Single entry point for the four if-const evaluation sites; never
    // requires a live insert block and never leaves a half-built block behind.
    int DecideIfConstCondition(CFlatParser::ExpressionContext* expr);

    // bodyNamespace: the namespace the BODY resolves in when it differs from the one the NAME is
    // built from - a generic instantiation carries its whole key in nameOverride, so namespaceName
    // must stay empty there while the body still belongs to the template's declaring namespace.
    void ParseFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName = {}, const std::string& namespaceName = {}, const std::string& nameOverride = {}, const std::string& bodyNamespace = {});

    std::vector<LLVMBackend::AnnotationValue> ParseAnnotationList(CFlatParser::AnnotationListContext* annList);

    std::vector<LLVMBackend::DeclTypeAndValue> ParseDeclarationList(std::vector<CFlatParser::DeclarationContext*> ctx);

    // Stage 1 of `unique`: the synthesized destructor deletes exactly one raw pointer per
    // field, so anything it cannot express that way is rejected rather than silently leaked.
    void ValidateUniqueField(const LLVMBackend::DeclTypeAndValue& f, antlr4::ParserRuleContext* ctx);

    // A field's `alignas(0, N)` allocation-alignment clause routes `delete field` to the aligned
    // deallocator (__delete_aligned), recovering N from the clause. That is only sound when the
    // compiler owns the block: a `unique` field is compiler-allocated and freed and its store site
    // validates the source, and an array view frees a `new T[n] alignas(0, N)` buffer via `delete[_]`.
    // On a hand-managed raw pointer the stored block may come from plain `operator new`, and the
    // aligned free then reads a header that was never written - heap corruption. Require `unique`.
    void ValidateAllocAlignField(const LLVMBackend::DeclTypeAndValue& f, antlr4::ParserRuleContext* ctx);

    /*
     * Run-once guard around a `static` local's initializer. Opened BEFORE the declarator emits any
     * IR, so the whole initializer (constructor calls, brace inits, array seeding) sits inside it,
     * and closed when the declarator is done - including on an early `continue`, via the RAII scope
     * below. A constant initializer is folded into the global's own initializer and the guard is
     * erased again, so `static int c = 5;` costs nothing at run time.
     */
    struct StaticLocalGuard
    {
        llvm::GlobalVariable* Flag = nullptr;
        llvm::BasicBlock* PreBB = nullptr;
        llvm::BasicBlock* InitBB = nullptr;
        llvm::BasicBlock* ContBB = nullptr;
        llvm::Instruction* FlagLoad = nullptr;
        llvm::Instruction* FlagCmp = nullptr;
        llvm::Instruction* CondBr = nullptr;
        llvm::Instruction* FlagStore = nullptr;
    };
    void OpenStaticLocalGuard(StaticLocalGuard& guard, const std::string& varName);
    void CloseStaticLocalGuard(StaticLocalGuard& guard, const std::string& varName);

    // Closes the guard at the end of the declarator, on every path out of it.
    struct StaticLocalGuardScope
    {
        MainListener* Owner = nullptr;
        StaticLocalGuard Guard;
        std::string Name;
        ~StaticLocalGuardScope();
    };

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseForDeclaration(CFlatParser::ForDeclarationContext* ctx);

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CFlatParser::DeclarationContext* ctx, const std::string& namespaceName = {});

    void ParseEnumSpecifier(CFlatParser::EnumSpecifierContext* ctx);

    // Descend a single-child expression chain from `ctx` to a top-level `move` expression, if one
    // is the entire expression. Returns null when any operator (binary, unary, sizeof, cast) sits
    // above the move, since then the move is not the whole RHS.
    static CFlatParser::MoveExpressionContext* TopLevelMoveExpression(antlr4::tree::ParseTree* node);

    // True when `text` is a single bare identifier (no `.`, `[`, `(`, `*`, etc.), so a `move <text>`
    // names a variable rather than a field/index/call/deref.
    static bool IsBareIdentifierText(const std::string& text);

    // Explicitly release a live owning local NOW (the `_ = move x;` form): run the same per-type
    // teardown scope exit uses, null the slot so exit is a no-op, and mark the local moved-from so
    // a later read is a compile error. Borrows and primitives own no resource - a harmless no-op.
    void ReleaseOwningLocalNow(antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable* nv,
                               const std::string& name);

    // Explicitly release a live owning GLOBAL NOW (the `_ = move g;` form). Globals have no
    // stackNamedVariable frame, so this mirrors ReleaseOwningLocalNow but resolves the
    // NamedVariable via GetGlobalVariableNV (globalNamedVariable/globalVariableTypes) instead.
    // Nulling the global's storage after DropValue is load-bearing: EmitGlobalDestructorsInMain
    // runs the same type's full destructor over every global at exit, and that destructor reads
    // the runtime owned-bit from storage - zeroing it here makes that later call a no-op instead
    // of a double free.
    // Only covers the shapes OwnsDroppableResource recognizes (string, owning value struct); a
    // `unique T*` / unique-interface / closure global is NOT released here, matching
    // EmitGlobalDestructorsInMain's own Pointer/IsInterface skip at exit.
    void ReleaseOwningGlobalNow(antlr4::ParserRuleContext* ctx, const std::string& name);

    // Recover ownership of a local/temp that was `move`d out of a container element slot. The slot
    // read demoted the element's `unique`-ness (a plain read hands out a borrow), so its TRUE
    // ownership is taken from `elemOwnershipType`: the DECLARED destination type for
    // `T tmp = move _data[i]` (its IsUniqueTypeArg), or the moved element value's ElementOwningUnique
    // for the `_ = move _data[i]` discard form (which has no destination type). An owning thin
    // `unique T*` / `unique <iface>` element is marked owning so DropValue frees it exactly once; a
    // bare borrow element clears ownership so DropValue frees nothing.
    static void ApplyMovedSlotOwnership(LLVMBackend::NamedVariable& nv,
                                        const LLVMBackend::TypeAndValue& elemOwnershipType);

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CFlatParser::DeclarationSpecifiersContext* declSpec, CFlatParser::InitDeclaratorListContext* initDecl, const std::string& namespaceName = {});

    /*
     * Name the `unique` field a NamedVariable reads, as "Struct.field", for diagnostics.
     * A bare self-field access inside the owning struct's own method comes from
     * GetMemberVariable, which deliberately leaves OwningStructName/FieldName empty so the
     * delete-encapsulation rule does not fire on self-access; recover the field from the
     * variable name and the struct from the enclosing function. That recovers the struct for a
     * destructor ("~Type") only - a method's frame carries the bare method name - so fall back
     * to the unqualified field name, as the sibling `unique` diagnostics already do.
     */
    std::string DescribeUniqueFieldOwner(const LLVMBackend::NamedVariable& nv);

    /*
     * Flatten a constant-offset address chain to its root value, accumulating the byte offset.
     * Stops at the first non-constant index (a runtime array subscript), reporting allConstant
     * false - the caller then knows nothing about that address and must not conclude anything.
     */
    static llvm::Value* ResolveConstantOffsetRoot(llvm::Value* addr, const llvm::DataLayout& dl,
                                                  llvm::APInt& offset, bool& allConstant);

    // The object an address bottoms out in, walking the whole GEP chain (an array element is two
    // or more GEPs deep). The result is an alloca, a global, a load, or anything else.
    static llvm::Value* StripAllGeps(llvm::Value* addr);

    /*
     * True when an address bottoms out in a STACK or GLOBAL object. Such storage is
     * default/zero-initialized, so destructing the old value in a slot is safe. A chain rooted at
     * a LOAD - a pointer receiver or an array view - is NOT, since a raw-malloc'd block holds
     * uninitialized garbage and destructing that corrupts the heap. Same trade, and same
     * polarity, as the closure store.
     */
    static bool AddressRootIsStackOrGlobal(llvm::Value* addr);

    /*
     * PROOF that two addresses lie in DIFFERENT objects: each bottoms out in its own stack or
     * global object, and two DISTINCT AllocaInsts / GlobalVariables are distinct objects in LLVM,
     * whatever the indices in between. Keyed on the root KIND, never on Value* inequality alone -
     * two LoadInsts can name one object, which is what SameLoadedPointer exists for.
     */
    static bool ProvablyDifferentObjects(llvm::Value* a, llvm::Value* b);

    /*
     * PROOF that two LoadInsts yield the same pointer: they read the same address in one basic
     * block with no memory write in between, so nothing can have changed the loaded value. An
     * array reached through a pointer or an array VIEW re-loads its base for each element, so
     * without this the two roots are distinct Values and nothing about them is provable.
     */
    bool SameLoadedPointer(llvm::Value* a, llvm::Value* b);

    /*
     * PROOF that two lvalues denote DIFFERENT slots: either they root in two distinct stack or
     * global OBJECTS, or they flatten to the SAME root with all-constant offsets and those
     * offsets DIFFER. A runtime subscript of ONE array (`arr[i].slot`) leaves a non-constant
     * index off a single root and answers false, so an unprovable pair stays treated as a
     * self-assign - the polarity that makes an unseen case a missing diagnostic, never a false
     * rejection.
     */
    bool ProvablyDifferentSlots(llvm::Value* a, llvm::Value* b);

    /*
     * The OBJECT an interface-FIELD lvalue reads through, resolved back to the value that was
     * boxed. The address is a GEP chain off `extractvalue fat, 1`, so the fat value is
     * recoverable; a fat value loaded out of an interface LOCAL is traced to the single box
     * stored into that slot. Two DISTINCT boxes of one object share a data pointer, which is what
     * makes this more than a Value equality test on the field address. Returns null - "cannot
     * tell" - for a parameter, a call result, a rebound slot, or any other shape.
     */
    llvm::Value* ResolveBoxedObjectOfInterfaceField(llvm::Value* addr, llvm::AllocaInst*& slot,
                                                    llvm::StoreInst*& boxStore);

    /*
     * The WRITTEN spelling of an INDEXED field-read RHS ("arr[1].slot", "p->arr[0].slot"), for a
     * diagnostic that would otherwise name the slot from CallerName - which is the CONTAINER for
     * an array element, so "<caller>.<field>" points at element 0 and its `move` remedy either
     * transfers the wrong slot or does not compile at all. Keyed on the SOURCE TEXT rather than
     * the GEP shape because a zero index folds its element GEP away, and that is exactly the
     * element whose name-derived spelling is wrong. Returns empty for anything that is not a
     * plain indexed lvalue path, leaving the name-derived spelling in place.
     */
    static std::string IndexedFieldPathText(const std::string& text);

    /*
     * The one spelling of a `unique` field SOURCE that is known to name the right slot, or empty
     * when none is. The written text wins for an indexed path; otherwise the name-derived
     * "<caller>.<field>" is trusted only when it IS what the user wrote. Empty means the caller
     * must not put any expression in a `move` remedy - naming the wrong element silently
     * transfers the wrong pointee, which is worse than the missing diagnostic this all replaced.
     */
    std::string ExactUniqueFieldAccess(const LLVMBackend::NamedVariable& nv,
                                       const std::string& srcText);

    // Spell the source expression a `unique` field was read through, as the user wrote it
    // ("b.p", or bare "p" for a self-field access), so a diagnostic can suggest `move <that>`.
    std::string DescribeUniqueFieldAccess(const LLVMBackend::NamedVariable& nv);

    /*
     * True when this NamedVariable reads an owning `unique` field's slot directly (`a.p`, or a
     * bare self-field access inside the owning struct's own method). `move a.p` returns a fresh
     * NamedVariable with no Storage, so it is not a field read and never matches - which is
     * what keeps the sanctioned transfer legal. IsInterfaceField mirrors the sibling
     * srcIsStructField test and fires for the WRITTEN-`unique` spelling as well as for a
     * generic-substituted fat-interface source (`Box<unique IShape>::t`), whose `Pointer` is
     * false and which is admitted by IsOwningUniqueInterfaceField rather than the pointer gate.
     *
     * The ownership gate reuses IsOwningUniquePointerField / IsOwningUniqueInterfaceField
     * (declared later in this class; legal since member bodies are compiled as if after the class
     * is complete). A source read off an owning TEMPORARY (`makeBox().t`, `list.get(0).t`) has no
     * field GEP for the shape test below and is answered by the sibling IsUniqueTempFieldRead;
     * the shape test itself stays narrow on purpose.
     */
    bool IsUniqueFieldRead(const LLVMBackend::NamedVariable& nv);

    /*
     * True when this reads an owning `unique` POINTER field off a TEMPORARY - a call result
     * (`makeBox().t`) or a borrowed container element (`l.get(0).t`). Such a read never lands on
     * a field GEP, so IsUniqueFieldRead's shape test cannot see it, yet the temp's synthesized
     * destructor frees the pointee just the same and a second owner would double-free. The temp
     * provenance is already recorded by the by-value member-access branch (MovableTempField for a
     * call result, FromOwningTempField for a borrowed element), so nothing new is tracked here.
     * `move` off a temp is rejected earlier ("requires an addressable source") and never arrives.
     */
    bool IsUniqueTempFieldRead(const LLVMBackend::NamedVariable& nv);

    /*
     * True when this reads a `unique` field off a temporary THIS STATEMENT destructs
     * (`makeBox().t`): the temp's synthesized destructor frees the pointee at the end of the
     * statement, so binding the value into any slot that outlives the statement dangles - whether
     * or not the POINTEE itself has a destructor. The sibling FromOwningTempField rejects are
     * keyed on IsOwningValueType(TypeName), which is only true for a dtor-bearing pointee, so a
     * dtor-less one fell through every gate as a silent use-after-free.
     *
     * OwningTempParent is the load-bearing half of the polarity: a BORROWED element
     * (`l.get(0).t`, an `alias` return) leaves it clear, nothing is freed at end of statement, and
     * the read is legal - measured on both binaries. Reads that do not outlive the statement
     * (`makeBox().t->v`, passing it to a reader) never reach a persist site and are untouched.
     *
     * TWO SOURCES, because a CAST overwrites TypeAndValue with the destination type and a JOIN
     * delivers a PHI with no NamedVariable at all - both drop every declared fact below. The
     * second source is owningTempUniqueFields_, ledgered at the READ under exactly the declared
     * predicate, and consulted through the join-arm walk. Ledgering cannot reject, so a spelling
     * the ledger misses degrades to no diagnostic rather than to a false rejection.
     */
    bool IsOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& nv);

    // The DECLARED half of the predicate above, read straight off the NamedVariable the
    // member-access branch built. Also the condition under which the read is ledgered.
    static bool DeclaredOwningTempUniqueFieldRead(const LLVMBackend::NamedVariable& nv);

    /*
     * The diagnostic for the predicate above. `move <temp>.<field>` is not a remedy (a temporary
     * has no address), so it names the one that works: bind the whole call result to a local.
     */
    void RejectOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& rightNV,
                                           const std::string& destDesc,
                                           antlr4::ParserRuleContext* ctx);

    /*
     * True when a destination field PROVABLY owns a raw pointer whose synthesized destructor
     * frees it, by either spelling: the written `unique` qualifier, or generic substitution of a
     * `unique T*` type argument. Destructor synthesis already ORs the two (LLVMBackend.h ~4614),
     * but the borrow-provenance rejects were keyed on IsUnique alone, so the generic spelling
     * (`Box<unique Item*>::t`) reached that free with no diagnostic at all (silent abort).
     * The type-arg arm mirrors the `uniqueAutoSink` ownership rule (LLVMBackend.h ~3581) and is
     * further restricted to the exact scalar-pointer shape whose free is synthesized, so it can
     * only reject a slot that is provably freed. Everything else is let through: an `alias` type
     * argument and a borrow-of-unique-element never set IsUniqueTypeArg in the first place, and a
     * container's own `T* _data` buffer has it cleared by the explicit-star rule (~3974).
     */
    static bool IsOwningUniquePointerField(const LLVMBackend::TypeAndValue& tv);

    /*
     * The fat-interface counterpart of IsOwningUniquePointerField: a field that PROVABLY owns a
     * boxed interface value, by the written `unique IShape` spelling or by generic substitution
     * of a `unique IShape` type argument. The value is a {vtable,data} struct rather than a raw
     * pointer, so the function above - whose type-arg arm requires `Pointer` - can never see it.
     * The exclusions mirror that function: an `alias` type argument and a borrow-of-unique
     * element never claim ownership, and an array/simd shape is not the scalar slot whose
     * teardown frees one boxed object.
     */
    static bool IsOwningUniqueInterfaceField(const LLVMBackend::TypeAndValue& tv);

    /*
     * Trap A: storing a BORROW into a `unique` field. The field declares that it owns the pointee
     * and its synthesized destructor deletes it, but the borrow's real owner frees it as well.
     * Shared by the two independent field-store paths - the `=` assignment in
     * ParseAssignmentExpression and brace-init / `<Tag attr=...>` sugar via EmitOneFieldInit - so
     * both spell the rejection identically. `fieldDesc` names the destination field.
     */
    void RejectBorrowIntoUniqueField(const LLVMBackend::NamedVariable& rightNV,
                                     const std::string& fieldDesc,
                                     antlr4::ParserRuleContext* ctx);

    /*
     * A `unique` location owns its pointee and its synthesized teardown frees it. A stack or
     * global address is not ownable - the free would run on memory the program never allocated
     * (abort / heap corruption at run time, with no diagnostic). Rejected only for a PROVABLE
     * non-heap address (LLVMBackend::IsProvableNonHeapAddress).
     */
    void RejectNonHeapAddressIntoUnique(const std::string& destDesc,
                                        antlr4::ParserRuleContext* ctx);

    /*
     * The LOCAL counterpart of RejectBorrowIntoUniqueField: a BORROW stored into a `unique` local.
     * The local's scope-exit teardown frees the pointee and so does its real owner, so this would
     * free it twice. Shared by the decl-init and `=` reassignment paths so both spell it the same.
     * `srcIsField` selects the wording: only a plain borrowed PARAMETER can be fixed by declaring
     * the parameter 'move', so the field form suggests dropping 'unique' instead.
     */
    void RejectBorrowIntoUniqueLocal(const std::string& srcDesc, const std::string& originName,
                                     bool srcIsField, const std::string& localName,
                                     bool isInit, antlr4::ParserRuleContext* ctx);

    /*
     * A direct `unique`-field-to-`unique`-field copy (`c.p = a.p`): both synthesized destructors
     * free the one pointee. The Trap A reject above cannot see this - a field read off a plain
     * local is not a borrow - and the owning-value field-to-field rule cannot either, since a raw
     * pointer is not an owning value type. Shared by the `=` and brace-init store paths.
     */
    std::string FormatUniqueFieldToUniqueField(const LLVMBackend::NamedVariable& rightNV,
                                               const std::string& fieldDesc,
                                               const std::string& srcText);

    void RejectUniqueFieldToUniqueField(const LLVMBackend::NamedVariable& rightNV,
                                        const std::string& fieldDesc,
                                        antlr4::ParserRuleContext* ctx,
                                        const std::string& srcText = {});

    /*
     * The TEMPORARY-source form of the reject above. Neither spelling may suggest `move <access>`:
     * the access names a temporary and `move` off one is rejected outright ("requires an
     * addressable source"). The two halves of IsUniqueTempFieldRead have DIFFERENT owners and so
     * different true explanations and different working remedies, measured on both binaries:
     *
     *   - OwningTempParent: a call result (`makeBox().t`). The TEMP owns the pointee and its
     *     destructor frees it at the end of the statement. Binding the whole call result to a
     *     local gives the field an address, after which `move` out of that local works.
     *   - Without it: a borrowed element (`l.get(0).t`, an `alias` return). Nothing
     *     is freed at end of statement - the CONTAINER owns the pointee and frees it at its own
     *     teardown. Binding it to a local and moving out of that DOUBLE-FREES, so that remedy must
     *     not be named here; a borrowing (non-`unique`) destination is the one that works.
     */
    std::string FormatUniqueTempFieldToField(const LLVMBackend::NamedVariable& rightNV,
                                             const std::string& fieldDesc);

    void RejectUniqueTempFieldToField(const LLVMBackend::NamedVariable& rightNV,
                                      const std::string& fieldDesc,
                                      antlr4::ParserRuleContext* ctx);

    /*
     * The fat-interface form of the reject above. Deliberately does NOT suggest `move`: the
     * written `unique IShape` field spelling refuses a `move` source too (the D5 leg above only
     * admits `new` / a move-returning call / nullptr), so naming `move` here would send the user
     * at a spelling that does not work. `new` is the transfer that does. The wording holds for a
     * `move` source as well, because a `move` out of an interface FIELD does not null it.
     */
    std::string FormatUniqueInterfaceFieldToField(const LLVMBackend::NamedVariable& rightNV,
                                                  const std::string& fieldDesc);

    void RejectUniqueInterfaceFieldToField(const LLVMBackend::NamedVariable& rightNV,
                                           const std::string& fieldDesc,
                                           antlr4::ParserRuleContext* ctx);

    /*
     * RECORD an interface field-to-field `unique` store whose two receivers are PROVABLY different
     * objects - two distinct boxed roots. Recording cannot reject, so a shape this cannot resolve
     * degrades to today's missing diagnostic, never to a false rejection; the verdict is settled at
     * end of body (RunUniqueIfaceFieldStoreCheck), where a receiver rebound later is visible.
     */
    void RecordInterfaceFieldToFieldStore(const LLVMBackend::NamedVariable& namedVar,
                                          const LLVMBackend::NamedVariable& rightNV,
                                          llvm::Value* destination,
                                          bool destOwnsUniqueInterface,
                                          const std::string& srcText,
                                          antlr4::ParserRuleContext* ctx);

    /*
     * Reject an allocation-alignment disagreement storing into a struct field. The alignment of an
     * over-aligned `new T[n] alignas(0, N)` is a property of the ALLOCATION, not of the type, so a
     * free through the field can only recover it when the FIELD declares the same clause; a missing
     * or mismatched one frees the block wrong and corrupts the heap. (Alignment carried by the TYPE
     * is not tagged here and escapes freely, as in C++.) Shared by the two field-store paths - `=`
     * in ParseAssignmentExpression and brace-init / `<Tag attr=...>` sugar via EmitOneFieldInit - so
     * neither spelling can reach a mismatched free site. `fieldTV` is the destination field's
     * declared type and `fieldAlign` its clause; `right` is the value being stored. Returns true
     * when an error was logged.
     */
    bool RejectFieldAllocAlignMismatch(
        const LLVMBackend::TypeAndValue& fieldTV,
        uint64_t fieldAlign,
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        const std::string& fieldDesc,
        antlr4::ParserRuleContext* ctx);

    /*
     * Ownership bookkeeping for storing an owning pointer into a slot. Two mechanisms, exactly as
     * the `=` path has always applied them: a new-allocated local escaping to a NON-local slot
     * (struct field, heap object) gets a lazy refcount so both sides validly hold the pointer and
     * only the last one frees; anything else owning (a move param, or any owning pointer into a
     * local slot) transfers by nulling the source so scope-exit cleanup skips it. Shared by the two
     * field-store paths - `=` in ParseAssignmentExpression and brace-init / `<Tag attr=...>` sugar
     * via EmitOneFieldInit - so a pointer stored through either spelling is freed exactly once.
     * `destination` is the slot actually stored to; `destIsInterface` drives the moved-into-interface
     * flag.
     */
    void TransferPointerOwnershipOnStore(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* destination,
        bool destIsInterface,
        antlr4::ParserRuleContext* ctx);

    /*
     * Closure store (Option A): closures are clone-safe, so a NAMED source is auto-cloned rather
     * than aliased - both sides then own an independent env and each frees exactly once. A `move`
     * source and any temp / call result (null Storage) already transferred ownership and are
     * returned untouched. Shared by the two field-store paths - `=` in ParseAssignmentExpression
     * and brace-init / `<Tag attr=...>` sugar via EmitOneFieldInit - so neither spelling can leave
     * two owners of one env. Returns the value to store (the clone, or `right` unchanged).
     */
    llvm::Value* CloneClosureFromNamedSource(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx);

    /*
     * Produce an INDEPENDENT copy of a copyable-owner value `right` for the copy-on-assign flip:
     * where T is a copyable owner (IsCopyableType), a named-source `dest = src` COPIES instead of
     * moving, leaving the source live. `string` uses the dedicated deep-copy; any other copyable
     * value type routes through copy() (a real overload or the memberwise synth). rightNV names the
     * source; a fresh arg NV (Storage/type only, no CallerName) keeps copy() overload resolution
     * from being confused by the source's named-argument metadata, mirroring CloneClosureFromNamedSource.
     */
    llvm::Value* EmitCopyableOwnerCopy(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx);

    // The copy-vs-move classification shared by the decl-init, reassign, and deref assignment paths.
    enum class AssignSourceKind { Copy, Move };

    /*
     * ONE decision point for `dest = src` where dest is a NON-closure owning value type (struct /
     * string) and `right` is the loaded source struct value. THE FLIP: a copyable owner with a NAMED
     * (non-`move`) source COPIES - returns an independent duplicate, source stays live (outKind=Copy).
     * A non-copyable owner (owns a `unique`, no copy()) - or an explicit `move` source - MOVES:
     * returns `right` unchanged and reports outKind=Move, so the caller consumes the source (zero its
     * storage + MarkVariableMoved). Callers gate that dest is an owning value type and `right` is a
     * struct value; closures clone via CloneClosureFromNamedSource and are handled at their own sites.
     */
    llvm::Value* ClassifyOwningAssignSource(
        llvm::Value* right, const std::string& destTypeName, bool srcIsMove,
        antlr4::ParserRuleContext* ctx, AssignSourceKind& outKind);

    /*
     * Storing a whole `alias` (borrow) value into a struct field: the field's always-run destructor
     * would free a buffer the real owner still holds (double-free). Reported with the precise
     * message ahead of the generic owning-value reject, which would wrongly suggest 'move' - you
     * cannot move out of a borrow. Use '.copy()'. Excludes `string`/`__closure_fat_ptr` - they carry
     * a runtime owned bit (the string-redesign borrow path); the `alias` machinery is only for
     * owning STRUCTS with no runtime bit. A borrow can only dangle when the field could destruct it:
     * a pointer (the pointee is freed) or an owning value type (its destructor frees buffers the
     * real owner holds). A POD struct copy owns nothing, cannot dangle, and has no '.copy()' to
     * suggest - and it reaches here routinely, since a by-reference lambda capture marks every
     * non-deep-copyable struct IsAliasBorrow. Mirrors the gate on the sibling `alias`-return reject.
     * Shared by the two field-store paths - `=` in ParseAssignmentExpression and brace-init /
     * `<Tag attr=...>` sugar via EmitOneFieldInit. The caller gates that the destination IS a field.
     * Returns true when an error was logged.
     */
    bool RejectAliasStoreIntoField(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx);

    /*
     * ONE question for every PERSIST site (field store, brace-init field, return slot, owning-local
     * store, `move` argument): is this source a borrow of storage it does not own, in a shape whose
     * new destroyer could dangle? An `alias` call result (TypeAndValue.IsAlias) or a local bound
     * from one (IsAliasBorrow), excluding `string`/`__closure_fat_ptr` (they carry a runtime owned
     * bit) and POD structs (nothing to free). The three original sites open-coded this identically.
     */
    static bool SourceIsDanglingAliasBorrow(LLVMBackend* compiler,
                                            const LLVMBackend::NamedVariable& nv);

    /*
     * The ADOPTING sites (owning-local store, `move` argument) additionally require the borrow to
     * live in its OWN slot - a named local/global alloca, or a bare `alias` call result with no
     * storage at all. A by-reference lambda capture is also IsAliasBorrow, but its Storage IS the
     * outer owner's address (a load of the env field, never an alloca), so consuming it really does
     * move out of the one owner - measured correct on master and must keep working.
     */
    static bool BorrowAdoptionIsUnsound(LLVMBackend* compiler,
                                        const LLVMBackend::NamedVariable& nv);

    /*
     * The DESTINATION-side twin of the questions above: this binding is an `alias`/mixed-join borrow
     * living in its OWN alloca/global slot. The own-slot clause is the same by-reference
     * lambda-capture carve-out BorrowAdoptionIsUnsound uses - such a capture's Storage IS the outer
     * owner's address, so a store through it correctly drops the outer's old value.
     */
    static bool IsAliasBorrowLocalBinding(const LLVMBackend::NamedVariable& nv);
    static bool DestinationIsAliasBorrowLocal(LLVMBackend* compiler, llvm::Value* destination);

    // Records where a borrow binding was created, so a rebind can prove it runs on every path.
    static void RecordAliasBorrowDeclBlock(LLVMBackend* compiler, LLVMBackend::NamedVariable& nv);

    /*
     * Re-binding a borrow local (`Box k = w.get(); k = makeBox(2);`) hands it a value it now really
     * owns, so the borrow classification must retire or the new value's scope-exit destructor stays
     * suppressed. Retired only when the store is emitted in the binding's DECLARATION block, where
     * every path to scope exit provably ran it; anywhere else (a conditional rebind) the borrow
     * stands and the new value LEAKS, which is the safe direction - the alternative destroys a
     * value the real owner still frees on the path that did not rebind.
     */
    static void RetireAliasBorrowOnRebind(LLVMBackend* compiler, llvm::Value* destination);

    /*
     * Same hazard as RejectAliasStoreIntoField with an owning LOCAL/GLOBAL destination (`other = k`)
     * instead of a field: the destination's always-run destructor would free storage the real owner
     * still holds. The caller gates that the destination destructs (an owning value type).
     * Returns true when an error was logged.
     */
    bool RejectAliasBorrowAdoption(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        const char* destKind,
        antlr4::ParserRuleContext* ctx);

    /*
     * A MIXED '?:' join of an owning-value STRUCT (`c ? makeBox() : borrowed`) may carry a live
     * borrow's bits on the path actually taken, so PropagateTernaryOwnership suppressed the join
     * and ledgered it non-owning. A DECLARATION can receive it (the new local is marked a borrow
     * and its destructor suppressed), but an existing owning local, a struct field, and a return
     * slot all destruct UNCONDITIONALLY with no per-value flag to carry the suppression - the
     * store would hand a second destroyer to storage its real owner still frees. Reject instead.
     * A function declared `alias` is the sanctioned escape hatch for the RETURN slot (it hands the
     * borrow through instead of transferring), so its caller gates on currentFunctionReturnTV.
     * Returns true when an error was logged.
     */
    bool RejectNonOwningStructJoinStore(llvm::Value* right, const std::string& destTypeName,
                                        const char* destKind, antlr4::ParserRuleContext* ctx);

    /*
     * Copying a NAMED owning value into a struct field by value aliases its backing buffer, which
     * both the field's destructor and the source's owner then free (double-free). Ownership is a
     * runtime property, so the compiler can neither safely deep-copy nor auto-move; the user must
     * say which. Shared by the two field-store paths - `=` in ParseAssignmentExpression and
     * brace-init / `<Tag attr=...>` sugar via EmitOneFieldInit - so neither spelling can reach the
     * double-free. The caller gates that the destination IS a field, and that the value is not a
     * closure (clone-safe, handled by CloneClosureFromNamedSource above).
     *
     * THE FLIP: when the source is a COPYABLE owner (IsCopyableType), `right` is replaced with an
     * independent copy in place and the store PROCEEDS (returns false); the source stays live. Only
     * a NON-copyable owner (owns a `unique`, no copy()) is rejected. Returns true when an error was
     * logged.
     */
    bool RejectOwningValueCopyIntoField(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value*& right,
        bool isSelfAssign,
        bool& outCopied,
        antlr4::ParserRuleContext* ctx);

    /*
     * Transfer ownership of a `move`d owning-string SOURCE by nulling its `_ptr` field, so the
     * source's always-run scope-exit destructor is a no-op after the buffer has been handed to
     * persistent storage. Shared by the two field-store paths - `=` in ParseAssignmentExpression
     * and brace-init / `<Tag attr=...>` sugar via EmitOneFieldInit - so a moved string transfers
     * identically through either spelling. Without it, brace-init deep-copies the moved value while
     * `move` has already suppressed the source's destructor, orphaning the original buffer (a leak).
     * Extracted VERBATIM from the `=` path; the caller gates operatorText == "=".
     */
    void TransferMoveStringOwnershipOnStore(
        const LLVMBackend::NamedVariable& rightNV,
        antlr4::ParserRuleContext* ctx);

    // True when the NamedVariable's value is the `string` value type.
    bool NamedVarIsString(const LLVMBackend::NamedVariable& nv);
    bool IsOwningArrayStringElementRead(const LLVMBackend::NamedVariable& nv, llvm::Value* value);
    // Return-position consume/copy of an owning value read out of another object's storage.
    bool ReturnSourceIsIndirectOwningLvalue(const LLVMBackend::NamedVariable& nv, llvm::Value* value);
    bool FieldPathRootIsFrameLocal(llvm::Value* storage);

    // Stamp owned-string ownership onto an expression result and restore the
    // "last call returns owned" flag for downstream consumers. `savedOwned` is the
    // flag value captured before this expression was evaluated. If the expression
    // ended in a call that returns an owned heap string (e.g. copy() / operator+),
    // mark the result IsOwningString so passing it directly as a `move string`
    // argument skips the defensive re-copy in CreateOverloadedFunctionCall (which
    // would clone the buffer and orphan the freshly returned one - a leak). The
    // flag is left reflecting this expression's own call when it made one, else
    // restored to the prior value so existing consumers see unchanged semantics.
    LLVMBackend::NamedVariable FinishAssignmentExpressionNamed(
        LLVMBackend::NamedVariable nv, bool savedOwned);

    // Returns a NamedVariable (preserving TypeName) for simple single-child expression chains.
    // Used by ParseDeclaration to get the struct TypeName for struct->interface upcasting.
    // Falls back to value-only for complex expressions (ternary, binary ops, etc.).
    LLVMBackend::NamedVariable ParseAssignmentExpressionNamed(CFlatParser::AssignmentExpressionContext* ctx,
                                                              ResultUse use = ResultUse::Value);

    /*
     * Provable shape mismatch on a DIRECT class->interface upcast (decl-init, '=', return, and
     * brace/element init). The shape test and the message live on LLVMBackend
     * (DescribePointerShapedInterfaceSource) so the call-argument boxing sites there reject the
     * same spellings with the same diagnostic. Returns true (and logs) when rejected.
     *
     * Its "true" return never actually reaches a caller: LogErrorContext never returns (it throws,
     * or exits through FailCompilation). Every call site still guards on it (an empty 'else if'
     * body, or a '&& !Reject(...)' conjunct) so that if LogErrorContext ever started returning,
     * control would not fall through into the boxing code and emit a fat pointer built over
     * wrong-shaped storage.
     */
    bool RejectPointerShapedInterfaceUpcast(antlr4::ParserRuleContext* errCtx,
                                            const LLVMBackend::TypeAndValue& src,
                                            const std::string& interfaceName);

    /*
     * The one pointer-shaped source RejectPointerShapedInterfaceUpcast never gets to see: an
     * `int[3]` and friends never reach BoxConcreteIntoInterface at all, because every boxing site
     * only calls it once StructImplementsInterface() said yes - and a primitive implements
     * nothing. The plain-assignment chain then fell out of its if/else with a raw ptr and stored
     * it into the fat slot.
     *
     * Polarity: this proves all three of - the target is a REGISTERED interface, the source
     * element is a BUILTIN primitive, and the source is pointer-SHAPED - before rejecting. A class
     * element, a generic type parameter, an unresolved name, a plain scalar, an already-fat value:
     * none of them is proven wrong here, so all of them return false and keep today's behaviour.
     */
    bool RejectPrimitiveShapedInterfaceUpcast(antlr4::ParserRuleContext* errCtx,
                                              const LLVMBackend::TypeAndValue& src,
                                              const std::string& interfaceName);

    /*
     * Where the object a boxing site is about to box actually lives. `srcNV` decides the cases the
     * IR cannot: a borrowed pointer parameter and an owning local both arrive as a load.
     */
    LLVMBackend::InterfaceBoxSource ClassifyInterfaceBoxSource(llvm::Value* dataPtr,
                                                              const LLVMBackend::NamedVariable* srcNV,
                                                              bool ownershipTransferred);

    /*
     * Retire the OWNING source behind an already-boxed VALUE when the name-keyed transfer could
     * not run - the spelling erased the binding (parentheses, a cast chain, anything that drops
     * Storage / IsOwning off the NamedVariable). Keyed on value identity, so no new spelling can
     * defeat it the way SoleCastOperandOf's syntactic walk could.
     *
     * Polarity: this PROVES ownership before acting - `value` must be a pointer LoadInst whose
     * slot IS the Storage of a live binding that declares itself owning (IsOwningValue). A call
     * result, a phi, a field GEP, a borrow: none of them is proven owning, so all are left alone
     * and keep today's behaviour. The marks mirror what the plain named spelling already emits, so
     * `IShape s = (c);` and `IShape s = c;` diagnose a later use of `c` identically.
     */
    bool RetireOwningSourceOfBoxedValue(llvm::Value* value);

    /*
     * The shared class->interface boxing site: decl-init, 'as', the assignment STATEMENT and
     * brace/element init all route here, so this is the only place a class is boxed into an
     * interface from a single concrete source and the guards cannot drift apart again (the '?:'
     * and '??' JOINS box per arm through BoxInterfaceJoinArms, which applies the same ledger).
     * Applies the implements check, the pointer-shape rejection, data-pointer selection and the
     * owning-source transfer in that order, then ledgers what it boxed so later guards can ask
     * instead of re-deriving it from emitted IR. `srcNV` is null when the operand has no named
     * binding (a '?:' arm, an arithmetic result); the binding-dependent guards are then skipped by
     * construction, and the VALUE-keyed retirement below covers the ownership half.
     * `adoptsOwnership` is false for the sites whose destination runs its own ownership
     * bookkeeping (a struct FIELD store refcounts an escaping `new` instead of nulling the source),
     * so this helper must not pre-empt it.
     * Every LogError below throws, so no partially-built fat value or ledger entry escapes.
     */
    llvm::Value* BoxConcreteIntoInterface(antlr4::ParserRuleContext* errCtx, llvm::Value* srcValue,
                                          bool sourceIsPointer, const std::string& structName,
                                          const std::string& interfaceName,
                                          const LLVMBackend::NamedVariable* srcNV,
                                          bool adoptsOwnership = true);

    /*
     * PROVE, from ONE binding, that a NAMEABLE other owner frees the object being boxed. Three
     * positive proofs and nothing else: the binding frees it itself at scope exit (IsOwning, so the
     * box would be a second owner); it reads a `unique` FIELD, whose synthesized destructor frees
     * it; or it is a non-move pointer PARAMETER, whose caller owns it.
     *
     * `IsAliasBorrow` is deliberately excluded, having been measured false-rejecting a correct
     * program: it is the OPPOSITE of this question. It means this binding's scope-exit free is
     * SUPPRESSED, i.e. it frees NOTHING - `alias` hands the lifetime to the caller to manage by
     * hand, so `alias T* e = makeT(); IS s = e; delete s;` is the CORRECT way to release it and
     * rejecting it both false-rejects and leaks.
     *
     * A local that received its `new` in a LATER statement is not a proof: it does not carry
     * IsOwning, and the box is its ONLY owner. An unannotated copy off an OWNING local
     * (`T* b = c;`) carries no IsOwning either, but IS a proof - established at its DECLARATION and
     * re-asked here through OwningLocalCopyStillAliases, which requires the source to still be a
     * live non-rebound owner. The two are only distinguishable at the declaration, not here.
     *
     * ORDER IS LOAD-BEARING below the IsOwning gate. The two clauses a plain '=' REFRESHES -
     * BorrowsOwnedElement (re-established by SetVariableBorrowsOwnedElement on that same store) and
     * InheritedKeepsOwner (set by MarkPointerRebound from the RHS binding's own proof) - are asked
     * ABOVE the PointerRebound retirement, because both describe the store that set the bit and are
     * therefore fresher than it. The two DECLARATION-time clauses - IsBorrowed and the parameter
     * test - are asked BELOW it, because a rebound binding no longer points at what its declaration
     * established an owner for.
     */
    bool BindingKeepsOwnershipOfBoxedObject(const LLVMBackend::NamedVariable* nv) const;

    /*
     * The AST name to blame, taken from the binding that ACTUALLY proved the claim - which is often
     * not `srcNV`: a self-field read and a container-element read both arrive here as a transient
     * NamedVariable stripped of the flags, and only the binding recovered from the loaded slot
     * carries them. Describing `srcNV` regardless printed the local's own name for both.
     * Never an LLVM value name - those carry '.N' uniquifying suffixes absent from the source.
     */
    std::string DescribeBoxedSourceOwner(llvm::Value* dataPtr,
                                         const LLVMBackend::NamedVariable* srcNV) const;

    /*
     * The owner a `lhs = rhs` store hands ACROSS to the LHS, or empty when the RHS proves nothing.
     * Recovered by STORAGE IDENTITY, never spelling: a name-keyed lookup off `rightNV.CallerName`
     * would resolve a field read (`p = q->next`) to its BASE object and blame the wrong binding.
     * Empty is the accept direction, so every shape that does not resolve retires as before.
     */
    std::string DescribeAssignedSourceOwner(const LLVMBackend::NamedVariable& rightNV) const;

    /*
     * The owner a '?:' / '??' pointer JOIN hands to whatever receives it, or empty when it proves
     * nothing. BOTH-ARMS rule, the same one the per-arm interface boxing ledger settled: EVERY
     * non-null arm must resolve to a live binding that keeps ownership, so a MIXED join (one arm a
     * fresh `new`, one an unresolvable call result) is ACCEPTED rather than rejected on a may-alias.
     * A null arm owns nothing, so it neither proves nor blocks. Empty is the accept direction.
     */
    std::string JoinArmsKeepOwner(llvm::Value* joined,
                                  std::vector<llvm::Value*>* slotsOut = nullptr) const;

    /*
     * POSITIVE proof that a join ARM holds null: the null literal, or a load off a non-escaping
     * alloca whose every store parks a null (or a load off another such slot). Such an arm owns
     * nothing, exactly as the literal owns nothing, so both join proofs skip it as NEUTRAL rather
     * than counting it as an arm that proves no owner. "We could not tell what it holds" must NEVER
     * answer true here: that would make a join of two unresolvable arms provable and false-reject.
     * A slot with no store at all, an escaping slot, or any non-null store answers false.
     */
    bool JoinArmIsProvablyNull(llvm::Value* arm, int depth = 0) const;

    /*
     * Re-ask a recorded join proof at a CONSUMER: EVERY arm slot must STILL resolve to a binding
     * that proves another owner. Recording alone is not enough - nulling or rebinding an ARM
     * (`c = nullptr;` after `T* b = cond ? c : c;`) makes the join's receiver the sole owner, and a
     * rejection there is a false one whose remedy leaks. This is the arm-side twin of
     * OwningLocalCopyStillAliases, and it uses the same explicit `!PointerRebound` test: a rebound
     * binding keeps its IsOwning flag, which BindingKeepsOwnershipOfBoxedObject answers ABOVE its
     * own retirement, so asking that helper alone would never retire an arm.
     * An unresolvable, dead or self-referential arm answers false - the accept direction.
     */
    bool JoinArmsStillKeepOwner(const LLVMBackend::NamedVariable& nv, int depth = 0) const;

    /*
     * The binding that PROVES some other owner frees the boxed object, or null when none does.
     * `srcNV` is the source binding when the spelling carried one; parentheses, a join arm, a
     * self-field read and a container-element read all arrive without the deciding flags, so the
     * binding is also recovered from the boxed VALUE - a pointer load off a live slot. Both routes
     * are positive proofs, so asking both only widens what can be PROVEN, never what is assumed.
     */
    const LLVMBackend::NamedVariable* ProvingBindingForBoxedSource(
        llvm::Value* dataPtr, const LLVMBackend::NamedVariable* srcNV) const;

    // The boxing site's verdict for InterfaceBoxRecord::SourceKeepsOwner.
    bool ClassifyBoxedSourceKeepsOwner(llvm::Value* dataPtr,
                                       const LLVMBackend::NamedVariable* srcNV,
                                       bool ownershipTransferred) const;

    /*
     * PROVE that the fat value about to be bound to an interface local is a box the frame does not
     * own. A join ('?:' / '??') is a PHI of the per-arm boxes: EVERY non-null arm must resolve to a
     * ledgered record whose source keeps ownership, so a MIXED join - where one arm owns nothing
     * else - is accepted rather than rejected on a may-alias. A null arm owns nothing and is
     * skipped. Anything with no ledger entry at all answers false and keeps compiling.
     *
     * `sourceNames` collects EVERY arm that keeps an owner, because the taken arm is a runtime
     * choice: naming only the first would state as fact something the run can contradict.
     */
    bool InterfaceBoxValueIsProvablyBorrowed(llvm::Value* fatValue,
                                             std::vector<std::string>& sourceNames);

    // "'a'", "'a' or 'b'", "'a', 'b' or 'c'" - every owner the value can carry, never just one.
    std::string DescribeInterfaceBoxOwners(const std::vector<std::string>& names) const;

    /*
     * The one binding-site hook: tag an interface LOCAL with the provenance of the box it was just
     * handed. A NULL box is neutral - it owns nothing, deleting it is a no-op - so it neither arms
     * nor poisons the tag. Everything else either proves a borrow or poisons the local for good.
     */
    void TagInterfaceBoxProvenance(const std::string& varName, llvm::Value* fatValue);

    // True when any boxing site reachable through `fatValue` (following '?:' joins) boxed a heap
    // object whose ownership it took over. Answers a join through the arms' data pointers.
    bool FatValueOwnsHeapBox(llvm::Value* fatValue);

    /*
     * Inverse of the fresh-allocation return check, scoped to INTERFACE returns. A concrete
     * implementer pointer boxed into an interface return is still a bare pointer at the return
     * site (boxing happens just after). The caller receives a { vtable, data } fat pointer with
     * no idea it owns the boxed heap object, so it never frees it (leak). Require
     * 'move <interface>' so the transfer is explicit and the caller knows to 'delete' it
     * (interface locals are not auto-destructed). Raw and struct 'T*' returns are deliberately
     * not covered - owning-pointer factories are a valid manual-memory pattern in this codebase.
     *
     * A `return p as IShape;` operand is ALREADY a fat pointer, so the bare-pointer test cannot
     * see the heap object it carries; the boxing site's provenance ledger answers for it. A
     * frame-local arm is the worse defect and has its own unconditional diagnostic later on the
     * return path, so that shape is handed to it rather than blamed on ownership here.
     */
    bool ReturnLeaksOwnershipIntoInterface(llvm::Value* right, LLVMBackend* compiler);

    // One arm of an interface-bound join: the incoming value and the predecessor block whose
    // terminator the arm's box is emitted before.
    struct InterfaceJoinArm
    {
        llvm::Value* Value = nullptr;
        llvm::BasicBlock* Block = nullptr;
    };

    /*
     * The arms of a pointer JOIN of either spelling, or empty when `value` is not a join. A '?:'
     * is a PHI whose incoming edges ARE the arms; a '??' joins through a slot, so its arms are
     * unrecoverable from the IR and come from the lowering's ledger.
     */
    std::vector<InterfaceJoinArm> CollectPointerJoinArms(llvm::Value* value) const;

    static constexpr int kMaxNestedJoinDepth = 8;

    /*
     * Can every LEAF arm of a NESTED join be boxed into `interfaceName`? An arm that is itself a
     * join - `a ?? b ?? c` (which parses as `a ?? (b ?? c)`), `x ? (y ? p : q) : r`, or either
     * spelling nested in the other - has no concrete class of its own, so the flat resolve loop
     * in BoxInterfaceJoinArms cannot name it. This answers for the whole subtree WITHOUT emitting
     * IR, because a partial rewrite would leave half-boxed IR behind on a later arm's failure.
     *
     * Deliberately NOT a ledger-flattening of the chain into one arm list: the arms of an inner
     * '??' live in blocks that branch to the INNER resume block, so they are not predecessors of
     * the outer join point and a flat phi over them is invalid IR. The nested join is boxed in
     * its own resume block instead - see the recursion in BoxInterfaceJoinArms.
     */
    bool NestedJoinArmsBoxable(llvm::Value* value, const std::string& interfaceName,
                               std::string* armFailure, int depth = 0);

    /*
     * Every LEAF arm's concrete class of a join, recursing into arms that are themselves joins.
     * False when an arm resolves to neither a registered data structure nor a boxable join, which
     * is the accept-nothing direction: the caller then leaves the argument untouched.
     */
    bool CollectJoinArmClasses(llvm::Value* value, std::vector<std::string>& out, int depth = 0);

    /*
     * Box the arms of a pointer JOIN ('?:' or '??') into an interface fat pointer. A join carries
     * no NamedVariable TypeName, so the ordinary upcast is skipped and a raw `ptr` would be bitcast
     * into the fat struct type - invalid IR. Box each arm inside the arm's OWN block, which also
     * covers arms with DIFFERENT concrete classes (each gets its own vtable), and join the fat
     * pointers with a phi inserted at `joinPoint`. A null arm contributes a null fat pointer. An
     * arm's concrete class comes from ResolvePointerElementTypeName, which answers a BORROWED arm
     * (a plain load, absent from the `new`-site ledger) from the loaded binding's declared type -
     * otherwise that arm would fail to resolve and the caller would bitcast a raw `ptr` into the
     * fat struct. Returns nullptr when this does not apply, leaving the caller's normal path
     * untouched. `armFailure` (when given) is set ONLY for a pointer join that genuinely targets
     * this interface but has an arm that cannot be boxed - the caller must diagnose that instead of
     * bitcasting a raw `ptr` into the fat struct. It stays empty for every "does not apply" bail.
     *
     * `transferArmOwnership` is set ONLY by the `move`-interface RETURN path: the box escapes the
     * frame there, so an OWNING arm local is nulled inside its own arm block and the untaken arm
     * still runs its ordinary null-guarded scope-exit free (no leak, no double free). Every other
     * caller keeps the box inside the frame and must not disturb the arms' owners - a join into a
     * plain interface local is a BORROW by design (test_move's iface_ternary_thin_* legs pin that
     * the owner stays alive and frees exactly once), so no transfer may happen there. It also arms
     * the per-arm ownership check that reports through `armNotOwned`: transferring an arm that owns
     * nothing would make the caller a second owner of a live borrow, so the caller must raise the
     * ordinary not-owned return diagnostic instead of boxing.
     */
    llvm::Value* BoxInterfaceJoinArms(const std::vector<InterfaceJoinArm>& arms,
                                      llvm::Value* joinValue, llvm::Instruction* joinPoint,
                                      const std::string& interfaceName, std::string* armFailure,
                                      bool transferArmOwnership, bool* armNotOwned);

    // The '?:' spelling of a join: the arms ARE the phi's incoming edges. See BoxInterfaceJoinArms.
    llvm::Value* UpcastTernaryPhiToInterface(llvm::Value* right, const std::string& interfaceName,
                                             std::string* armFailure = nullptr,
                                             bool transferArmOwnership = false,
                                             bool* armNotOwned = nullptr);

    /*
     * The '??' spelling of the same join. Unlike '?:' it lowers through a SLOT, so the joined value
     * is a plain load and the arms are unrecoverable from the IR - the lowering ledgers them
     * (RegisterNullCoalesceJoin) and this reads them back. Without it the boxing chain found no
     * TypeName on the result, skipped every branch, and stored a raw `ptr` into the fat slot: a
     * module-verifier failure with no source diagnostic. The load is the first instruction of the
     * join's resume block, so the fat phi inserted before it is still the block's leading phi.
     */
    llvm::Value* UpcastNullCoalesceToInterface(llvm::Value* right, const std::string& interfaceName,
                                               std::string* armFailure = nullptr,
                                               bool transferArmOwnership = false,
                                               bool* armNotOwned = nullptr);

    /*
     * Box a pointer JOIN of either spelling ('?:' or '??') into `interfaceName`. Returns nullptr
     * when neither applies, leaving the caller's normal path untouched. `joinSpelling` (when given)
     * names the spelling that produced a failure, so the diagnostic blames the operator the source
     * actually wrote; it is left alone unless this call sets `armFailure`.
     *
     * `transferArmOwnership` / `armNotOwned` are threaded through to BOTH spellings unchanged: the
     * `move`-interface RETURN path is the only caller that sets them, and a '??' return escapes the
     * frame exactly as a '?:' return does, so hardcoding "no transfer" for one spelling would drop
     * arm ownership on that half. At most ONE spelling can ever run (a PHINode is never a LoadInst),
     * so the early return on `armNotOwned` guards nothing - it is only there to name the spelling
     * for the caller's diagnostic, exactly like the `armFailure` return below it.
     */
    llvm::Value* UpcastPointerJoinToInterface(llvm::Value* right, const std::string& interfaceName,
                                              std::string* armFailure,
                                              std::string* joinSpelling = nullptr,
                                              bool transferArmOwnership = false,
                                              bool* armNotOwned = nullptr);

    /*
     * A pointer JOIN of either spelling in ARGUMENT position. A join carries no TypeName - the
     * '??' spelling lowers through a SLOT so its result is a plain load, and the '?:' spelling is
     * a bare PHI - and the overload scorer's interface clause needs one to score a class against
     * an interface parameter, so `take(z ?? a)` was a false rejection before any boxing could run.
     * The target interface is not known until overload selection, so resolve it HERE from the
     * candidate parameters at this position: the one interface every arm's class implements. Then
     * box per arm (each arm boxes in its own block with its own vtable, so mixed-class arms work)
     * and hand the scorer a genuine INTERFACE argument. Arms that are THEMSELVES joins (a chained
     * '??', a join nested in a join arm) recurse - their class comes from their own leaf arms.
     *
     * Deliberately NOT a bare TypeName stamp of the arms' class. Measured when this was written:
     * stamping the CLASS made a by-value `f(Circle c)` parameter score a PERFECT match on a
     * `Circle*` and lower a raw pointer into a struct slot - a module-verifier dump with no source
     * location; and where an interface overload also existed, the by-value candidate displaced the
     * very interface call this exists to enable. `TypeAndValue::IsTypeMatch` has since gained a
     * pointer gate, which may cover the first half - the second half is reason enough on its own.
     * An interface-typed argument cannot match a by-value class parameter at all, which is correct.
     *
     * Returns nullptr - leaving the argument untouched, so the ordinary LOCATED "no overload
     * matches" diagnostic stands - for every case it cannot prove: not a ledgered join, an arm
     * whose class will not resolve, no interface parameter all arms implement, two candidates
     * offering DIFFERENT interfaces here, or ANY candidate taking a POINTER of any kind at this
     * position. That last bail was once narrowed to pointers-to-a-CLASS, which silently STOLE the
     * call from `f(void*)`, `f(char*)` and `q(int*)` competitors - all working programs before -
     * because boxing runs BEFORE the scorer and those parameters would have won it. The narrowing
     * bought nothing: a `T*` parameter is no more the join's natural home than a `void*` one.
     *
     * NOT inverted to "bail unless EVERY candidate parameter here is an interface", which looks
     * like the safer polarity and is not: `f(Circle c)` / `f(IShape s)` has a non-interface
     * candidate, so the inversion would refuse to box and leave BOTH candidates unmatchable - the
     * false rejection this whole helper exists to remove. A by-value class parameter cannot take a
     * pointer at all, so there is nothing to steal there; a pointer parameter can, so there is.
     */
    llvm::Value* BoxPointerJoinArgument(
        const std::vector<const LLVMBackend::TypeAndValue*>& paramsAtPosition,
        llvm::Value* argValue, std::string& ifaceNameOut);

    // True when every arm of a '?:' phi is a null pointer, so the joined value owns nothing.
    bool IsAllNullPhi(llvm::Value* value) const;

    // Soundness gate for the thin int[] array-view: reject binding a raw `T*` into a `T[]`.
    // A view carries a noalias contract - it always spans a whole, distinct allocation - which
    // lets the -O2 vectorizer drop its runtime alias check. A raw `T*` may alias or point
    // partway into a buffer (it has pointer arithmetic), so laundering one into a view would
    // forge that contract and silently miscompile. The safe direction (T[] -> T* decay) is
    // always allowed; view -> view and `new T[n]` -> view are allowed (RHS is itself a view).
    // Returns true (and logs) when the bind is rejected. `target` is the declared type of the
    // assignment/parameter/return slot; `rhs` is the value being bound into it.
    bool RejectRawPointerToArrayView(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::TypeAndValue& rhs);

    llvm::Value* ParseAssignmentExpression(CFlatParser::AssignmentExpressionContext* ctx);

    // Box a THIN '?:' arm (`new T()` / `nullptr` / a borrowed pointer) into the interface fat
    // struct, matching a sibling arm already fat (e.g. `move` of an interface-typed local).
    // Caller must position the builder in the thin arm's own block first (atTrue/atFalse).
    llvm::Value* BoxTernaryThinArmToInterface(llvm::Value* thinValue, const std::string& interfaceName,
                                              std::string& armFailure);

    /*
     * The one join arm that CONSUMES the code evidence instead of carrying it: a `?:` whose other
     * arm is a `string` wraps the pointer arm as a NUL-terminated buffer, so the machine code is
     * read as text and the join delivers a `string` value no downstream gate can question. Refuse
     * it here, where the arm value is still ledgered. Proof-only: an unledgered pointer arm (a
     * literal, a `char*`) keeps wrapping exactly as before.
     */
    bool RejectCodeValueTernaryStringArm(CFlatParser::ConditionalExpressionContext* ctx,
                                         llvm::Value* armValue, size_t armOccurrence);

    /*
     * Align the two '?:' arm values onto one LLVM type so the join (a PHI, or a select in the
     * constant-context fallback) has matching operand types. `atTrue`/`atFalse` reposition the
     * builder into the block that produced the corresponding arm - required by the branching
     * lowering, where a coercion instruction must land on its own arm's path, not at the join.
     * Returns false, after logging, on a genuine mismatch.
     */
    bool UnifyTernaryArmTypes(CFlatParser::ConditionalExpressionContext* ctx,
                              llvm::Value*& trueValue, llvm::Value*& falseValue,
                              const std::function<void()>& atTrue,
                              const std::function<void()>& atFalse,
                              size_t trueOccurrence, size_t falseOccurrence);

    /*
     * Make one '?:' arm's string value an INDEPENDENT owned buffer inside the arm's own block.
     * The raw value may alias an owning field/local buffer (binding the result would free it
     * twice) and a call-result temp has no other owner. Deep-copy it, then free the consumed
     * temp here - the join cannot free it, since an arm value does not dominate the join.
     */
    llvm::Value* AdoptTernaryStringArm(LLVMBackend* compiler, llvm::Value* value, bool& deepCopied);

    /*
     * Close one '?:' arm. ORDER IS LOAD-BEARING and is the whole point of this function: the deep
     * copy MUST be emitted before the arm's flush. An arm value can BORROW from a temp the flush
     * destroys - `c ? makeToken("hi").text : "-"` yields a string aliasing the owning-struct temp's
     * buffer - so copying afterwards would memcpy from freed memory (use-after-free), and not
     * copying at all would leak the parent. Then flush what the arm registered: those entries are
     * keyed to the arm's block, which does not dominate the join, so the end-of-statement flush
     * would skip them (OwnedTempDominatesHere) and every buffer would leak. The yielded value is
     * kept unless the copy above already consumed it.
     */
    void FinishTernaryArm(LLVMBackend* compiler, llvm::Value*& value,
                          const LLVMBackend::OwnedTempMark& mark, bool& deepCopied);

    /*
     * Lower `cond ? a : b` as a real branch, mirroring the '??' lowering directly below: the true
     * arm is emitted into its own block, the false arm into its own, and the two join through a PHI
     * in the resume block. Only the selected arm's code runs, so a `move` or a dereference in the
     * other arm has no effect - the eager CreateSelect form executed both unconditionally.
     */
    LLVMBackend::TypedValue ParseTernaryBranches(
        CFlatParser::ConditionalExpressionContext* ctx,
        const LLVMBackend::TypedValue& condTv,
        CFlatParser::ExpressionContext* expressionTrueCtx,
        CFlatParser::ConditionalExpressionContext* expressionFalseCtx);

    LLVMBackend::TypedValue ParseConditionalExpression(CFlatParser::ConditionalExpressionContext* ctx);

    LLVMBackend::TypedValue ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx);

    LLVMBackend::TypedValue ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx);

    LLVMBackend::TypedValue ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx);

    LLVMBackend::TypedValue ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx);

    LLVMBackend::TypedValue ParseAndExpression(CFlatParser::AndExpressionContext* ctx);

    // `ifaceVal == nullptr` / `!= nullptr`: rewrite the fat-ptr operand to its data pointer so the
    // comparison is a plain pointer compare. Only fires when the other operand is a null constant.
    void LowerInterfaceNullCompare(antlr4::ParserRuleContext* ctx,
                                   LLVMBackend::TypedValue& lv, LLVMBackend::TypedValue& rv);

    LLVMBackend::TypedValue ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx);

    // The single-child multiplicative body: reduce a parsed cast-expression binding to the
    // TypedValue the arithmetic chain hands upward. Shared so an 'as' operand can keep the
    // NamedVariable and still produce byte-identical TypedValue state.
    LLVMBackend::TypedValue TypedValueOfNamedOperand(LLVMBackend::NamedVariable& namedVar,
                                                     antlr4::ParserRuleContext* ctx);

    /*
     * The castExpression an 'is'/'as' operand reduces to when relational -> shift -> additive ->
     * multiplicative is a pure single-child passthrough. Parsing it through ParseCastExpression
     * keeps the source's NamedVariable (Storage, IsOwning, CallerName, TypeAndValue), which every
     * boxing guard needs and which the TypedValue-returning chain discards. Returns nullptr for
     * any other shape (a '?:', an arithmetic operand); those legitimately have no named source.
     */
    CFlatParser::CastExpressionContext* SoleCastOperandOf(CFlatParser::RelationalExpressionContext* relCtx);

    LLVMBackend::TypedValue ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx);

    // Returns the type descriptor pointer loaded from vtable[0].
    // Works whether interfaceValue is an aggregate {i8*,i8*} or a pointer to one.
    llvm::Value* LoadTypeDescFromInterface(llvm::Value* interfaceValue, antlr4::ParserRuleContext* ctx);

    // Returns the registered struct/class name if elemType names a concrete data structure
    // (a plain T* source), as opposed to the shared interface fat-pointer struct or a
    // primitive. Returns "" when the source is not a concrete struct/class pointer.
    std::string ConcreteStructNameFromElemType(llvm::Type* elemType, LLVMBackend* compiler);

    // A class VALUE operand (e.g. `s as IMore` where s is a stack `Impl`) arrives as the loaded
    // aggregate and carries no elemType - only pointer sources get one. Its concrete type is
    // still statically known, so recover the name from the value's own type.
    std::string ConcreteStructNameFromValue(llvm::Value* value, LLVMBackend* compiler);

    /*
     * Source categories for an 'is' / 'as' operand. The old code inferred "this must be an
     * interface fat pointer" from the ABSENCE of a concrete struct name, so every shape the two
     * name helpers did not recognise (a pointer '?:' join, a fixed-array slot) silently took the
     * fat-pointer path and read unrelated storage as {vtable,data}. Classification is positive
     * now: a shape that matches no category is diagnosed instead of falling through.
     */
    enum class CastSourceKind
    {
        ConcretePointer,     // T* whose elemType names a registered class
        ConcreteValue,       // a loaded T aggregate of a registered class
        TernaryPointerJoin,  // '?:' phi of concrete pointers - box/answer per arm
        PointerShaped,       // T[N] and friends: no single instance to box
        InterfaceValue,      // a real fat pointer (or a pointer to one)
        Unknown
    };

    /*
     * Fill `shape` for a source that is pointer-SHAPED rather than one instance: a fixed `T[N]`,
     * a nested `T[N][M]`, or a `T**`. The fields mirror what DescribePointerShapedInterfaceSource
     * reads, so the rejection message comes out byte-identical to the plain-assignment spelling -
     * including reporting only the OUTER dimension of a nested array. A fixed array arrives in
     * three guises: the loaded aggregate, the decayed `getelementptr [N x %T]`, or (for a global)
     * the GlobalVariable itself. Returns false when the source is not one of these shapes.
     */
    bool ClassifyPointerShapedSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                     LLVMBackend::TypeAndValue& shape);

    /*
     * Classify an 'is'/'as' source. `structName` is set for the two concrete kinds; `shape` is
     * set for PointerShaped and is the spelling used in the shared rejection message.
     *
     * Unknown is REACHABLE and is a hard error, not a dead bucket. Known spellings that land
     * there: the `select` a chained `(x as C) as I` lowers to, the join a `??` produces, and any
     * other bare `ptr` with no elemType. All of them miscompiled into a bogus fat pointer before
     * this classification existed, so the error is the improvement - but do not assume the bucket
     * is empty.
     */
    CastSourceKind ClassifyCastSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                      std::string& structName, LLVMBackend::TypeAndValue& shape,
                                      const std::string& srcTypeName = {});

    // Concrete class of every arm of a pointer '?:' join. Returns false (with `failure` set) when
    // an arm's class cannot be resolved. A null arm yields an empty name and is never an instance.
    bool ResolveTernaryArmClasses(llvm::Value* value, LLVMBackend* compiler,
                                  std::vector<std::string>& armTypes, std::string& failure);

    // Join per-arm i1 answers back onto a pointer '?:'. Uniform answers fold to a constant;
    // a mixed join becomes an i1 phi placed alongside the pointer phi it mirrors.
    llvm::Value* JoinTernaryArmPredicates(llvm::Value* value, const std::vector<bool>& answers,
                                          LLVMBackend* compiler);

    // Boxing a class value into an interface needs the object's ADDRESS. A loaded value came
    // straight from its storage, so reuse that; anything else (a call result) is spilled.
    llvm::Value* AddressOfClassValueOperand(llvm::Value* value, LLVMBackend* compiler);

    llvm::Value* GenerateIsCheck(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType = nullptr,
                                  const std::string& srcTypeNameIn = {});

    llvm::Value* GenerateSafeCast(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType = nullptr,
                                  const LLVMBackend::NamedVariable* srcBinding = nullptr,
                                  const std::string& srcTypeNameIn = {});

    LLVMBackend::TypedValue ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx);

    // Walk a single-child expression chain to the leaf Identifier terminal.
    // Returns the identifier name, or "" if the expression is complex (e.g., arithmetic, member access).
    std::string TryGetSimpleIdentifier(antlr4::ParserRuleContext* ctx);

    // Find the llvm::Function* for a method named `methodName` whose first parameter is `stream`.
    llvm::Function* FindStreamMethodFn(LLVMBackend* compiler, const std::string& methodName);

    // Wire p1.onStdout to write into the stream. Called for `p1 >> s`.
    void EmitProgramToStreamWire(const std::string& progName,
        llvm::Value* progStorage, llvm::Value* streamStorage,
        antlr4::ParserRuleContext* ctx);

    // Wire p2.onStdin to read from the stream. Called for `s >> p2`.
    void EmitStreamToProgramWire(llvm::Value* streamStorage,
        const std::string& progName, llvm::Value* progStorage,
        antlr4::ParserRuleContext* ctx);

    // Allocate a lightweight (uninitialized) arena_channel shell on the heap and return the
    // typed pointer. The shell's _ring._cap is 0, so its send/recv are silent no-ops until
    // init() is called. Used to default a program's inbox/outbox to a non-null self-loopback
    // shell, so an unwired recv()/send() can never deref a null `this`. The builder must be
    // positioned inside a function (this emits malloc + ctor calls). Returns the LLVM null
    // arena pointer if the ctor/malloc are unavailable (degrades to the old null default).
    llvm::Value* EmitArenaChannelShellAlloc(LLVMBackend* compiler);

    // Wire `producer.outbox = consumer.inbox` for `producer >> consumer` (program-owned
    // arena_channel piping). Both inbox and outbox are non-null lightweight shells allocated in
    // each program's ctor (self-loopback default); this only runs when both programs opted in via
    // useChannel (see the `>>` call site). It init()s the consumer's existing inbox shell -
    // upgrading it to a live ring - then rebinds the producer's outbox from its own self-loopback
    // shell to point at that inbox. init() is idempotent, so fan-in (`a >> c; b >> c`) wires the
    // one channel c owns without re-initializing it. The producer's own inbox shell is untouched
    // (freed by the producer's teardown); the rebound outbox is a borrowed handle, never freed.
    void EmitProgramToProgramArenaWire(const std::string& producerName, llvm::Value* producerStorage,
        const std::string& consumerName, llvm::Value* consumerStorage,
        antlr4::ParserRuleContext* ctx);

    // Wire stdout->stdin stream piping for direct `producer >> consumer` (no explicit `stream` in
    // the middle). Synthesizes a hidden `stream` local - default-constructed, init()ed, and
    // registered for ~stream() at scope exit so its buffers are freed - then reuses the same
    // EmitProgramToStreamWire / EmitStreamToProgramWire helpers as the explicit `p >> s; s >> q`
    // form. The synthesized stream is marked `_autoClose` so the producer's run() trampoline calls
    // close() after main() returns (the analogue of the explicit form's manual `s.close()`); the
    // user therefore writes only `p >> q`, then `p.run(); q.run(); p.WaitForExit(); q.WaitForExit();`.
    // No-op (arena channel still wired) when stream.cb is not imported.
    void EmitProgramToProgramStreamWire(const std::string& producerName, llvm::Value* producerStorage,
        const std::string& consumerName, llvm::Value* consumerStorage,
        antlr4::ParserRuleContext* ctx);

    // True if functionTable has an `opName` overload whose first parameter type matches
    // typeName. Used to decide whether a shift operator should dispatch to an overload
    // (member or free) instead of a primitive bit-shift.
    bool HasOperatorOverloadForFirstParam(const std::string& opName, const std::string& typeName);

    LLVMBackend::TypedValue ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx);

    LLVMBackend::TypedValue ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx);

    // Diagnose move-flag mismatch when assigning a named function to a typed function pointer.
    // E.g. `function<void(move T*)> fp = borrowFn;` is rejected because borrowFn declares a borrow param.
    void VerifyFuncPtrAssignmentMoveFlags(const std::string& funcName,
                                          const LLVMBackend::TypeAndValue& funcPtrType,
                                          antlr4::ParserRuleContext* ctx);

    // If `loaded` is a load instruction reading an array-view element (NoaliasScopeId >= 0), tag it
    // with the view's alias scope so the vectorizer can prove disjointness. No-op otherwise.
    llvm::Value* TagViewElementAccess(llvm::Value* loaded, const LLVMBackend::NamedVariable& namedVar);

    // Lower a span<T> get(i)/set(i,v) to a direct `_ptr[i]` element access carrying the receiver's
    // alias scope - the same noalias contract `y[i]` and the local-`T[]` form already get, which a
    // method call through `this` cannot (it collapses every span to one origin, so two spans never
    // prove disjoint). Returns the element as an lvalue NamedVariable (Storage = element address,
    // NoaliasScopeId stamped); the caller loads it (get) or stores through it (set). Returns an empty
    // NV (Storage == null) when `structVar` is not an addressable by-value span - the caller then
    // falls back to the real method call (correct, just without the noalias contract). Matched
    // structurally via ArrayViewBufferFieldIndex, so the may-alias sibling view<T> (raw-T* `_ptr`)
    // is excluded and keeps method dispatch.
    LLVMBackend::NamedVariable LowerSpanElementAccess(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& structVar,
        llvm::Value* indexValue);

    // Read a named local/field's value, then (when it is a whole interface value, i.e. a fat
    // {vtable,data} struct) ledger which interface it belongs to by VALUE IDENTITY - the fat
    // struct is one shared LLVM type for every interface, so a later '?:' fat-vs-thin harmonizer
    // (UnifyTernaryArmTypes) has no other way to learn which interface a `move` of an
    // interface-typed local produced.
    llvm::Value* LoadNamedVariable(LLVMBackend::NamedVariable& namedVar);

    llvm::Value* LoadNamedVariableImpl(LLVMBackend::NamedVariable& namedVar);

    // If lvalue is a struct type with a user-defined operator, dispatch to it.
    // Returns the result Value*, or nullptr to fall back to built-in CreateOperation.
    // Returns a non-null value if a user-defined operator overload was found and called,
    // nullptr if the operand type has no matching operator (fall back to primitive handling).
    llvm::Value* TryUnaryOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx);

    // If an operator overload (e.g. operator+) just returned an owned heap string as an
    // unnamed SSA temporary, register it for end-of-full-expression cleanup. This is the
    // intermediate (a + b) of a chained concat a + b + c: it is consumed as the next
    // operator's argument and never bound to a named local, so without this its buffer
    // leaks. The named-local binding path and the move-string-parameter path unregister
    // the temporary they take ownership of, so only true intermediates are freed here.
    void TrackOwnedStringOperatorResult(LLVMBackend* compiler, llvm::Value* result);

    // A comparison (== != < > <= >=) or string concat (+) only BORROWS its string
    // operands: it reads their data/length and never takes ownership. When an operand is a
    // string returned straight from a CALL (a function/method/operator result that was
    // never bound to a named local), nothing else owns that temporary, so it must be
    // freed at end-of-statement or it leaks. Register every call-result string operand
    // for FlushOwnedStringTemps.
    //   - A named local / field read lowers to a load / insertvalue chain (not a CallInst)
    //     and is skipped here, so the operand's real owner (its scope destructor) frees it
    //     exactly once - `localA != localB` / `localA + localB` does not double free.
    //   - Registration is idempotent (RegisterOwnedStringTemp dedups), so an operator+
    //     result already tracked by TryBinaryOperatorOverload is freed once, not twice.
    //   - string.dtor checks the runtime owned bit, so registering a call that returns a
    //     borrow (e.g. an `alias string` accessor) is a safe no-op rather than a free.
    //   - Only string-struct operands are registered, so pointer arithmetic / ptr - ptr
    //     operands in an additive expression are ignored.
    void RegisterBorrowedStringOperandTemp(LLVMBackend* compiler, llvm::Value* operand);

    // Comparisons are the only operators that may dispatch off a raw-pointer left operand.
    // Arithmetic on a pointer keeps its builtin GEP/ptrdiff meaning.
    static bool IsComparisonOperatorText(const std::string& op);

    // Pointer-valued left operand (e.g. `R* r; r == k`): select an `operator op(T*, U)`
    // declared over the POINTEE struct. The pointer is passed straight through as `this` -
    // never copied into an alloca, which would change aliasing for a mutating operator.
    llvm::Value* TryPointerLhsOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType);

    // lhsElemType is the POINTEE type when lvalue is a pointer (TypedValue::elemType);
    // it is what makes an `operator op(T*, U)` candidate findable under opaque pointers.
    llvm::Value* TryBinaryOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType = nullptr);

    LLVMBackend::TypedValue ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx);

    LLVMBackend::NamedVariable ParseCastExpression(CFlatParser::CastExpressionContext* ctx, bool lvalue = false,
                                                   ResultUse use = ResultUse::Value);

    /*
     * Give a cast operand that carries no llvm::Value a value or a diagnostic, so CreateCast is
     * never handed a null operand (which SIGSEGV'd the compiler with no output). See the
     * definition in MainListener_Expressions.cpp for which operands are resolved vs rejected.
     */
    void ResolveValuelessCastOperand(CFlatParser::CastExpressionContext* ctx,
                                     CFlatParser::CastExpressionContext* operandCtx,
                                     LLVMBackend::NamedVariable& namedVar,
                                     const LLVMBackend::TypeAndValue& destTypeName);

    LLVMBackend::TypeAndValue ParseTypeName(CFlatParser::TypeNameContext* ctx);

    LLVMBackend::NamedVariable ParseUnaryExpression(CFlatParser::UnaryExpressionContext* ctx,
                                                    ResultUse use = ResultUse::Value);

    std::string ParseTypeSpecifierName(CFlatParser::TypeSpecifierContext* ctx);

    // Resolves the struct type expected at a call-site field initializer argument.
    // Pass effectiveParamIdx >= 0 for positional args (already offset by implicit 'this').
    // Pass effectiveParamIdx = -1 and non-empty namedParam for named-parameter form.
    std::string ResolveInitializerArgType(
        antlr4::ParserRuleContext* ctx,
        const std::string& functionName,
        int effectiveParamIdx,
        const std::string& namedParam);

    // Store a pre-parsed value into one named (non-bitfield) struct field, with the
    // same coercions and ownership behavior as brace-init: a char* string literal is
    // wrapped to the string struct, an implementer pointer is boxed into an interface
    // fat pointer, unions store at offset 0. Shared by EmitFieldInitializer (brace
    // init) and ParseElementExpression (<Tag attr=...> sugar). Returns false on error.
    /*
     * Coerce one brace-initializer value to an INTERFACE destination slot: box a thin concrete
     * pointer/value into the fat struct, or re-box an already-fat value to the destination's own
     * interface. Shared by the struct-field and the fixed-array / array-view element paths, so a
     * brace list cannot store a source vtable that dispatches the destination interface's slots.
     * Returns `val` unchanged when nothing applies.
     */
    llvm::Value* CoerceInitValueToInterface(
        LLVMBackend::NamedVariable& rightNV,
        llvm::Value* val,
        const std::string& ifaceName,
        antlr4::ParserRuleContext* errCtx);

    bool EmitOneFieldInit(
        llvm::Value* structPtr,
        const LLVMBackend::StructData& sd,
        const std::string& typeName,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& rightNV,
        antlr4::ParserRuleContext* errCtx);

    /*
     * A brace list whose DESTINATION is a pointer slot is memory-unsafe: EmitFieldInitializer
     * GEPs the pointee struct out of those 8 bytes (writing field values into the pointer
     * itself), and TryEmitContainerInitializer passes that same slot as the container's `this`.
     * The four sites that can produce such a destination reject here. The fifth caller - the
     * named-argument site - builds its own struct temp and is CORRECT; it does not call this.
     * Note LogErrorContext throws, so this never returns on the compile path.
     */
    void LogPointerBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& what,
        const std::string& typeName,
        const std::string& typeText,
        bool canAllocate,
        bool isUnique = false);

    /*
     * A brace list WITH VALUES on a declarator whose type is neither a struct/union/class nor a
     * recognized container ('int x = {5};', 'I gi = { a = 1 };' on an interface). Shared by the
     * local and global declarator paths so the two scopes give one identical diagnostic.
     * Note LogErrorContext throws, so this never returns on the compile path.
     */
    void LogNonAggregateBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& name,
        const std::string& typeName);

    /*
     * A brace list WITH VALUES on a 'T[]' VIEW field ('int[] v = {1,2,3};'). The local
     * declarator spelling can infer backing storage for the list (EmitArrayViewInferredInit),
     * but a field has no such storage to point at: it would need a lifetime tied to the
     * containing object, which nothing here allocates. An empty '{}' stays accepted as a null
     * view (unchanged); only a non-empty list reaches this. LogErrorContext throws.
     */
    void LogArrayViewFieldBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& name,
        const std::string& typeName,
        bool elemPointer);

    /*
     * Render the declared type for a diagnostic - 'S*', 'S**', 'void*', 'S*[2]' - so a remedy is
     * never synthesized from the bare pointee name. A type ALIAS ('using PS = S*') is already
     * resolved away by ParseDeclarationSpecifiers, so this names the resolved type, not 'PS'.
     */
    static std::string DescribePointerDeclType(const LLVMBackend::TypeAndValue& tv);

    /*
     * Spell a code-value store DESTINATION the way the source wrote it: 'Rec*', 'Rec**', 'int[]',
     * 'Rec*[2]', 'string'. DescribePointerDeclType renders an array VIEW as 'int*', which is not
     * what the declaration says, so views are spelled here instead.
     */
    static std::string CodeValueDestSpelling(const LLVMBackend::TypeAndValue& dest);

    /*
     * The cast escape is advised only where one actually COMPILES - verified per spelling, not
     * assumed. A plain pointer takes '(Rec*)' / '(Rec**)'. An array VIEW takes neither: '(int*)w'
     * then fails the view's own "cannot bind a raw pointer 'T*' to an array-view" check and
     * '(int[])w' is refused outright, so advising either would send the user into a second error.
     * `string` is reached by the char* coercion and '(string)' of a raw value is itself rejected.
     */
    static std::string CodeValueCastAdvice(const LLVMBackend::TypeAndValue& dest);

    /*
     * Parse a struct/class field DEFAULT initializer and gate it. `struct H { Rec* p = ro; };` is a
     * store path reached by neither the declarator nor the braces, and it has FIVE default-ctor
     * emitters - ParseStructDefinition, ParseClassDefinition, ParseConstructorDefinition, and BOTH
     * `program` emitters (ParseProgramDefinition and ParseImportedProgramDefinition). The guard has
     * to be in all five or the same spelling survives through another declaration kind.
     */
    llvm::Value* ParseFieldDefaultInitializer(
        const std::string& structName,
        const LLVMBackend::TypeAndValue& field,
        CFlatParser::AssignmentExpressionContext* ae);

    /*
     * The brace-list spelling of a field default (`Inner i = { x = 1 };` and the bare
     * `Inner i { x = 1 };`). Returns null when the list is not one this path can apply, in
     * which case the caller keeps its pre-existing no-initializer handling.
     */
    CFlatParser::InitializerListContext* FieldDefaultBraceList(
        const LLVMBackend::DeclTypeAndValue& field);

    llvm::Value* ParseFieldDefaultBraceInitializer(
        const std::string& structName,
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list);

    /*
     * The fixed-array arm of the above. The list is POSITIONAL, so the value has to be built
     * as an '[N x T]' aggregate for the CreateInsertValue into the containing struct - the
     * named field-init path cannot express it. Mirrors the local declarator's array-brace
     * split arm for arm (positional / pointer-element reject / non-struct-element reject /
     * seed-and-splat), writing into a slot instead of registering a local.
     */
    llvm::Value* EmitFieldDefaultFixedArrayBrace(
        const std::string& structName,
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list);

    // Per-slot value-init of an array whose element type owns a resource: construct each slot
    // independently and re-apply the named overrides there, instead of splatting one seed.
    void EmitOwningArrayValueInitSlots(
        llvm::Value* arrAlloc,
        llvm::StructType* elemTy,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* list,
        bool forceRoot);

    // Seed-once value-init of an array field: default-construct one element, apply the
    // named overrides, memcpy it into every slot of `slot`. POD elements only - an owning
    // element type routes to EmitOwningArrayValueInitSlots instead.
    void EmitFieldDefaultArraySplat(
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list,
        llvm::Value* slot,
        llvm::ArrayType* arrTy,
        llvm::StructType* elemTy);

    /*
     * The union arm of the default-constructor emitter. Fields alias at offset 0, so at most ONE
     * field default can apply: the first field carrying an explicit initializer, else the first
     * field's `= default`. A second explicit initializer is ambiguous and is rejected.
     */
    void EmitUnionDefaultConstructorBody(
        antlr4::ParserRuleContext* ctx,
        const std::string& structName,
        llvm::StructType* structType,
        std::vector<LLVMBackend::DeclTypeAndValue>& declList);

    /*
     * Code-value store gate for the AGGREGATE spellings the declarator, assignment and return
     * sites never see: a brace field initializer, a positional fixed-array or view element, a
     * global array element, and a struct field DEFAULT. `dest` is the SLOT's type - for an array
     * element that is the ELEMENT type, not the array's. LogErrorContext throws.
     */
    void RejectCodeValueIntoDataSlot(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::NamedVariable& src,
                                     const LLVMBackend::TypeAndValue& dest,
                                     const std::string& role,
                                     const std::string& what);

    // 'new T()' is meaningful only for a SINGLE star over a known struct: there is no 'new void()',
    // and 'new S()' is the wrong shape for an 'S**'.
    bool CanSuggestAllocation(antlr4::ParserRuleContext* ctx, const LLVMBackend::TypeAndValue& tv);

    /*
     * The EMPTY-'{}' companion to LogPointerBraceInitReject. A non-empty list on a pointer is
     * rejected because it is memory-unsafe; an empty one is rejected because it is AMBIGUOUS -
     * "the null pointer" and "a pointer to an empty object" are both reasonable readings, and
     * the compiler picking one silently would teach the other one wrong. Role-named the same
     * way so a test can prove which site fired. LogErrorContext throws; this never returns.
     *
     * '= default' is named FIRST and unconditionally because it is the only remedy valid at every
     * site: a generic body's 'T x = {}' rejects when T substitutes to a pointer, and there
     * 'nullptr' is nonsense guidance for the same body instantiated at T=int. The two cases are
     * not distinguished - the parameter-default arm has no declSpec to ask whether a '*' was
     * written - so both remedies are named everywhere rather than inventing a type-flag for it.
     */
    void LogEmptyBraceOnPointerReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& what,
        const LLVMBackend::TypeAndValue& tv);

    void EmitFieldInitializer(
        llvm::Value* structPtr,
        const std::string& typeName,
        CFlatParser::InitializerListContext* ctx);

    // Coerce a parsed element value to the container's `string` element type.
    // Mirrors EmitFieldInitializer's string handling: a char* string-literal constant is
    // wrapped directly; any other char* runtime value goes through operator string(char*).
    // On success the NV is rebuilt to carry the string aggregate via Storage.
    void CoerceElementToString(
        LLVMBackend* compiler,
        LLVMBackend::NamedVariable& nv,
        llvm::Value*& val,
        antlr4::ParserRuleContext* ctx);

    // Positional fixed-array initializer: T[N] x = {v0, v1, ...}.
    // Zero-initializes all N slots, then stores the provided elements in order. A count
    // greater than N is an error; a smaller count leaves the trailing slots zero-filled.
    // Creates and registers the array local. `elemType` string literals are coerced to string.
    void EmitPositionalFixedArrayInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        llvm::Value* arraySize,
        size_t line,
        std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList);

    /*
     * Per-element owning-source decision for a positional fixed-array brace list. Constructs the
     * slot: a copyable owner copies, a non-copyable owner moves (source zeroed + MarkVariableMoved
     * for a named slot). No drop-old - the slot is fresh. `val` is replaced by the value to store.
     */
    void ConsumeOwningBraceElementSource(
        const LLVMBackend::NamedVariable& nv,
        llvm::Value*& val,
        llvm::Value* elemPtr,
        const LLVMBackend::TypeAndValue& elemTV,
        const std::string& elemTypeName,
        antlr4::ParserRuleContext* ctx);

    // The slot-taking half of the above: everything after the array storage exists. Shared
    // with the field-default path, which owns an alloca rather than a registered local.
    void EmitPositionalFixedArrayIntoSlot(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        llvm::Value* arrAlloc);

    // Name the initializer for a diagnostic hint: "a", "h.d", or a neutral placeholder.
    static std::string DescribeInitializerPath(const std::string& callerName,
                                               const std::string& fieldName);

    /*
     * Element indirection depth of a FIXED ARRAY type. On `T[N]`, GetType applies Pointer /
     * ElemPointer to the ELEMENT before wrapping it in the ArrayType, so `int*[2]` carries
     * Pointer (one star on the element) and `int**[2]` carries both. Reading ElemPointer alone
     * as "the element is a pointer" is wrong and silently answers 0 for `int*[2]`.
     */
    static int FixedArrayElementStars(const LLVMBackend::TypeAndValue& tv);

    // Render a fixed-array type for a diagnostic: "int[3]", "int*[2]", "Foo**[2][4]".
    static std::string DescribeArrayShape(const LLVMBackend::TypeAndValue& tv);

    // Same, for the initializer side, whose extents arrive as loose values.
    static std::string DescribeArrayShape(const std::string& typeName, int stars, uint64_t n,
                                          const std::vector<uint64_t>& innerDims);

    /*
     * `T[N] b = a;` - copy one fixed array into another. The destination owns its storage, so
     * this is a memcpy of sizeof(T) * N, not an alias. Only ever entered for an ARRAY-SHAPED
     * source (a fixed array or a view); a scalar RHS keeps its own, better, diagnostic. Every
     * shape this cannot prove safe gets a LogError naming both array types; the repo rule is
     * that an unsupported construct produces a diagnostic, never an LLVM verifier dump.
     */
    void EmitFixedArrayValueCopy(antlr4::ParserRuleContext* ctx,
                                 const LLVMBackend::TypeAndValue& destTV,
                                 llvm::Value* destAlloc, llvm::ArrayType* destArrTy,
                                 llvm::Value* srcPtr,
                                 const std::string& srcTypeName, bool srcPointer,
                                 bool srcElemPointer, uint64_t srcConstArraySize,
                                 const std::vector<uint64_t>& srcInnerDims);

    /*
     * Default-initialize a fixed array of structs declared with no initializer (`T arr[N];`),
     * giving every element what the scalar path gives `T one;` - the synthesized default
     * constructor's value, field initializers included. Primitive elements are deliberately not
     * covered: plain `int x;` is uninitialized (C semantics), so `int[N] x;` stays that way too.
     * Multi-dimensional arrays are contiguous, so the flat element walk covers `T[N][M]` as well.
     */
    void EmitFixedArrayDefaultInit(llvm::Value* arrAlloc, const LLVMBackend::TypeAndValue& tv);

    // Coerce a folded element constant to the array's element type so ConstantArray::get
    // does not assert. Mirrors CreateGlobalVariable's int/fp-widen/null coercion.
    static llvm::Constant* CoerceConstantToArrayElement(
        LLVMBackend* compiler, llvm::Constant* c, llvm::Type* elemTy);

    // Global fixed-size array brace/positional initializer: T[N] g = {v0,...} / {}.
    // Lowers to an LLVM constant array on the GlobalVariable (no runtime stores, which
    // cannot run at global-init time). Positional elements must be compile-time constants.
    void EmitGlobalFixedArrayInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        bool positionalArray,
        CFlatParser::DirectDeclaratorContext* direct,
        antlr4::ParserRuleContext* errCtx);

    // Fold an evaluated size expression to a compile-time unsigned integer. Handles literals,
    // loads of a global int with a constant initializer (cflat `const int N = ...`), integer
    // arithmetic (W*H etc.), and integer width casts. Returns false if it is not foldable.
    // constGlobals (optional): when non-null, a load of a GLOBAL only folds if the global's name
    // is in the set. This restricts if-const folding to true compile-time constants (const scalars,
    // enum members) and excludes mutable globals. When null, any global with a const-int initializer
    // folds (the original behavior, used by EvalGlobalArrayDim).
    static bool TryFoldConstInt(llvm::Value* v, uint64_t& out,
                                const std::unordered_set<std::string>* constGlobals = nullptr);

    // Resolve a GLOBAL array dimension to a compile-time constant. The size IR is built inside
    // a throwaway function so its loads/arithmetic never leak into the program-init block, then
    // folded. On a non-constant size, logs an error and returns 1 as an error-recovery extent
    // (LogError does not abort, so the dimension must stay well-formed to avoid a later crash).
    llvm::ConstantInt* EvalGlobalArrayDim(CFlatParser::AssignmentExpressionContext* expr);

    // Length-inferred array-view initializer: T[] x = {v0, v1, ...}.
    // `T[]` is a thin T* (IsArrayView + Pointer), so there is no fixed extent in the type.
    // We allocate an inline [N x T] backing array on the stack (N = the element count),
    // fill it positionally, then point the T* local at element 0. The length N is a
    // compile-time fact only; it is not stored at runtime, matching T[]'s thin repr.
    void EmitArrayViewInferredInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        size_t line,
        std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList);

    // Handle brace initializers whose target is a generic container:
    //   list<T> x       = {a, b};      -> default-construct, then x.add(a); x.add(b);
    //   array<T> x      = {a, b};      -> default-construct, then x.init(2); x.set(0,a); x.set(1,b);
    //   dictionary<K,V> x = {k: v};    -> default-construct, then x.set(k, v);
    // `alloc` must already hold a default-constructed (empty) container. Returns true when
    // `tv` names a recognized container and the list was consumed here (so the caller skips
    // EmitFieldInitializer); false otherwise (caller falls back to struct field-init).
    bool TryEmitContainerInitializer(
        llvm::Value* alloc,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList);

    LLVMBackend::NamedVariable ParseNewExpression(CFlatParser::NewExpressionContext* ctx);

    // True when the pointee TYPE's own alignment (`struct alignas(64) T`) already routes `new` and
    // `delete` to the aligned pair. Mirrors the free site's recovery in EmitOwningPtrCleanup and the
    // `typeAlreadyAligned` test in ParseNewExpression: such a block carries no per-site tag, so a
    // field's `alignas(0, N)` clause cannot disagree with the allocation and must not be demanded.
    bool ElementTypeIsOverAligned(const LLVMBackend::TypeAndValue& typeAndValue) const;

    // If funcName is a method ("Type.method") or destructor ("~Type") whose owning
    // struct is registered, return the struct name. Returns "" for top-level
    // functions or unrecognized owners. Used by the delete-of-field rule to gate
    // 'delete obj->field' on "is the current function a method of obj's type."
    std::string SplitEnclosingStruct(const std::string& funcName, LLVMBackend* compiler) const;

    LLVMBackend::NamedVariable ParseDeleteExpression(CFlatParser::DeleteExpressionContext* ctx);

    // True when this 'move' expression sits DIRECTLY in a call-argument position. Walks up
    // through single-child pass-through expression nodes only, so 'f(move a)' matches while
    // 'f(g(move a))' or 'f(move a + b)' does not.
    static bool IsDirectCallArgument(antlr4::ParserRuleContext* ctx);

    /*
     * True when `name` is a by-value STRUCT parameter of the current function that was NOT
     * declared `move` - i.e. the callee only borrows the caller's struct. Moving a `unique`
     * field out of such a parameter nulls the callee's copy while the caller's original still
     * holds the pointer, so both free it. A `move` parameter (leg E) owns its copy and is safe.
     */
    bool IsBorrowedStructParameter(LLVMBackend* compiler, const std::string& name);

    // The same question asked of a RESOLVED binding rather than a name. Storage identity is what
    // separates the parameter from an inner local declared with the parameter's name.
    bool IsBorrowedByValueParamBinding(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& nv);

    // A 'move' by-value struct parameter whose type owns storage (e.g. `move Holder h` where
    // Holder holds a unique pointer). Returning such a parameter by a PLAIN `return h` copies
    // its owning bits without releasing the parameter, so both the returned value and the
    // expiring parameter free the same storage. 'return move h' is the correct transfer form.
    bool IsMoveOwningStructParameter(LLVMBackend* compiler, const std::string& name);

    // The variable an alias-origin description like "h.p" was reached through ("h"); the whole
    // string when there is no dot.
    static std::string BorrowedOriginRoot(const std::string& origin);

    // The ROOT variable of a field-path NamedVariable ("w" for `w.b` and for `w.a.b`).
    static std::string FieldPathRootName(const LLVMBackend::NamedVariable& nv);

    /*
     * Reject an IMPLICIT consuming store (`o = w.b`, `dst[0] = w.b`, `UBox o = w.b`, a brace
     * element) whose source is a field path rooted at a BORROWED by-value struct parameter.
     * The consume zeroes only the callee's bit copy of the field, so the caller's struct still
     * frees the same pointee. This is the ruling ParseMoveExpression applies to `move w.b`.
     * Returns true (and has already reported) when the store must not be lowered.
     */
    bool RejectConsumeOfBorrowedByValueParamField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        antlr4::ParserRuleContext* ctx);

    /*
     * Copy the OWNERSHIP PROVENANCE of a wrapped operand onto the wrapper's result. A
     * parenthesis and a REDUNDANT same-type cast are not operators - they change the spelling,
     * never the value - so `(x)` / `(T)x` must reach the consume arms, the reject guards and the
     * moved-from marking with exactly the facts `x` reaches them with. Only provenance is copied;
     * the wrapper keeps its own Primary / TypeAndValue / Storage decisions.
     */
    static void AdoptWrapperProvenance(LLVMBackend::NamedVariable& dst,
                                       const LLVMBackend::NamedVariable& src);

    // True when `dest` names exactly `src`'s own type, so the cast is a whole-value pass-through.
    // A TYPE-CHANGING cast (interface boxing, a numeric conversion, a pointer reinterpret) is NOT
    // a consume of the named variable and must keep failing every provenance-keyed test.
    bool IsRedundantCastOfSource(const LLVMBackend::TypeAndValue& src,
                                 const LLVMBackend::TypeAndValue& dest);

    // The sole typeCheckExpression of a conditional expression, or null when the descent passes
    // through any operator. Lets the whole-expression consumer tell a bare `p as T` from `p as T + 1`.
    static CFlatParser::TypeCheckExpressionContext* SoleTypeCheckExpressionOf(
        CFlatParser::ConditionalExpressionContext* ctx);

    LLVMBackend::NamedVariable ParseMoveExpression(CFlatParser::MoveExpressionContext* ctx);

    LLVMBackend::NamedVariable ParseOperatorStringExpression(CFlatParser::OperatorStringExpressionContext* ctx);

    // Build a simd<T,N> TypeAndValue from a `simd<T,N>` type-specifier node (element type +
    // lane count), resolving a generic type parameter for the element if one is active. Mirrors
    // the simd handling in ParseDeclarationSpecifiers.
    LLVMBackend::TypeAndValue ParseSimdTypeSpec(CFlatParser::SimdTypeSpecifierContext* sd);

    // Lower `simd<T,N>.load(arr, i)` / `simd<T,N>.store(vec, arr, i)` - the explicit memory bridge
    // for simd values. `i` is an element index (not bytes); natural element alignment is used so an
    // arbitrary index into an ordinary buffer is safe (the buffer need not be vector-aligned). The
    // load returns a `<N x T>`; the store writes a vector's N lanes back. See doc/HPC.md.
    LLVMBackend::NamedVariable ParseSimdStaticMethod(
        CFlatParser::PostfixExpressionContext* ctx, CFlatParser::SimdTypeSpecifierContext* simdSpec);

    // Validate that a simd<T,N> static-method argument is itself a `simd<T,N>` of the same
    // shape (the math ops take no implicit scalar splat). Returns the value for chaining.
    llvm::Value* requireSimdArg(antlr4::ParserRuleContext* ctx, llvm::Value* v, int k,
                                llvm::Type* vecTy, const std::string& method);

    // The elementwise vector-math intrinsics callable as static methods on simd<T,N>.
    // Each maps 1:1 to an LLVM intrinsic that lowers to a real SIMD instruction (no
    // scalarization, no libm). Arity is the number of vector operands. Transcendentals
    // (exp/log/sin/...) are deliberately excluded: they have no hardware vector form and
    // would scalarize to per-lane libm calls. See doc/HPC.md.
    struct SimdMathIntrinsic { llvm::Intrinsic::ID Id; int Arity; };
    const SimdMathIntrinsic* LookupSimdMathIntrinsic(const std::string& name);

    // Emit a bitfield READ from a storage word and return a fully-configured NamedVariable. The
    // returned variable carries BitfieldStorage/Offset/Width/Unsigned so the assignment codegen
    // (bitfieldAssign) can do the read-modify-write store on the WRITE path. This is the single
    // source of truth for bitfield access masking/shift, shared by the normal member-access path
    // and the transparent anonymous-member path (do not duplicate the mask/shift logic).
    LLVMBackend::NamedVariable EmitBitfieldAccess(
        LLVMBackend* compiler,
        llvm::Value* storagePtr,
        llvm::Type* storageTy,
        const LLVMBackend::BitfieldInfo& bf,
        const std::string& parentVariableName,
        const std::string& owningStructName);

    // C11 transparent anonymous-member access: resolve `base.fieldName` where `fieldName` is not
    // a direct field of `base` but lives inside an anonymous (synthetic "__anonN") struct/union
    // member - possibly nested. The C-interop extractor flattens a C11 anonymous member to a
    // synthetic "__anonN" sub-record (e.g. _LARGE_INTEGER's unnamed DUMMYSTRUCTNAME struct), so
    // reaching `li.LowPart` means walking through `li.__anon0.LowPart`. Builds the chained
    // GEP/load and returns an lvalue NamedVariable identical to the explicit form. Only "__anonN"
    // members are searched (real named members like `u` are NOT transparent). Returns false
    // (leaving `out` untouched) when no anonymous member exposes `fieldName`.
    bool ResolveTransparentAnonField(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& structVar,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& out);

    // Classify a postfix call result into the struct/interface receiver slots so the next
    // chained '.member'/'.method()' resolves against it. Mirrors the inline classifier used
    // after the normal (non-interface) call path; the caller clears both slots first.
    void ClassifyPostfixCallResult(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& result,
        LLVMBackend::NamedVariable& structVar,
        LLVMBackend::NamedVariable& interfaceVar);

    // Returns the member-name text immediately following `opNode` (a `.`/`->` token) in the
    // postfix child list, or "" if the next child is not an Identifier/`move` name. Used by
    // [PFX-1a] to peek the member before resolution so operator-> forwards only on a miss.
    static std::string NextMemberName(CFlatParser::PostfixExpressionContext* ctx,
                                      antlr4::tree::ParseTree* opNode);

    // §3.7 ParsePostfixExpression - walks `primary (suffix)*`, threading the running receiver
    // through structVar/interfaceVar/namedVar. Each suffix is one loop iteration. Internal map
    // (grep the "// [PFX-n]" anchors to jump):
    //   [PFX-1] member-access operator (. -> ?.): auto-deref receiver into structVar
    //   [PFX-2] member name: field GEP, else defer to call (member fn / winrt slot / UFCS)
    //   [PFX-3] subscript `[expr]` (array / pointer / simd / span / operator[])
    //   [PFX-4] call `(args)`: builtins, then the member/free-call path
    //   [PFX-5]   indirect call through a function<...> value (vtable slot, callback field)
    //   [PFX-6]   argument assembly + implicit-this prepend
    //   [PFX-7]   call lowering: [winrt] vtable dispatch | null-conditional | overloaded call
    LLVMBackend::NamedVariable ParsePostfixExpression(CFlatParser::PostfixExpressionContext* ctx, bool lValue = false,
                                                       size_t dropTrailingChildren = 0,
                                                       ResultUse use = ResultUse::Value);

    // The body of ParsePostfixExpression. The public entry is a thin wrapper that funnels
    // every exit through the void-result gate below.
    LLVMBackend::NamedVariable ParsePostfixExpressionInner(CFlatParser::PostfixExpressionContext* ctx, bool lValue,
                                                       size_t dropTrailingChildren, ResultUse use);

    // A call whose return type is 'void' produces no consumable value: the direct path hands
    // back a void-typed CallInst that reaches the verifier as `void <badref>`. Reject at the
    // call, where the position is still known, rather than at each consumer.
    void DiagnoseVoidResultConsumed(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& nv,
                                    ResultUse use, const std::string& subject);

    // Walk down single-child rule nodes to find a UnaryExpressionContext.
    // Returns nullptr if the path branches or never reaches a unaryExpression.
    CFlatParser::UnaryExpressionContext* tryGetUnaryExpression(antlr4::RuleContext* ctx);

    // Walk down single-child rule nodes to find a PostfixExpressionContext. Used by the lock
    // statement to isolate the base of `rw.read` / `rw.write` - the mode suffix is a soft
    // keyword, not a real field, so evaluating the whole postfix chain fails.
    // Returns nullptr if the path branches or never reaches a postfixExpression.
    CFlatParser::PostfixExpressionContext* tryGetPostfixExpression(antlr4::RuleContext* ctx);

    // Extract the callee identifier from a delete operand that is a call, for diagnostics only.
    // Takes the operand text up to its first '(' (the call's argument list) and returns the
    // trailing identifier after the last '.'/'->' (e.g. "l.get" -> "get", "foo" -> "foo").
    // Returns "" when no plausible identifier is found; detection does not depend on this.
    std::string DeleteOperandCalleeName(CFlatParser::UnaryExpressionContext* ue);

    // Drill down single-rule-child chains to find the first actual cast expression
    // (one of the form '(' typeName ')' castExpression). Used by ParseDeleteExpression
    // to peel casts off a delete target - 'delete[_] (T*)p' must behave like
    // 'delete[_] p'. Returns nullptr if the chain branches before reaching such a cast.
    CFlatParser::CastExpressionContext* tryGetCastExpression(antlr4::RuleContext* ctx);

    // Returns true if rawText (the full StringLiteral token text including quotes)
    // contains at least one unescaped '{' that starts an interpolation expression.
    // '{{' is an escaped literal '{' and does NOT count as interpolation.
    bool HasInterpolation(const std::string& rawText);

    // Parses a format string literal with {expr} interpolation.
    // Splits the literal into alternating plain-text and expression segments,
    // coerces each expression segment to string via operator string,
    // stacks all (ptr, len) pairs on the stack and calls __strconcat.
    llvm::Value* ParseFormatString(CFlatParser::PrimaryExpressionContext* ctx, const std::string& rawText);

    struct CaptureInfo
    {
        std::string Name;
        LLVMBackend::TypeAndValue TV;
        bool ByReference;       // true for non-pointer struct types (capture by reference)
        llvm::Value* OuterStorage;
        bool IsThis = false;    // the implicit method self pointer; capture by value, re-register as 'this'
    };

    // Walk the lambda body AST and collect variables captured from the enclosing function scope.
    std::vector<CaptureInfo> CollectLambdaCaptures(
        antlr4::ParserRuleContext* bodyCtx,
        const std::set<std::string>& lambdaParamNames,
        LLVMBackend* compiler);

    LLVMBackend::NamedVariable ParseLambdaExpression(CFlatParser::LambdaExpressionContext* ctx);

    // Map an LLVM type to its CFlat canonical type name for tuple construction.
    std::string LLVMTypeToTypeName(llvm::Type* ty, const std::string& structHint = "");

    // Build a tuple<T1,T2,...> value from a parenthesized expression list: (e1, e2, ...)
    LLVMBackend::NamedVariable ParseTupleExpression(CFlatParser::TupleExpressionContext* ctx);

    // Heap-allocate and default-construct a node type for the <Tag> sugar: operator
    // new(sizeof) then the default constructor, returning a typed pointer. Mirrors
    // the non-array path of ParseNewExpression. Returns null on error.
    llvm::Value* EmitHeapDefaultConstruct(LLVMBackend* compiler, const std::string& typeName,
                                          antlr4::ParserRuleContext* errCtx);

    // <Tag attr="lit" attr2={expr}> children </Tag> - JSX-like element sugar. Lowers
    // 1:1 to: construct Tag on the heap, store each attribute into the matching field
    // (ownership-correct, via EmitOneFieldInit), and add() each child (nested element
    // or {expr}). Library-agnostic: the compiler only knows the tag is a type in scope
    // with the matching fields and an add() method. The result is laundered to behave
    // like a factory pointer (NOT a `new` local), so `return <View/>` is allowed.
    llvm::Value* ParseElementExpression(CFlatParser::ElementExpressionContext* ctx);

    // `use` is forwarded ONLY through the '(' expression ')' alternative: parentheses are not
    // an operator, so a discarded `(g());` is still a discard. Every other alternative ignores it.
    llvm::Value* ParsePrimaryExpression(CFlatParser::PrimaryExpressionContext* ctx,
                                        ResultUse use = ResultUse::Value);

    LLVMBackend::NamedVariable ParseIdentifier(antlr4::tree::TerminalNode* node);

    // Consumes one escape sequence starting just after the leading '\'.
    // Advances itr past the consumed character(s) and returns the decoded char.
    char ProcessEscapeChar(std::string::const_iterator& itr, const std::string::const_iterator& end);

    char ParseCharLiteral(const std::string& text);

    // foldBraces: when true, {{ -> { and }} -> } (source-level escape for non-interpolated strings).
    // Pass false when decoding accumulated literal content inside ParseFormatString, because
    // that content may contain legitimate }} sequences (e.g. from JSON) that must not be folded.
    std::string ProcessRawText(const std::string& rawText, bool foldBraces = false);

    llvm::Value* ParseExpression(CFlatParser::ExpressionContext* ctx);

    // A discarded statement result (`makePlain(2);`) that is an unclaimed owning-struct rvalue
    // temp is claimed by nothing, so without this it leaks. Spill it and register for destruction
    // at the end of the full expression (FlushOwnedTemps at the block-item boundary). Mirrors the
    // field-access registration gate (see the `makeToken().text` path). Storage==null is the
    // rvalue-temp signal: a named local or a deref (`(*p)`) carries Storage and is freed by its own
    // scope dtor, so we must not double-free it here. `alias` borrows and string/closure values
    // (own runtime owned-bit + temp lists) are excluded.
    void RegisterDiscardedOwningStructTemp(const LLVMBackend::NamedVariable& nv);

    // Mandatory-nodiscard: an owning RETURN value used as a bare discarded statement (or a bare
    // for-update) must be consumed, not dropped. Value identity picks out exactly the top-level
    // result - an inner call passed as an argument, a member/index/comparison operand, an
    // assignment target, or a `delete`d value is a DIFFERENT value and is never flagged.
    void DiagnoseDiscardedOwningReturn(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& nv);

    // The owning-value STRUCT rvalue-return gate (mirror of RegisterDiscardedOwningStructTemp):
    // an rvalue temp (Storage==null) of an owning value type, not an alias/field/string/closure.
    bool IsDiscardedOwningStructResult(const LLVMBackend::NamedVariable& nv);

    // True when the resolved overload takes the receiver BY VALUE with `move` (an extension method
    // like `drop(move list<string> s)`): the callee owns and frees it, so the caller must not.
    bool MethodConsumesReceiver(const std::string& functionName, const std::string& recvType);

    // A method invoked on an owning-value RVALUE temp receiver (`makeList().count()`) consumes the
    // temp as `this` and then drops it, so without this it leaks. Spill the receiver to an entry
    // alloca, register it for end-of-full-expression destruction (FlushOwnedTemps at the block-item
    // boundary), and bind that alloca as the `this` argument's Storage so the call and the
    // destructor address the same object. Mirrors the field-access gate (`makeToken().text`):
    // Storage==null is the rvalue-temp signal (a named local or a deref carries Storage and is freed
    // by its own scope dtor - registering it would DOUBLE-FREE), an `alias` borrow return must not be
    // destructed, string/closure values run their own owned-bit/temp-list paths, and a receiver that
    // is a field of an already-registered owning temp (FromOwningTempField) is covered by the
    // parent's full destructor.
    void RegisterOwningTempReceiver(antlr4::ParserRuleContext* ctx,
                                    const LLVMBackend::NamedVariable& receiver,
                                    LLVMBackend::NamedVariable& thisArg,
                                    const std::string& functionName);

    void ProcessPlusPlus();

    // Recursively walk any AST subtree and queue generic instantiations found anywhere
    // (declaration specifiers, initializer expressions, etc.).
    // Skips generic template struct definition bodies (contain unbound type parameters).
    void ScanAndQueueGenericTypeUses(antlr4::RuleContext* ctx);

    // Check if a declaration uses a generic type and queue it for instantiation if needed.
    // Instantiation is deferred to avoid interrupting code generation in the current context.
    void QueueInstantiateGenericType(CFlatParser::DeclarationSpecifiersContext* declSpec);

    // Compute the mangled name for a generic instantiation, e.g. Box<int, float> -> "Box__int__float".
    std::string MangledGenericName(const std::string& baseName, const std::vector<std::string>& typeArgs);

    // Process all pending generic instantiations that were queued during parsing.
    // This is called when it's safe to emit code (e.g., after a declaration completes).
    void ProcessPendingInstantiations();

    // Pre-declare a generic instantiation's member function signatures so that
    // cross-references resolve to a declaration before any method body is emitted.
    // This matters because the post-fields dependency flush (ProcessPendingInstantiations)
    // can pull in a *sibling* instantiation whose body calls back into this
    // still-incomplete type's methods (e.g. block_pool<T> field-flushed during
    // arena_channel<T>, calling page_arena<T>::reset). Without a forward declaration
    // the call site reports "Unknown identifier". LLVM fills the body in later when
    // this type's member-body loop runs. Caller gates on "is an instantiation".
    void PreDeclareInstantiationMembers(
        LLVMBackend* compiler,
        const std::vector<CFlatParser::FunctionDefinitionContext*>& functionList,
        const std::string& baseName,
        const std::string& structName,
        const LLVMBackend::TypeAndValue& returnType);

    // The program-owned inbox/outbox channel is always arena_channel<IMessage>:
    // the seam is untyped (consumers downcast pa->_root), so IMessage is the
    // single, fixed payload interface. Mangled name of that instantiation.
    static constexpr const char* kArenaChannelType = "arena_channel__IMessage";

    // Ensure arena_channel<IMessage> (and its transitive block_pool<IMessage> /
    // page_arena<IMessage>) is fully instantiated so the synthetic program
    // inbox/outbox fields and the `>>` wiring have a concrete type + methods to
    // reference. Returns true if the type is available (template was imported).
    bool EnsureArenaChannelInstantiated(LLVMBackend* compiler);

    void ParseStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& nameOverride = {}, const std::string& namespaceName = {});

    // Extract the canonical lock expression text from a single lock arg expression.
    // Normalizes '->' to '.' and strips a trailing '.read' / '.write' soft-keyword suffix.
    // The mode suffix is handled by the caller via GetLockArgMode().
    std::string GetLockArgCanonical(CFlatParser::ExpressionContext* expr);

    // Returns "read", "write", or "" (mutex / default exclusive) for the lock arg expression.
    std::string GetLockArgMode(CFlatParser::ExpressionContext* expr);

    // Canonicalize a raw lock string ('->' to '.', drop the rwlock mode suffix).
    static std::string StripLockModeSuffix(const std::string& text);

    // Lock-set tokens a held lock contributes. A namespace-qualified global ("Reg.g_mtx")
    // also contributes its bare form, because a guard group inside "namespace Reg" names its
    // guardian as written there ("g_mtx"). Only true namespace prefixes are stripped, so a
    // member access like "obj.mtx" never aliases to a bare "mtx".
    std::vector<std::string> LockSetAliases(const std::string& canonical);

    // Lock-set check for a global declared inside a file-scope lock group. Mirrors the
    // struct-field check, but says "Global" so the two diagnostics are distinguishable.
    // Templated on the node so both the bare-identifier and the namespace-qualified
    // (Reg.g_count) access paths can report against their own context.
    template <typename TNode>
    void CheckGlobalGuard(TNode* node, const std::string& name,
                          const LLVMBackend::NamedVariable& globalNV)
    {
        const std::string& guard = globalNV.TypeAndValue.GuardedBy;
        if (guard.empty()) return;
        if (currentLockSet.find(guard) != currentLockSet.end()) return;
        LogErrorContext(node, std::format(
            "Global '{}' is guarded by '{}': must hold '{}' before accessing it.",
            name, guard, guard));
    }

    // The lock-set key a guarded variable's guard resolves to: a field reached through a
    // receiver ("n->count") keys on "n.ver"; a self-field or guarded global keys on the bare
    // guardian name. Mirrors the key the read-side guard checks build.
    static std::string GuardLockKey(const LLVMBackend::TypeAndValue& tv);

    // Write-side half of the guarded-access check. Reading a guarded field is legal in every
    // held mode; WRITING one is legal only under an exclusive lock - a shared reader may not
    // mutate what other readers are reading, and an optimistic reader holds nothing at all
    // (its body must be side-effect free or the version validation means nothing).
    // No-ops when the guard is not held: the read-side check has already reported that.
    void CheckGuardedWrite(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& target);

    // Verify that the current lock-set satisfies the RequiredLocks of the function just called.
    // Called immediately after CreateOverloadedFunctionCall; reads lastCallRequiredLocks and
    // lastCallParameterNames from the side-channel populated by that call.
    // receiverText: the name of the receiver object ("acct"), "this" for bare method calls, or "".
    // arguments: the NamedVariable vector passed to the call (includes implicit this at index 0 when present).
    void CheckCallSiteLocks(antlr4::ParserRuleContext* ctx,
                            const std::string& receiverText,
                            const std::vector<LLVMBackend::NamedVariable>& arguments);

    // Find the first registered overload of `methodName` whose first parameter type is `firstParamType`.
    llvm::Function* FindMethodOf(const std::string& methodName, const std::string& firstParamType);

    // Emit the program's builtin field teardown into the CURRENT insert point of the
    // destructor being built: cleanup() + free the IAllocator stored in _allocator,
    // dispose _stop_source, and free the consumer-owned inbox arena_channel. All steps
    // null-check first, so it is safe even if run() was never called.
    //
    // This does NOT emit the function return - the caller appends the ret afterward. That
    // is how the program gets ONE destructor with the builtin fields cleaned up at the end:
    // a user-written ~Name() body runs first, then this teardown is appended before the ret
    // (see ParseProgramDestructorDefinition); programs without a user destructor get a
    // synthetic ~Name() that is just this teardown.
    //
    // Self-contained: every value is rederived from `name` via programTable / dataStructures,
    // so it works from both the user-destructor path and the synthetic-destructor path.
    void EmitProgramSyntheticTeardown(const std::string& name, llvm::Value* thisArg);

    void EmitProgramRunWrapper(const std::string& name, CFlatParser::ProgramDefinitionContext* ctx = nullptr);

    // Reject a user definition that already occupies a synthesized program member's exact overload
    // slot. Called before each synth CreateFunctionDefinition in EmitProgramRunWrapper.
    void RejectIfProgramMemberSlotTaken(CFlatParser::ProgramDefinitionContext* ctx,
        const std::string& progName, const std::string& member, const std::string& signature,
        const LLVMBackend::TypeAndValue& returnType,
        const std::vector<LLVMBackend::TypeAndValue>& params);

    void ParseImportedProgramDefinition(const std::string& name);

    void ParseProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx);

    void ParseClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& nameOverride = {}, const std::string& namespaceName = {});

    std::vector<std::string> ParseGenericTypeParameters(CFlatParser::GenericTypeParametersContext* genericParams);

    // Returns { typeParamName -> [requiredInterface, ...] } from a whereClause context.
    std::unordered_map<std::string, std::vector<std::string>>
    ParseWhereClause(CFlatParser::WhereClauseContext* wc);

    // Checks that each concrete type argument satisfies its where-clause constraints.
    // Logs an error and returns false on the first violation.
    bool CheckConstraints(
        const std::string& templateName,
        const std::vector<std::string>& typeParams,
        const std::vector<std::string>& typeArgs,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& constraintMap,
        antlr4::ParserRuleContext* ctx);

    void ParseConstructorDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName, bool suppliesNoArgCtor = false);

    void ParseDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& structName);

    // Like ParseDestructorDefinition, but for a `program`: emit the user's ~Name() body,
    // then append the builtin field teardown so the program has ONE destructor with the
    // builtin fields cleaned up at the end. Requires programTable[name] field indices to
    // be set before this is called.
    void ParseProgramDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& name);

    std::vector<LLVMBackend::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList);

    LLVMBackend::ConstantVariant ParseNumberConstant(std::string rawNumber);

    void LogErrorContext(antlr4::tree::TerminalNode* ctx, std::string errorMessage);

    void LogErrorContext(antlr4::ParserRuleContext* ctx, std::string errorMessage);

    void LogWarningContext(antlr4::ParserRuleContext* ctx, std::string warningMessage);

    void PrintContext(antlr4::ParserRuleContext* ctx, std::string suffix = "");
};
