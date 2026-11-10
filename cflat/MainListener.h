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
#include <bit>
#include <cassert>
#include "CompilerManager.h"
#include <vector>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "TypeMangling.h"
#include "LspSymbolIndex.h"

// Preserve the written star as an element pointer before Pointer becomes the view pointer.
LLVM_ATTRIBUTE_NOINLINE static void SetFunctionPointerArrayViewShape(
    LLVMBackend::TypeAndValue& type)
{
    type.ElemPointer = type.ElemPointer || type.Pointer;
    type.IsArrayView = true;
    type.Pointer = true;
}

std::string FlexibleArrayMemberLengthError(const LLVMBackend::TypeAndValue& type);

// Every postfix expression that IS the operand of `sizeof` / `&`: the outer one and each
// parenthesised layer down to the member expression. Empty when the operand is anything more
// than that, e.g. `*m` or `m + 1`.
std::vector<antlr4::ParserRuleContext*> DirectOperandPostfixes(antlr4::tree::ParseTree* node);
// Shape queries for `*&X` (flexible-member flags): whole operand of a prefix '*' / its operator.
bool IsWholeDerefOperand(antlr4::tree::ParseTree* node);
bool IsParenthesizedPostfix(CFlatParser::PostfixExpressionContext* postfix);
std::string LeadingUnaryOperator(antlr4::tree::ParseTree* castNode);

// Installs the keep list for ONE operand and restores the enclosing list afterwards (also on a
// LogError throw), so a nested `*p` / `&x` / assignment cannot wipe `sizeof((*p).data)`'s list.
struct FlexibleArrayKeepScope
{
    std::vector<antlr4::ParserRuleContext*>& slot;
    std::vector<antlr4::ParserRuleContext*> saved;
    FlexibleArrayKeepScope(std::vector<antlr4::ParserRuleContext*>& keep,
                           std::vector<antlr4::ParserRuleContext*> next)
        : slot(keep), saved(std::move(keep)) { slot = std::move(next); }
    ~FlexibleArrayKeepScope() { slot = std::move(saved); }
    FlexibleArrayKeepScope(const FlexibleArrayKeepScope&) = delete;
    FlexibleArrayKeepScope& operator=(const FlexibleArrayKeepScope&) = delete;
};

// Parentheses around a whole initializer add nothing (`C x = (f(a));` is `C x = f(a);`): peel
// `( expression )` primaries reached through single-child rules only. A call with a suffix, or any
// operator above the parens, stops the peel. Shape only, never the text of the expression (T272).
LLVM_ATTRIBUTE_NOINLINE CFlatParser::AssignmentExpressionContext* UnwrapInitializerParens(
    CFlatParser::AssignmentExpressionContext* assign);

CFlatParser::PostfixExpressionContext* SolePostfixExpressionShape(
    antlr4::tree::ParseTree* node);
struct StringLiteralExpressionInfo
{
    std::string Value;
    bool IsNarrow = true;
    bool IsNarrowOrUtf8 = true;
    // True only for a real expression segment; Verbatim brace regions remain literal text.
    bool HasExpressionSegment = false;
};
std::string DecodeStringLiteralToken(const std::string& rawText, bool foldBraces);
bool HasInterpolationSegment(const std::string& rawText);
CFlatParser::ArgumentExpressionListContext* SoleDirectNamedCallArguments(
    antlr4::tree::ParseTree* node, const std::string& functionName);

std::string EnumMemberSourceSpelling(antlr4::tree::ParseTree* node);

std::string EnumMemberDiagnosticName(const LLVMBackend::NamedVariable& member,
                                            antlr4::tree::ParseTree* expression,
                                            const llvm::GlobalVariable* foundMember = nullptr);

antlr4::tree::TerminalNode* BareIdentifierInExpression(
    antlr4::tree::ParseTree* expression);

antlr4::tree::TerminalNode* GenericFunctionValueIdentifier(
    antlr4::tree::ParseTree* expression, bool allowAddressOf = true);

// Every '(' suffix and parenthesized primary under `node`: a single-call initializer has exactly one.
size_t CountCallParens(antlr4::tree::ParseTree* node);

// A field initializer may build straight into the field slot only when it is ONE call (T272): after
// peeling redundant parentheses, a postfix chain whose top suffix is the call's `( ... )` and that
// holds no other call or parenthesis. A conditional or a member access/subscript above the call fails.
bool FieldInitializerIsSingleCall(CFlatParser::AssignmentExpressionContext* expr);

// A zero-length member (`T m[0]`) still undecayed: the one operand `sizeof` measures as 0.
bool IsZeroLengthArrayMemberOperand(const LLVMBackend::TypeAndValue& type);

bool HasSoftDeclarationSpecifier(CFlatParser::DeclarationSpecifiersContext* specs,
                                        const std::string& name);

std::string ImportProgramAlias(CFlatParser::ImportDeclarationContext* imp);

std::string GenericFunctionTemplateSignature(
    CFlatParser::FunctionDefinitionContext* function, LLVMBackend* compiler);

std::string ImportNamespace(CFlatParser::ImportDeclarationContext* imp);

template <typename Fn>
static void ForEachImportClause(CFlatParser::ImportDeclarationContext* imp, Fn&& fn)
{
    if (imp == nullptr) return;
    fn(imp->libClause());
    fn(imp->frameworkClause());
    for (auto* clause : imp->defineClause())
        fn(clause);
    fn(imp->fromClause());
    fn(imp->priClause());
}

bool ImportClauseHasWord(antlr4::ParserRuleContext* clause, const char* word);

std::string ValidateImportSoftKeywords(CFlatParser::ImportDeclarationContext* imp);

std::string ValidateImportLeadingMarker(CFlatParser::ImportDeclarationContext* imp);

// Returns true when a function's entire body is a single 'return { ... };' statement,
// marking it as a return-block function (to be inlined at every call site).
bool IsReturnBlockFunction(CFlatParser::FunctionDefinitionContext* func);

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

std::string FixedArrayPointerTypeMessage(const std::string& typeName);

// simd needs its own wording: the generic message steers to 'T*', but a pointer to a simd
// vector is not a supported type either, so decaying is not the fix here.
std::string SimdPointerTypeMessage(const std::string& spelling, bool fixedArray);
std::string JoinTypeSpecifierWords(const std::vector<std::string>& words);

enum class PrimitiveTypeErrorKind
{
    None,
    ExpectedType,
    InvalidSpelling,
    InvalidSpellingSuggestion
};

struct PrimitiveTypeError
{
    PrimitiveTypeErrorKind kind = PrimitiveTypeErrorKind::None;
    std::string spelling;
    std::string suggestion;
};

bool HasPrimitiveTypeError(const PrimitiveTypeError& error);

bool HasLongDoubleTypeArgument(const std::vector<std::string>& typeArgs);

std::string LocalizePrimitiveTypeError(const LLVMBackend* compiler,
                                              const PrimitiveTypeError& error);

// Canonicalize one complete C/C++ primitive spelling. A single unknown word is left alone so
// user-defined and generic names continue through the ordinary name-resolution path.
bool CanonicalizePrimitiveTypeWords(const std::vector<std::string>& words,
                                           std::string& canonical, PrimitiveTypeError& error);

// A soft keyword parsed as a typeSpecifier that qualifies the declaration rather than naming
// part of its type.
bool IsSoftKeywordTypeSpecifier(const std::string& text);

std::vector<std::string> CollectDeclarationTypeSpecifierWords(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs);

// A '?', '*' or '[N]' written on a NON-final word of a multi-word type (`unsigned* int p`). Both
// passes read the suffix only from the last word, so it would be dropped. Returns the offending
// specifier and its suffix text, or null when every suffix follows the complete type.
CFlatParser::DeclarationSpecifierContext* MisplacedMultiWordTypeSuffix(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs,
    std::string& suffixText);

CFlatParser::DeclarationSpecifierContext* LastDeclarationTypeSpecifier(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs);

// The CFlat word a declaration's type specifiers spell, for diagnostics: `unsigned int` -> `u32`.
// A spelling that does not canonicalize keeps `fallback`; the type-parsing pass reports it.
std::string CanonicalDeclarationTypeName(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs,
    const std::string& fallback);

// True for non-pointer primitive destinations, excluding function-pointer types.
bool IsGenericFunctionScalarDestination(LLVMBackend* compiler,
                                               LLVMBackend::TypeAndValue destination);

std::string GenericFunctionScalarValueDiagnostic(LLVMBackend* compiler,
                                                        const std::string& functionName);

antlr4::tree::TerminalNode* BareGenericFunctionValueIdentifierInExpression(
    antlr4::tree::ParseTree* expression);

// True when `name` resolves as a visible variable in ParseIdentifier lookup order.
bool NamesVisibleVariable(LLVMBackend* compiler, const std::string& name);

// The declared type of the variable NamesVisibleVariable finds, looked up in the same order.
// Call only after NamesVisibleVariable returned true.
LLVMBackend::TypeAndValue VisibleVariableType(LLVMBackend* compiler, const std::string& name);

// The identifier a typeName spells when it is exactly one unqualified, non-generic name with no
// declarator (`(c16)`, `sizeof(Foo)`); null for any other shape.
antlr4::tree::TerminalNode* BareTypeNameIdentifier(CFlatParser::TypeNameContext* typeName);

antlr4::tree::TerminalNode* BareCastTypeNameIdentifier(CFlatParser::CastTypeNameContext* typeName);

std::vector<std::string> TypeSpecifierWords(
    CFlatParser::TypeSpecifierContext* typeSpec,
    CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr);

std::string CanonicalTypeSpecifierText(
    CFlatParser::TypeSpecifierContext* typeSpec,
    CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr,
    bool = false,
    PrimitiveTypeError* errorOut = nullptr);

// Keep C and C++ spellings of the same primitive on one specialization key.
std::string CanonicalTemplateTypeArgument(
    CFlatParser::TypeParameterEntryContext* entry, PrimitiveTypeError* errorOut = nullptr);

// An empty `[]` is representable ONLY as the sole dimension: the `T[]` array-view is a thin
// `ptr` and carries no row stride, so `T[][]`, `T[][M]` and `T[N][]` have no lowering. The
// grammar folds every bracket pair into one arrayDimSpec and drops the empty ones from
// assignmentExpression(), so an unsized dimension is visible only as a bracket-count mismatch.
bool DimSpecIsUnsizedMultiDim(CFlatParser::ArrayDimSpecContext* dims);
template <class Ctx>
static bool HasUnsizedMultiDim(Ctx* c)
{
    return DimSpecIsUnsizedMultiDim(ArrayDimsOf(c));
}
// One wording for every site that rejects an unsized multi-dimensional bracket form.
std::string UnsizedMultiDimMessage(const std::string& typeName);
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
template <class Ctx>
static std::string FunctionTypeArraySuffixError(Ctx* c)
{
    if (c == nullptr || c->arrayTypeSuffix() == nullptr || !IsBadArrayArg(c)) return {};
    return "'T[]' is the only bracketed form valid as a function-type parameter; a sized 'T[N]', "
        "an unsized multi-dimensional 'T[][]' and a pointer-to-array 'T[]*' are not";
}
// Number of '*' written on a pointer() context. The grammar is ('*' typeQualifierList?)+, so the
// depth is Star().size(); the funcptr signature sites used to collapse it to a bool.
int PointerDepthOf(CFlatParser::PointerContext* p);

std::vector<CFlatParser::TypeQualifierContext*> ConstQualifiersAfterPointerStar(
    CFlatParser::PointerContext* pointer);

std::string ConstAfterPointerDiagnostic(const std::string& spelledBaseType);

std::string SpelledTypeNameBase(
    const std::vector<CFlatParser::TypeSpecifierContext*>& typeSpecs);

std::string SpelledTypeNameBase(CFlatParser::TypeNameContext* typeName);

std::string SpelledTypeNameBase(
    const std::vector<CFlatParser::TypeSpecifierContext*>& typeSpecs);

std::string SpelledTypeNameBase(CFlatParser::TypeSpecifierContext* typeSpec,
                                       CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr);

// Type-arg string for one tuple element: the bare type plus its reference-kind suffix
// ("*" pointer, "[]" noalias array-view). Bad bracket forms (`[N]`, `[]*`) contribute no
// suffix here - the authoritative tuple-sugar path rejects them with a diagnostic.
// `compiler` namespace-resolves the element type the same way a generic type ARGUMENT is
// resolved, so the `(Item, int)` sugar and an explicit `tuple<Item, int>` keep mangling to one
// name inside a namespace that declares its own Item.
// Peel trailing '*' characters off a resolved type-alias string (e.g. "void*" -> "void"),
// returning the pointer depth removed. A pointer alias keeps its stars in the stored string
// (no type-descriptor struct); this re-derives the depth at the resolution site so it can be
// OR'd onto the flat TypeAndValue.Pointer / .ElemPointer flags.
int PeelAliasPointerStars(std::string& name);

// Resolve an enum base without its generic-argument pointer/view suffix, then restore that suffix.
template <class ResolveBase>
static bool ResolveEnumTypeArgumentBase(std::string& argument, ResolveBase&& resolveBase)
{
    const bool isArrayView = argument.ends_with("[]");
    size_t baseEnd = argument.size() - (isArrayView ? 2 : 0);
    while (baseEnd > 0 && argument[baseEnd - 1] == '*') --baseEnd;
    if (baseEnd == 0) return false;
    std::string resolved = resolveBase(argument.substr(0, baseEnd));
    if (resolved.empty()) return false;
    argument = std::move(resolved) + argument.substr(baseEnd);
    return true;
}

// Function signatures store pointer shape separately from the base type name.
std::string SignatureComponentAliasBase(LLVMBackend* compiler, std::string name);

// Fold a POINTER alias (`using IP = int*;`) into a type-ARGUMENT spelling: rebinds `name` to the
// alias target's bare base and adds the target's stars to `stars` (the ELEMENT depth for a view).
// One rule for every type-argument encoder, so `V<IP[]>` and `V<int*[]>` are one instantiation.
bool FoldPointerAliasArg(const LLVMBackend* compiler, std::string& name, int& stars);

struct TupleTypeElementResolveContext
{
    LLVMBackend* Compiler = nullptr;
    const std::unordered_map<std::string, std::string>* Substitutions = nullptr;
    std::string Namespace;
    std::unordered_set<std::string> LocalTypes;
    std::function<std::string(CFlatParser::TypeSpecifierContext*,
                              CFlatParser::MultiWordTypeSuffixContext*)> ResolveType;
    std::function<void(antlr4::ParserRuleContext*, std::string)> ReportError;
    std::function<std::string(const std::string&, bool, antlr4::ParserRuleContext*)> ResolveUniqueType;
};

std::string ResolveTupleTypeElementArgName(
    const TupleTypeElementResolveContext& context,
    CFlatParser::TupleTypeEntryContext* entry,
    PrimitiveTypeError* errorOut = nullptr);

// True when a tuple element names a type. Judged by the parse-tree shape (simd<...> and
// function<...>/Lambda<...> are type keywords) or by the resolved spelling's base after the
// pointer and view suffixes are peeled (`int*` -> `int`), or a function-type alias. The pre-scans
// use it to decide whether a parenthesized sugar is a tuple type; a bare identifier pair `(a, b)`
// names no type and stays out, so a `*` suffix never turns an expression into a type.
bool TupleElementEntryNamesType(const LLVMBackend* compiler,
    CFlatParser::TupleTypeEntryContext* entry, const std::string& argName);
// The element's spelled base for a diagnostic: `Nope*` -> `Nope`, as a plain `Nope* p;` reports it.
std::string TupleElementBaseName(std::string argName);

std::string getOperatorName(CFlatParser::OperatorFunctionIdContext* opId);

std::string getOperatorName(CFlatParser::OperatorFunctionIdContext* opId,
                                  const LLVMBackend* compiler);

std::string getInterfaceMethodName(CFlatParser::InterfaceMethodContext* m);

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

// Decoded text of one annotation argument; non-string tokens are returned verbatim.
std::string AnnotationArgOne(CFlatParser::AnnotationArgContext* arg);

// Every quote-stripped argument of an annotation, in source order (empty when it has none).
std::vector<std::string> AnnotationArgTexts(CFlatParser::AnnotationContext* ann);

// The FIRST argument of an annotation (empty if it has none). Single-arg annotations
// ([uuid], [JsonName], [MaxLength]) read through this; multi-arg ones read Values.
std::string AnnotationArgText(CFlatParser::AnnotationContext* ann);

// Extract every [Name(arg, ...)] as a raw annotation value, WITHOUT registry validation. Used by
// the forward scan to record type-level annotations (e.g. [uuid]) before the main pass validates.
std::vector<LLVMBackend::AnnotationValue> ExtractAnnotations(CFlatParser::AnnotationListContext* annList);

std::optional<int64_t> FoldCompileTimeIntLeaf(LLVMBackend* compiler,
                                                     antlr4::tree::ParseTree* node);
std::optional<int64_t> FoldCompileTimeIdentifier(LLVMBackend* compiler,
                                                        CFlatParser::GenericIdentifierContext* gid);
bool InScannerInt32Range(int64_t v);
bool ScannerAddOverflow(int64_t a, int64_t b, int64_t* out);
bool ScannerSubOverflow(int64_t a, int64_t b, int64_t* out);
bool ScannerMulOverflow(int64_t a, int64_t b, int64_t* out);
std::optional<int64_t> ParseScannerIntegerLiteral(const std::string& textIn);
std::string ResolveCastTypeNameForScanner(
    LLVMBackend* compiler, CFlatParser::CastTypeNameContext* castTypeName);

/*
 * The shared compile-time integer folder, callable from BOTH passes (the ForwardRefScanner
 * members ScannerFoldIfConst / ...Leaf are thin forwarders onto it). It mirrors
 * EvalIfConstConstant's tree walk arm for arm - ?:, ||, &&, expression / assignmentExpression
 * unwrap, and the same "any known-false wins" partial-knowledge rules - and only substitutes a
 * constant-folding leaf for the emitting one. Any change to EvalIfConstConstant's structure must
 * be mirrored here. Whatever folds to nullopt is undecidable, which every caller handles safely.
 */
// A grouped &&/||/?: that the syntax folder may decide. Typed casts are excluded: the folder computes
// in plain int64, so `(u32)0` would lose its unsignedness in a relational compare.
bool HasNoTypedCast(antlr4::tree::ParseTree* node);
// Structural (never text: a string literal can hold "&&" or "?").
bool HasShortCircuit(antlr4::tree::ParseTree* node);
bool HasTernary(antlr4::tree::ParseTree* node);
bool IsStructurallyGrouped(antlr4::tree::ParseTree* node);
bool IsGroupedFoldCandidate(antlr4::tree::ParseTree* node);
std::optional<int64_t> FoldCompileTimeInt(LLVMBackend* compiler, antlr4::tree::ParseTree* node);
bool HasUnfoldableCountofCall(LLVMBackend* compiler, antlr4::tree::ParseTree* node);
void SeedScanTimeLocalVariableTypes(LLVMBackend* compiler,
                                    CFlatParser::DeclarationContext* declaration);

// The leaf half: constant-folds the integer operator chain without emitting IR. Covers a
// compile-time macro (a -D define, a platform builtin, or a seeded const int global), an integer
// literal, redundant parens, unary !/-/~/+, and the comparison / bitwise / arithmetic chain over
// those. Anything else folds to nullopt.
std::optional<int64_t> FoldCompileTimeIntLeaf(LLVMBackend* compiler, antlr4::tree::ParseTree* node);

// A const integer local folds only when its value is exactly representable in its own type.
bool ConstLocalFoldFits(llvm::IntegerType* type, int64_t value);

/*
 * A pre-pass has no local frames, so a value generic argument naming a const integer local
 * (`const int K = 4; Buf<int, K> b;`) is found lexically. Every binding construct of the grammar
 * either folds (a const integer declarator) or shadows: earlier block items and earlier declarators
 * of the same declaration, for-init declarators, range-for and case-arm variables, lambda, function
 * and generic parameters, destructuring entries and block `using` names. `found` reports a binding.
 */
using LexicalConstTypeParser =
    std::function<LLVMBackend::TypeAndValue(CFlatParser::DeclarationSpecifiersContext*)>;
// Set while a lexical const initializer folds, so its identifiers resolve in their own scope.
inline thread_local const LexicalConstTypeParser* activeLexicalConstTypeParser = nullptr;
struct LexicalConstParserScope
{
    const LexicalConstTypeParser* saved = activeLexicalConstTypeParser;
    explicit LexicalConstParserScope(const LexicalConstTypeParser* p) { activeLexicalConstTypeParser = p; }
    ~LexicalConstParserScope() { activeLexicalConstTypeParser = saved; }
    LexicalConstParserScope(const LexicalConstParserScope&) = delete;
    LexicalConstParserScope& operator=(const LexicalConstParserScope&) = delete;
};
std::string getDirectDeclName(CFlatParser::DirectDeclaratorContext* d);
bool LexicalIsUnder(antlr4::tree::ParseTree* node, antlr4::tree::ParseTree* ancestor);
// A generic type or value parameter of that name shadows any outer constant.
bool LexicalGenericParamNamed(CFlatParser::GenericTypeParametersContext* g, const std::string& name);
/*
 * The last declarator of `list` named `name` visible at `use`. A use inside `owner` (the
 * declaration itself) sees only the declarators before the one holding it; a use in the
 * specifiers sees none. `self`: the declarator holding the use declares `name` (C++ point of
 * declaration), so the name is that unfoldable object, never an outer one.
 */
CFlatParser::InitDeclaratorContext* LexicalVisibleDeclarator(antlr4::tree::ParseTree* owner,
    CFlatParser::InitDeclaratorListContext* list, const std::string& name, antlr4::tree::ParseTree* use,
    bool* self);
// A block declaration that introduces a TYPE of that name (a local enum, a struct/union body in
// the specifiers): the name is no value. Enumerators are scoped (`E.n`) and add no bare name.
bool LexicalDeclaresTypeNamed(CFlatParser::DeclarationContext* decl, const std::string& name);
bool LexicalBlockHasPriorEnum(CFlatParser::BlockItemListContext* list,
    CFlatParser::BlockItemContext* item, const std::string& name);
antlr4::tree::TerminalNode* LexicalDirectDeclaratorIdentifier(
    CFlatParser::DirectDeclaratorContext* direct);
enum class LexicalBlockBindingKind { None, Value, Type };
struct LexicalBlockBinding
{
    LexicalBlockBindingKind Kind = LexicalBlockBindingKind::None;
    CFlatParser::DeclarationSpecifiersContext* Specifiers = nullptr;
    CFlatParser::InitDeclaratorContext* Declarator = nullptr;
    bool HasVisibleValue = false;
};
// The latest visible binding in one block item list; shared by const folding and value lookup.
LexicalBlockBinding FindLexicalBlockBinding(CFlatParser::BlockItemListContext* list,
    antlr4::tree::ParseTree* use, const std::string& name);
bool LexicalFunctionHasParameter(CFlatParser::FunctionDefinitionContext* fn,
    const std::string& name);
bool LexicalLambdaHasParameter(CFlatParser::LambdaExpressionContext* lambda,
    const std::string& name);
bool LexicalBodyHasValueParameter(CFlatParser::BlockItemListContext* list,
    const std::string& name);
enum class LexicalBindingEventKind
{
    Block, FunctionParameter, GenericParameter, LambdaParameter, CaseBinding,
    RangeBinding, ForDeclaration
};
struct LexicalBindingEvent
{
    LexicalBindingEventKind Kind;
    LexicalBlockBinding BlockBinding;
    CFlatParser::BlockItemListContext* Block = nullptr;
    CFlatParser::DeclarationSpecifiersContext* Specifiers = nullptr;
    CFlatParser::InitDeclaratorContext* Declarator = nullptr;
    bool Self = false;
    bool HasSameScopeParameter = false;
};
// Both lexical const folding and name lookup consume binders in the same ancestor order.
template <typename Visitor>
static void WalkLexicalBindings(antlr4::tree::ParseTree* use, const std::string& name,
    Visitor&& visit)
{
    for (antlr4::tree::ParseTree* node = use; node != nullptr && node->parent != nullptr;
         node = node->parent)
    {
        auto* parent = node->parent;
        if (auto* lambda = dynamic_cast<CFlatParser::LambdaExpressionContext*>(parent);
            lambda != nullptr && LexicalLambdaHasParameter(lambda, name))
            if (visit(LexicalBindingEvent{ LexicalBindingEventKind::LambdaParameter })) return;
        if (auto* arm = dynamic_cast<CFlatParser::LabeledStatementContext*>(parent);
            arm != nullptr && arm->typeSpecifier() != nullptr && arm->Identifier() != nullptr
            && node == arm->statement() && arm->Identifier()->getText() == name)
            if (visit(LexicalBindingEvent{ LexicalBindingEventKind::CaseBinding })) return;
        if (auto* loop = dynamic_cast<CFlatParser::IterationStatementContext*>(parent))
        {
            if (loop->In() != nullptr && node == loop->statement() && loop->Identifier() != nullptr
                && loop->Identifier()->getText() == name)
                if (visit(LexicalBindingEvent{ LexicalBindingEventKind::RangeBinding })) return;
            if (loop->forCondition() != nullptr)
                if (auto* forDecl = loop->forCondition()->forDeclaration())
                {
                    bool self = false;
                    auto* match = LexicalVisibleDeclarator(
                        forDecl, forDecl->initDeclaratorList(), name, use, &self);
                    if (self || match != nullptr)
                    {
                        LexicalBindingEvent event{ LexicalBindingEventKind::ForDeclaration };
                        event.Specifiers = forDecl->declarationSpecifiers();
                        event.Declarator = match;
                        event.Self = self;
                        if (visit(event)) return;
                    }
                }
        }
        if (auto* fn = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(parent))
        {
            if (LexicalFunctionHasParameter(fn, name))
                if (visit(LexicalBindingEvent{ LexicalBindingEventKind::FunctionParameter })) return;
            if (LexicalGenericParamNamed(fn->genericTypeParameters(), name))
                if (visit(LexicalBindingEvent{ LexicalBindingEventKind::GenericParameter })) return;
        }
        if (auto* sd = dynamic_cast<CFlatParser::StructDefinitionContext*>(parent))
            if (LexicalGenericParamNamed(sd->genericTypeParameters(), name))
                if (visit(LexicalBindingEvent{ LexicalBindingEventKind::GenericParameter })) return;
        if (auto* list = dynamic_cast<CFlatParser::BlockItemListContext*>(parent))
        {
            const auto binding = FindLexicalBlockBinding(list, node, name);
            if (binding.Kind != LexicalBlockBindingKind::None)
            {
                LexicalBindingEvent event{ LexicalBindingEventKind::Block };
                event.BlockBinding = binding;
                event.Block = list;
                event.HasSameScopeParameter = LexicalBodyHasValueParameter(list, name);
                if (visit(event)) return;
            }
        }
    }
}
bool LexicalLocalTypeShadowsValue(LLVMBackend* compiler, antlr4::tree::ParseTree* use,
    const std::string& name);
bool LexicalHasFunctionScope(antlr4::tree::ParseTree* node);
void RegisterLexicalLocalTypeName(LLVMBackend* compiler,
    antlr4::tree::ParseTree* declaration, const std::string& name);
void RegisterLexicalStructNamesInDeclaration(LLVMBackend* compiler,
    CFlatParser::DeclarationContext* declaration);
struct LexicalLocalTypeScope
{
    LLVMBackend* Compiler;
    explicit LexicalLocalTypeScope(LLVMBackend* compiler) : Compiler(compiler)
    {
        if (Compiler != nullptr) Compiler->PushLexicalLocalTypeScope();
    }
    ~LexicalLocalTypeScope()
    {
        if (Compiler != nullptr) Compiler->PopLexicalLocalTypeScope();
    }
};
std::optional<int64_t> FoldLexicalConstLocal(LLVMBackend* compiler, antlr4::tree::ParseTree* use,
    const std::string& name, const LexicalConstTypeParser& parseType, bool* found = nullptr);

// A bare identifier that names a compile-time integer: a -D define or a platform builtin
// (compileTimeMacros), or a file-scope `const` integer global the scanner seeded. Anything
// generic, qualified or unknown is not constant.
std::optional<int64_t> FoldCompileTimeIdentifier(LLVMBackend* compiler,
                                                        CFlatParser::GenericIdentifierContext* gid);

// True when a value is representable as codegen's default `int` (i32). Every value this folder
// produces is kept inside this range, so no fold can silently differ from codegen by a 32-bit
// wraparound - the alternative reproduces codegen's exact literal typing, which the folder
// cannot see (suffixes, u32/long promotion), so out-of-range folds to nullopt instead.
bool InScannerInt32Range(int64_t v);

// Portable stand-ins for __builtin_*_overflow, which MSVC does not provide. Each computes in
// unsigned (well-defined wraparound) and reports whether the signed result overflowed.
bool ScannerAddOverflow(int64_t a, int64_t b, int64_t* out);

bool ScannerSubOverflow(int64_t a, int64_t b, int64_t* out);

bool ScannerMulOverflow(int64_t a, int64_t b, int64_t* out);

// An integer literal this folder can type EXACTLY the way codegen does, else nullopt.
// Deliberately narrow, because a literal typed differently from codegen is a silent
// scanner/main-pass disagreement, which surfaces as a raw LLVM verifier failure with no source
// location:
//   - a u/U/l/L suffix changes the type (unsigned / 64-bit) -> refuse
//   - a C OCTAL literal (leading 0) is base 8 to codegen -> refuse rather than misread as decimal
//   - a value outside i32 (0xFFFFFFFF, 0x80000000, 2147483648) is typed and wrapped by codegen
//     in a way this folder cannot reproduce -> refuse
// Refusing only makes the condition undecidable, which the if-const walk already handles safely.
std::optional<int64_t> ParseScannerIntegerLiteral(const std::string& textIn);

// Extract the plain identifier from a directDeclarator (handles both bare names and C-style array syntax).
std::string getDirectDeclName(CFlatParser::DirectDeclaratorContext* d);

// A parameter's C-style array suffix hangs on its declarator rather than its specifiers.
// Keep this structural check available to both declaration-specifier parsers.
bool HasParameterArrayDeclarator(CFlatParser::DeclarationSpecifiersContext* specs);

// Normalize a generic type argument for use in mangled names (e.g. "Employee*" -> "Employeeptr",
// "int[]" -> "intview"). A type-arg string carries its reference kind as a suffix: "*" for a
// pointer, "[]" for a noalias array-view (see PeelTypeArgSuffix). Both must fold to symbol-safe
// text so the mangled name stays a valid identifier.
// True if a type-parameter entry carries the `unique` qualifier: a leading Identifier
// (grammar: `Identifier? typeSpecifier ...`) whose text is exactly "unique".
bool TypeArgHasUnique(CFlatParser::TypeParameterEntryContext* entry);

// A blessed core `unique<X>` substitution answers `is_copyable` as its former raw-pointer
// spelling. The wrapper is transparent to this generic predicate; the interface arm stays a bare
// interface name. Sole remaining caller is is_copyable; is_unique answers IsCoreUniqueType.
std::string LegacyUniqueSubstitutionSpelling(const LLVMBackend* compiler,
                                                    const std::string& resolved);

/*
 * The allocation alignment a core `unique<T, N>` DECLARES, read off its own ALIGN generic
 * argument. The `alignas(0, N) unique T*` sugar records the same number on AllocAlignValue as it
 * mangles the wrapper name, so the explicitly-spelled generic must fold back to the identical
 * TypeAndValue or only the sugar seeds the inbound allocation-alignment channel. Returns 0 when
 * the type is not a core unique, carries no second argument, or the argument is not a number.
 */
uint64_t CoreUniqueDeclaredAllocAlign(const LLVMBackend* compiler,
                                             const std::string& typeName);

// Shape the argument handed to unique<T>'s constructor: the pointer arm hands over the pointee
// pointer, the interface arm hands over the interface VALUE (or an implementor pointer to box).
void ShapeCoreUniqueCtorArg(const LLVMBackend* compiler,
                                   LLVMBackend::NamedVariable& ctorArg,
                                   const std::string& uniqueTypeName, llvm::Value* value);

// A function-pointer SIGNATURE keeps the raw pointee spelling a desugared `unique X*` element
// had before the desugar, so a user lambda stays spelled `X*` and the indirect-call adaptation
// borrows the wrapper into it. The interface arm has no pointer spelling and is left alone.
void CanonicalizeUniqueSigComponent(const LLVMBackend* compiler, std::string& typeName,
                                           bool& pointer, int& pointerDepth);

// True when `unique X*` / `unique IFace` in GENERIC-ARGUMENT position spells the core
// `unique<X>` value. Unsupported forms are rejected before a type-argument spelling is mangled.
bool CanDesugarUniqueTypeArg(const LLVMBackend* compiler, const std::string& base,
                                    bool hasPointer, int pointerDepth, bool hasArrayView,
                                    bool baseIsGenericInstantiation = false);

// True when `name` is a type PARAMETER of a generic template (function, struct or class)
// enclosing `node` - the `T` in `int take<T>(unique T* p)` while T is still unbound.
bool NamesEnclosingTypeParameter(antlr4::tree::ParseTree* node, const std::string& name);

// A `unique T*` DECLARATION (parameter, return, local, field) whose pointee still names an UNBOUND
// template type parameter must not desugar here: the queued unique<T> would carry the literal
// parameter name and its body would fail on `using P = T*`. Defer to instantiation, where the
// same specifiers are re-parsed with T bound - the deferral the explicit `unique<T>` spelling gets.
bool UniqueDeclPointeeIsUnbound(const LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                       const std::string& name);

// The `alias` borrow qualifier on a generic type argument is carried the same way as `unique`:
// a leading "alias " prefix on the resolved type-arg string. It denotes a pure borrow element
// (list<alias T*>) - behaviorally identical to a bare pointer element, but instantiated
// distinctly so accessor returns can be tagged for the delete-of-borrow check.
static const char kAliasQualifierPrefix[] = "alias ";
static const size_t kAliasQualifierPrefixLen = sizeof(kAliasQualifierPrefix) - 1;

// True if a type-parameter entry carries the `alias` qualifier: a leading Identifier
// (grammar: `Identifier? typeSpecifier ...`) whose text is exactly "alias".
bool TypeArgHasAlias(CFlatParser::TypeParameterEntryContext* entry);

bool IsValueParameterDeclaration(CFlatParser::TypeParameterEntryContext* entry);

std::string ValueParameterTypeSpelling(CFlatParser::TypeParameterEntryContext* entry);

bool IsDecimalIntegerSpelling(const std::string& text);

// A generic VALUE argument is carried as a decimal spelling ("8", "-8"); this is the one place
// it turns back into a number. Not a validity check - IsDecimalIntegerSpelling gates that.
int64_t ParseValueArg(const std::string& text);

// The value-producing side of an initializer/assignment, when it is a heap ARRAY allocation
// (`new T[n]`). Recurses through parens, the assignment/conditional chain and the `?:`/`??`
// arms so a joined allocation is found too; never descends into a call argument or a
// condition, which are not the value. Pure DIAGNOSTIC: nothing about the representation is
// decided here.
CFlatParser::NewExpressionContext* RawNewArrayValueOf(antlr4::tree::ParseTree* node);

// T250: true when the RHS IS an assignment, reached only through parentheses and single-child links.
// A member access, cast or call around it consumes the value differently, so it never takes the slot.
bool IsChainedAssignmentShape(antlr4::tree::ParseTree* node);

// Fill omitted TRAILING generic arguments from the template's declared defaults. Returns true
// when every parameter now has an argument. Shared by the struct/class and function
// instantiation paths so both agree with the canonical mangled name.
bool FillGenericValueDefaults(const LLVMBackend& compiler, const std::string& templateName,
                                     size_t paramCount, std::vector<std::string>& typeArgs);

// True when the template declares at least one defaulted parameter, i.e. it opted into the
// arity check that a template without defaults has never had.
bool TemplateHasGenericDefaults(const LLVMBackend& compiler, const std::string& templateName);

// Strip a leading "alias " qualifier off a resolved type-arg string, returning whether it was
// present. The bare base (e.g. "Circle*") remains so it resolves to the same LLVM type as the
// unqualified spelling; the alias prefix only drives distinct monomorphization + borrow tagging.
bool StripAliasQualifier(std::string& name);

// Type parameter -> argument spelling of the instantiation being walked. An argument's pointee
// const ("(const int)*") is kept beside the plain spelling, which every reader of the map sees;
// copies (save/restore around a nested instantiation) carry both; a view's element const too.
class TypeSubstitutionMap : public std::unordered_map<std::string, std::string>
{
public:
    std::string& operator[](const std::string& name)
    {
        constDepths_.erase(name);
        return std::unordered_map<std::string, std::string>::operator[](name);
    }
    size_t erase(const std::string& name)
    {
        constDepths_.erase(name);
        return std::unordered_map<std::string, std::string>::erase(name);
    }
    // Binds `name` to a spelled or mangled argument, peeling its pointee const.
    void Bind(const std::string& name, const std::string& arg)
    {
        std::string plain;
        int depth = 0;
        if (SplitConstTypeArgument(arg, plain, depth))
        {
            std::unordered_map<std::string, std::string>::operator[](name) = plain;
            constDepths_[name] = depth;
            return;
        }
        (*this)[name] = arg;
    }
    int ConstDepth(const std::string& name) const
    {
        auto it = constDepths_.find(name);
        return it == constDepths_.end() ? 0 : it->second;
    }
private:
    std::unordered_map<std::string, int> constDepths_;
};

// Strip the leading `alias ` qualifier off a resolved type-arg string. It is a generic
// instantiation marker, not part of the underlying LLVM type.
void StripOwnershipQualifiers(std::string& name);

// The type-mangling funnel owns primitive canonicalization for both generic identities and
// overload symbol names, so one spelling cannot monomorphize differently from how it overloads.
// `compiler` is REQUIRED (no default): it supplies the pure-rename `using` alias fold, and a site
// that silently skipped it would mangle a name the other pass spells differently. Every current
// call site reaches one; a nullptr is only correct where the string cannot name a source alias.
std::string MangleTypeArg(const LLVMBackend* compiler, const std::string& typeName);

std::string SpellFunctionParameterValueType(
    const LLVMBackend& compiler, const LLVMBackend::TypeAndValue& type);

// One `using` declaration considered as a mangling-visible PURE RENAME. Shared by the two
// pre-scans (ForwardRefScanner::ScanGenericTypeUses and MainListener::ScanAndQueueGenericTypeUses)
// so a body-scope alias binds to the same target in both. With an alias frame open the rename
// lands in THAT frame, so it never displaces or outlives a file-scope alias of the same name.
// `using X = const W*;`: whole-word const over every level of the aliased word (ApplyWholeWordConst).
uint32_t UsingWholeWordConstMask(LLVMBackend* compiler, const std::string& target, size_t suffixStars);

std::vector<std::string> EnclosingAggregateNamesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);
std::string NamespaceNameFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);
bool IsUnparameterizedNamedTypeSpecifier(CFlatParser::TypeSpecifierContext* typeSpec);
std::string ResolveContextualTypeName(LLVMBackend* compiler,
    const std::string& spelled, const std::string& resolvedName,
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::vector<std::string>& aggregateScopes, const std::string& namespaceName);

void RegisterPureRenameAlias(LLVMBackend* compiler, CFlatParser::UsingDeclarationContext* ctx);

// Namespace-resolve a type-argument string that carries a reference-kind suffix ("Item*",
// "Item[]"): only the bare core participates in the walk. For the two scanner paths that build the
// arg string from a raw getText(); paths that keep the core separate call
// LLVMBackend::ResolveTypeArgBaseName directly. No ownership-prefix handling: getText() has no
// whitespace, so a `unique`/`alias` arg arrives as "uniqueItem*" and never reaches here as a
// prefixed spelling (those two are routed through ResolveForwardTypeArg instead).
std::string ResolveTypeArgSpelling(const LLVMBackend* compiler, const std::string& arg);

// The generic instantiation written at `ts`, covering BOTH spellings: the bare `Box<int>`
// (genericIdentifier) and the namespace-qualified `Windows.Foundation.IReference<int>`
// (qualifiedGenericIdentifier). `base` receives the base name, dotted when it was written dotted.
// Returns the <...> argument list, or nullptr when `ts` is not a generic instantiation (in which
// case callers fall through to their plain-type path, which reads getText()).
CFlatParser::GenericTypeParametersContext* GenericSpecOf(
    CFlatParser::TypeSpecifierContext* ts, std::string& base);

bool IsGlobalScopeQualifiedType(CFlatParser::TypeSpecifierContext* typeSpec);
std::string RootQualifiedTypeSpelling(CFlatParser::TypeSpecifierContext* typeSpec);
std::string ResolveQualifiedTypeSpecifierName(
    LLVMBackend* compiler, CFlatParser::TypeSpecifierContext* typeSpec,
    const std::string& spelling = {}, bool resolveFromRoot = false);

struct EnclosingAggregateLookupScope
{
    std::string Name;
    antlr4::ParserRuleContext* Definition = nullptr;
    bool NameIsResolved = false;
};

// Why a generic spelling resolved to no template: a non-template type of an enclosing aggregate,
// or of an enclosing namespace (the two get different diagnostics).
enum class NonGenericRefusal { None, EnclosingAggregate, EnclosingNamespace };

std::string NamespaceNameFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

bool IsUnparameterizedNamedTypeSpecifier(CFlatParser::TypeSpecifierContext* typeSpec);

std::vector<std::string> NamedTypePathFromContext(CFlatParser::TypeSpecifierContext* typeSpec);

bool HasNestedAggregatePathInContext(antlr4::tree::ParseTree* useSite,
    size_t aggregateIndex, const std::vector<std::string>& path);

std::vector<EnclosingAggregateLookupScope> EnclosingAggregateTypeScopesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

std::vector<EnclosingAggregateLookupScope> EnclosingAggregateScopesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

std::vector<EnclosingAggregateLookupScope> EnclosingAggregateTypeScopesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

std::vector<std::string> EnclosingAggregateNamesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

std::string AggregateDefinitionName(antlr4::ParserRuleContext* definition);

std::string ResolveContextualTypeName(LLVMBackend* compiler,
    const std::string& spelled, const std::string& resolvedName,
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::vector<std::string>& aggregateScopes, const std::string& namespaceName);

std::vector<EnclosingAggregateLookupScope> EnclosingAggregateScopesForCall(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

std::vector<EnclosingAggregateLookupScope> EnclosingAggregateScopesFromName(
    const std::string& scopeName);

std::string EnclosingAggregateScopeFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node);

// A function or destructor body is a complete-class context: every member of the enclosing
// aggregate is visible there, declared later or not. Field types and signatures keep order.
bool IsCompleteClassBodyUse(antlr4::tree::ParseTree* node);

bool HasVisibleNonGenericAggregateName(
    const EnclosingAggregateLookupScope& scope, const std::string& name,
    antlr4::tree::ParseTree* useSite);

// True when a non-template type `name` in the use's namespace chain (innermost first, declared
// before the use) hides a generic template of that name further out.
bool NonGenericNamespaceTypeHides(LLVMBackend* compiler, antlr4::tree::ParseTree* useSite,
    const std::string& name);

bool HasEnclosingAggregateContext(antlr4::tree::ParseTree* node);

bool IsAggregateMemberDeclarationTypeUse(antlr4::tree::ParseTree* node);

bool GenericTemplateDeclaredBeforeUse(
    LLVMBackend* compiler, const std::string& base, antlr4::tree::ParseTree* useSite);

bool GenericTemplateKeyNestedInScope(const std::string& key, const std::string& scope);

bool GenericTemplateKeyNestedInAnyScope(
    const std::string& key, const std::vector<EnclosingAggregateLookupScope>& scopes);

std::string ResolveGenericTemplateBaseAtScope(
    LLVMBackend* compiler, const std::string& spelledBase,
    const std::vector<EnclosingAggregateLookupScope>& enclosingScopes,
    antlr4::tree::ParseTree* useSite = nullptr, const std::string& lookupName = {},
    NonGenericRefusal* nonGenericRefusal = nullptr);

std::string ResolveGenericTemplateSpellingAtScope(
    LLVMBackend* compiler, const std::string& spelledBase, std::vector<std::string>& args,
    const std::vector<EnclosingAggregateLookupScope>& enclosingScopes,
    antlr4::tree::ParseTree* useSite,
    const std::string& lookupName, bool report, NonGenericRefusal* nonGenericRefusal = nullptr);

std::vector<EnclosingAggregateLookupScope> EnclosingScopesForLegacyScope(
    LLVMBackend* compiler, const std::string& scopeName, antlr4::tree::ParseTree* useSite);

std::string ResolveGenericTemplateBaseAtScope(
    LLVMBackend* compiler, const std::string& spelledBase, const std::string& enclosingScope,
    antlr4::tree::ParseTree* useSite = nullptr, const std::string& lookupName = {},
    NonGenericRefusal* nonGenericRefusal = nullptr);

std::string ResolveGenericTemplateSpellingAtScope(
    LLVMBackend* compiler, const std::string& spelledBase, std::vector<std::string>& args,
    const std::string& enclosingScope, antlr4::tree::ParseTree* useSite,
    const std::string& lookupName, bool report, NonGenericRefusal* nonGenericRefusal = nullptr);

using TypeNameArgumentResolver = std::function<std::string(CFlatParser::TypeParameterEntryContext*)>;
using TypeNameMemberResolver = std::function<std::string(CFlatParser::MemberTypeSpecifierContext*)>;
using TypeNameTupleResolver = std::function<std::string(CFlatParser::TupleTypeSpecifierContext*)>;
using CxxTypeErrorReporter = void (*)(void*, antlr4::ParserRuleContext*, const std::string&);

struct CanonicalTypeSpecifier
{
    std::string TypeName;
    std::string GenericBase;
    std::vector<std::string> GenericArgs;
    int PointerDepth = 0;
    uint32_t ConstMask = 0;
    bool IsGeneric = false;
};

bool ResolveCanonicalTypeSpecifier(
    LLVMBackend* compiler, const std::vector<CFlatParser::TypeSpecifierContext*>& specs,
    const std::unordered_map<std::string, std::string>* substitutions,
    const TypeNameArgumentResolver& resolveTypeArg,
    const TypeNameMemberResolver& resolveMemberType,
    bool reportGenericAlias, CanonicalTypeSpecifier& out,
    PrimitiveTypeError* errorOut = nullptr,
    const TypeNameTupleResolver& resolveTupleType = {},
    CxxTypeErrorReporter reportCxxTypeError = nullptr, void* reportContext = nullptr);

struct ConstTypeSyntax
{
    std::string TypeName;
    int PointerDepth = 0;
    uint32_t ConstMask = 0;
    bool IsArrayView = false;
};

void ApplyConstMaskThroughDepth(uint32_t& mask, int depth);

bool ResolveConstTypeSyntax(
    LLVMBackend* compiler, CFlatParser::TypeNameContext* ctx, ConstTypeSyntax& out,
    const std::unordered_map<std::string, std::string>* substitutions = nullptr,
    const TypeNameArgumentResolver& resolveTypeArg = {},
    const TypeNameMemberResolver& resolveMemberType = {},
    const TypeNameTupleResolver& resolveTupleType = {},
    CxxTypeErrorReporter reportCxxTypeError = nullptr, void* reportContext = nullptr);

bool ResolveConstTypeSpecifierSyntax(
    LLVMBackend* compiler, CFlatParser::TypeSpecifierContext* typeSpec, ConstTypeSyntax& out,
    const std::unordered_map<std::string, std::string>* substitutions = nullptr,
    const TypeNameArgumentResolver& resolveTypeArg = {},
    const TypeNameMemberResolver& resolveMemberType = {},
    const TypeNameTupleResolver& resolveTupleType = {},
    CxxTypeErrorReporter reportCxxTypeError = nullptr, void* reportContext = nullptr);

bool ResolveConstCastTypeSyntax(
    LLVMBackend* compiler, CFlatParser::CastTypeNameContext* ctx, ConstTypeSyntax& out,
    const std::unordered_map<std::string, std::string>* substitutions = nullptr,
    const TypeNameArgumentResolver& resolveTypeArg = {},
    const TypeNameMemberResolver& resolveMemberType = {},
    const TypeNameTupleResolver& resolveTupleType = {},
    CxxTypeErrorReporter reportCxxTypeError = nullptr, void* reportContext = nullptr);

std::string ResolveCastTypeNameForScanner(
    LLVMBackend* compiler, CFlatParser::CastTypeNameContext* castTypeName);

std::unordered_set<std::string> CollectFunctionLocalEnumNames(
    CFlatParser::FunctionDefinitionContext* function);

bool TypeArgumentNamesFunctionLocalEnum(
    CFlatParser::TypeParameterEntryContext* entry,
    const std::unordered_set<std::string>& localEnumNames);

std::string SimpleTypeSpecifierName(CFlatParser::TypeSpecifierContext* typeSpec);

bool TypeSpecifierNamesFunctionLocalEnum(
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::unordered_set<std::string>& localEnumNames);

bool TypeArgumentNamesFunctionLocalEnum(
    CFlatParser::TypeParameterEntryContext* entry,
    const std::unordered_set<std::string>& localEnumNames);

bool GenericArgumentsNameFunctionLocalEnum(
    CFlatParser::GenericTypeParametersContext* params,
    const std::unordered_set<std::string>* localEnumNames);

bool TypeArgumentNeedsFunctionLocalEnumDeferral(
    CFlatParser::TypeParameterEntryContext* entry,
    const std::unordered_set<std::string>& localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames);

bool TypeSpecifierNeedsFunctionLocalEnumDeferral(
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::unordered_set<std::string>& localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames);

bool TypeArgumentNeedsFunctionLocalEnumDeferral(
    CFlatParser::TypeParameterEntryContext* entry,
    const std::unordered_set<std::string>& localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames);

bool GenericArgumentsNeedFunctionLocalEnumDeferral(
    CFlatParser::GenericTypeParametersContext* params,
    const std::unordered_set<std::string>* localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames);

bool TypeArgumentUsesRegisteredLocalEnum(
    CFlatParser::TypeParameterEntryContext* entry,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions = nullptr);

bool TypeSpecifierUsesRegisteredLocalEnum(
    CFlatParser::TypeSpecifierContext* typeSpec,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions);

bool TypeArgumentUsesRegisteredLocalEnum(
    CFlatParser::TypeParameterEntryContext* entry,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions);

bool GenericArgumentsUseRegisteredLocalEnum(
    CFlatParser::GenericTypeParametersContext* params,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions = nullptr);

// Declarators, `new` results and casts carry up to 31 pointer levels (ConstMask bit per level).
static const int MaxDeclaratorPointerDepth = 31;

// Count of contiguous const levels from the base (bit 0 = pointee of the first '*'), capped at the
// declarator depth limit. A spelled `(const E)` argument joins as this many levels.
inline int ContiguousLowConstLevels(uint32_t mask)
{
    int depth = 0;
    while (depth < MaxDeclaratorPointerDepth && (mask & (uint32_t{ 1 } << depth)) != 0) ++depth;
    return depth;
}

// Reconcile a written '*' count with the pointer flag ResolveSigComponent* may have CHANGED - a
// `string` component resolves to char* with no '*' written. The FLAG wins; the count only adds
// precision when the two agree.
int ReconcilePointerDepth(bool pointer, int stars);

// Keep pointee const at `depth` levels. An alias result at depth 0 also keeps its referent's const
// (the value itself); a pointer alias's referent is its pointee, already kept. By-value results drop
// their top-level const.
uint32_t PointeeConstOfComponent(uint32_t mask, int depth, bool preserveAliasReferentConst = false);

// Each component folds its pointer DEPTH in via MangleTypeArg ("int*" -> ".p$int", "int**" ->
// ".p$.p$int"); isThin selects the thin/fat prefix. Examples: Lambda<int(int)> ->
// "fatfn$.1$int$int"; function<void(UiTest*)> -> "cfn$.1$void$.p$UiTest";
// Lambda<list<string>()> -> "fatfn$.1$list$string". Both compiler passes MUST call this
// identically. Depths are ints, so a caller that only knows "is a pointer" passes 0 or 1 and gets
// the string this produced before depth existed.
std::string BuildEncodedClosureName(const LLVMBackend* compiler, bool isThin,
    const std::string& ret, int retDepth,
    const std::vector<std::pair<std::string, int>>& params,
    int64_t retConstMask = -1, const std::vector<int64_t>* paramConstMasks = nullptr,
    const std::vector<bool>* paramArrayViews = nullptr,
    const std::vector<bool>* paramAliases = nullptr, bool retAlias = false);



// Peel the reference-kind suffixes off a (possibly substituted) type-arg string: a trailing "[]"
// is a noalias array-view (which is also a pointer repr), a trailing "*" is a plain pointer. They
// never combine ("T[]*" is rejected at the grammar/listener). Suffixes were appended in source
// order, so peel from the right until none remain. Returns the bare base type name in `name`.
// `pointerDepth` is ACCUMULATED (one per star), not a flag: `C**` must stay distinguishable from
// `C*` all the way to MangleTypeArg, which is what makes Box<C**> its own instantiation.
void PeelTypeArgSuffix(std::string& name, int& pointerDepth, bool& arrayView,
    bool* aliasOut = nullptr, int* elemPointerDepth = nullptr);

// Strip a trailing fixed-array suffix ("[3]" / "[3][4]") off a resolved alias string into `dims`
// (outer dimension first). The sizes were constant-folded into digit literals at registration,
// so only `[<digits>]` is accepted. Returns true if any bracket was peeled. Call BEFORE
// PeelAliasPointerStars so "int*[3]" yields dims {3} over a base "int*".
bool PeelAliasArrayDims(std::string& name, std::vector<uint64_t>& dims);

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

// Base-clause identifiers of a class or struct definition. Struct bases are recorded by the scan
// and consumed by the C++-class path when that feature is enabled.
std::vector<CFlatParser::BaseSpecifierContext*> BaseClauseIdentifiers(CFlatParser::ClassDefinitionContext* ctx);
std::vector<CFlatParser::BaseSpecifierContext*> BaseClauseIdentifiers(CFlatParser::StructDefinitionContext* ctx);

template <typename TCtx>
inline bool IsGeneratedCppStruct(TCtx* ctx)
{
    if (dynamic_cast<CFlatParser::StructDefinitionContext*>(ctx) == nullptr) return false;
    const auto annotations = ExtractAnnotations(ctx->annotationList());
    return !BaseClauseIdentifiers(ctx).empty()
        || std::any_of(annotations.begin(), annotations.end(),
                       [](const auto& ann) { return ann.Name == "cpp"; });
}

// The dotted name a base-clause entry spells, without its generic type arguments:
// `IS` -> "IS", `shapes.IS` -> "shapes.IS". Empty when the entry has no identifier.
std::string BaseSpecifierName(CFlatParser::BaseSpecifierContext* spec);

/*
 * Every name a base-clause spelling could be registered under, appended to `out`. The resolver
 * answers one name once the interface is registered; during the forward scan an interface
 * declared later in the same namespace is not yet visible, so the enclosing-namespace
 * candidates are retained as deferred possibilities. The conversion check must treat
 * those candidates as uncertainty: a surplus candidate can set sawSourceImplementor and must not
 * become a false impossibility proof.
 */
void AppendInterfaceNameCandidates(LLVMBackend* compiler, const std::string& namespaceName,
                                          const std::string& spelled, std::vector<std::string>& out);

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
std::string DefinitionSiteText(LLVMBackend* compiler, antlr4::ParserRuleContext* ctx);

// Source text of a parse subtree with runs of whitespace collapsed to one space, so a condition
// reads as written instead of as getText()'s token concatenation. Length-bounded per condition;
// the composed nesting chain is bounded again where the diagnostic assembles it.
std::string CollapsedSourceText(antlr4::ParserRuleContext* ctx);

// The `if const (...)` condition governing this node, if the node is itself an `if const` at file,
// member, or interface scope. Empty when it is any other kind of node.
std::string IfConstConditionText(antlr4::tree::ParseTree* node);

// True for the brace block of one `if const` arm. Both the then arm and a plain `else` arm are
// children of the SAME `if const` node, so the arms can only be told apart by their order here.
// A chained `else if const` is not a block - it is a nested node that carries its own condition.
bool IsIfConstArmBlock(antlr4::tree::ParseTree* node);

// Dotted name of a namespace definition, used to qualify class names recorded from inside one.
std::string NamespaceDefinitionName(CFlatParser::NamespaceDefinitionContext* ctx);

std::string JoinQualified(const std::string& outer, const std::string& inner);

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
void MarkIfConstClassImplsUncertain(LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                           const std::string& namespaceName = {},
                                           const std::vector<std::string>& guardChain = {},
                                           const std::string& qualifier = {},
                                           bool inGenericTemplate = false);

/*
 * MainListener decided to TAKE this arm, so the diagnostic must never claim its classes are absent
 * because of it. A class sitting directly in the arm is live - retract it outright. A class behind
 * a FURTHER nested `if const` only loses this one level: that inner arm gets its own decision, and
 * is retracted or peeled in turn when MainListener reaches it. Callers that descend into a chained
 * `else if const` pass nested=true, since taking the else PATH is one level and nothing more.
 */
void RetractIfConstArmGuardedImpls(LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                          bool nested = false);

/*
 * The walk of this subtree was ABANDONED - LogError threw and an `expect_error` block swallowed it -
 * so no `if const` inside it was ever decided. Nothing here may be claimed untaken: drop every entry
 * outright rather than peel, since a peel would leave a front level the walk never even reached.
 */
void ForgetIfConstGuardedImpls(LLVMBackend* compiler, antlr4::tree::ParseTree* node);

// Interface bodies parse through the named `interfaceMember` rule, so methods and fields are nested
// one level under it. Same macro shape as the aggregate flatteners: each pass gets its own copy so
// the unqualified ResolveInterfaceMembers call binds to that pass's branch-selection policy.
#define CFLAT_DEFINE_INTERFACE_FLATTENERS()                                                                                                                                           \
    std::vector<CFlatParser::InterfaceMethodContext*> InterfaceMethods(CFlatParser::InterfaceDefinitionContext* ctx) { return MemberFilter<CFlatParser::InterfaceMethodContext>(ResolveInterfaceMembers(ctx), [](auto* m){ return m->interfaceMethod(); }); } \
    std::vector<CFlatParser::InterfaceFieldContext*>  InterfaceFields(CFlatParser::InterfaceDefinitionContext* ctx)  { return MemberFilter<CFlatParser::InterfaceFieldContext> (ResolveInterfaceMembers(ctx), [](auto* m){ return m->interfaceField();  }); }

// simd<T,N>: validate the lane count N. It must fold to a power-of-2 integer in [2,64]
// (the explicit SIMD type is the hardware-control escape hatch, so a non-power-of-2 that would
// silently waste a lane is rejected rather than padded). Returns true and sets lanesOut on
// success; otherwise sets errorOut and returns false.
// Decodes a raw `simd<E,N>` spelling (LLVMBackend_Lookup.cpp); false for any other name.
bool DecodeSimdSpelling(const std::string& text, std::string& elemOut, uint64_t& lanesOut);
bool TryParseSimdLaneCount(LLVMBackend* compiler,
                                  CFlatParser::AssignmentExpressionContext* expr,
                                  uint64_t& lanesOut, std::string& errorOut);

/*
 * Record the declarator's pointer depth and array dimensions onto a simd<T,N> declared type.
 * The simd branch breaks out of the specifier loop before the common tail that does this for
 * every other type, so without it `simd<T,N>**` lost its second level and `simd<T,N>[N]`
 * silently lost its dimension (allocating one vector and turning `a[i]` into a LANE read).
 * Shared by BOTH ParseDeclarationSpecifiers copies (ForwardRefScanner and MainListener).
 */
void RecordSimdPointerAndDims(LLVMBackend::DeclTypeAndValue& declType,
                                     CFlatParser::DeclarationSpecifierContext* declSpec);

// Extract function name from FunctionDefinitionContext (handles operator overloads).
std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx);

std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx,
                                   const LLVMBackend* compiler);

// Build a readable one-line signature for a function definition by slicing the source
// from the function's start up to its body, then collapsing whitespace. Used for the
// symbol index of generic-template methods, which would otherwise record only the bare
// name (the monomorphized instance is what carries fully-resolved parameter types).
std::string getFunctionSignatureText(CFlatParser::FunctionDefinitionContext* ctx);

// Extract the leading doc comment block above the given declaration's start token.
// Tier 1 (preferred): contiguous /// or /** ... */ doc comments anchored to the declaration.
// Tier 2 (fallback): contiguous // or /* ... */ plain comments anchored to the declaration.
// "Anchored" means no blank line between the comment block and the declaration, and no
// blank line between adjacent comments in the chain.
// Returns markdown-ready text with comment markers stripped, or empty string on no match.
std::string ExtractLeadingDoc(antlr4::BufferedTokenStream* tokens, antlr4::Token* declStart);

// Check if a function definition has the 'static' storage class.
bool isFunctionStatic(CFlatParser::FunctionDefinitionContext* func);

/*
 * True when a parameter list is callable with ZERO arguments: every parameter carries an
 * '= initializer' default and there is no '...' pack. Such a constructor IS the type's no-arg
 * constructor (its cutoff-0 default-parameter wrapper mangles to the same symbol), so the
 * synthetic one must not also be emitted. A null list means "no parameters at all" and is
 * NOT this case - callers test that separately.
 */
bool AllParametersDefaulted(CFlatParser::ParameterTypeListContext* paramTypeList);

// The source text of `node` with REDUNDANT PARENTHESES peeled off, so a parenthesized whole-value
// source `(p)` / `((p))` records the bare name `p` and matches a parameter name. Only the
// `'(' expression ')'` primaryExpression alternative is peeled - descending through single-child
// nodes never changes the text, so a non-bare source (`v.f`, `v + 1`, `(a, b)`, `f(1)`) is
// returned unchanged and keeps failing every name comparison, as it must.
// When `outWrapperTypes` is supplied, a `(T)x` cast and an `x as T` are peeled too and each
// peeled target type's SPELLING is appended. The peel is unconditional here because the scanner
// cannot resolve the operand's type; the CALLER makes it type-aware by demanding every recorded
// spelling name the type it is matching against (see AllWrapperTypesName).
std::string BareSourceText(antlr4::tree::ParseTree* node,
                                  std::vector<std::string>* outWrapperTypes = nullptr);

// Bare consume-source names that were reached only by peeling a CAST / `as` wrapper, mapped to
// every peeled wrapper type spelling. Kept apart from the plain name set so the wrapped spelling
// is admitted only where the wrapper is proven REDUNDANT.
using WrappedSourceNames = std::unordered_map<std::string, std::vector<std::string>>;

// Split a generic argument list's inner text ("int,list<int>") at TOP-LEVEL commas only, so a
// nested instantiation stays one argument. Whitespace-free input (a parse tree's getText()).
std::vector<std::string> SplitTopLevelTypeArgs(const std::string& args);

// Canonical name for a TYPE SPELLING as written in a cast / `as`, so it can be compared with a
// declaration's already-canonical TypeName. Mirrors the declaration path: namespace resolution,
// then alias folding, and a generic instantiation through the MangleTypeArg funnel. Idempotent on
// an already-canonical name, so BOTH sides of the comparison run through it.
std::string CanonicalWrapperTypeName(const LLVMBackend* compiler, const std::string& spelled);

// Every peeled wrapper names exactly `typeName`, so the whole wrapper chain is redundant and the
// source is a whole-value consume of the bare name. Both sides are canonicalized first, so an
// alias ("UB"), a namespace-relative spelling ("NBox" inside its own namespace) and a generic
// instantiation ("Box<int>") each meet the declared type they name. A decorated target ("UBox*")
// still does not match, which is the conservative direction (no new consume inferred).
bool AllWrapperTypesName(const LLVMBackend* compiler,
                                const std::vector<std::string>& wrapperTypes,
                                const std::string& typeName);

// Record one consume source: the plain set when only parentheses were peeled, the wrapped map
// when a cast / `as` had to be peeled as well.
void RecordConsumeSourceName(antlr4::tree::ParseTree* node,
                                    std::unordered_set<std::string>& out,
                                    WrappedSourceNames* wrapped);

// Detect functions that heap-allocate and return a new owned value. Core string
// concatenation signatures and operator string(i32) allocate; user functions opt
// in via 'move string' / 'move T*' / 'move <interface>'.
bool ComputeReturnsOwned(const LLVMBackend::DeclTypeAndValue& returnType,
                                CFlatParser::FunctionDefinitionContext* func,
                                const std::vector<LLVMBackend::TypeAndValue>& allParams);

// Map a generated parser context type to its rule index, so a node-type test is an integer
// compare instead of the RTTI hierarchy walk dynamic_cast performs on every tree node.
// LOAD-BEARING INVARIANT: this is exact only because CFlat.g4 has NO labeled alternatives
// (`# Label`). ANTLR generates a context SUBCLASS per labeled alternative, and such a subclass
// shares its parent's rule index - dynamic_cast would distinguish the two, AsRuleCtx cannot.
// If a labeled alternative is ever added to the grammar, revisit every AsRuleCtx call site.
template <typename Ctx> struct RuleIndexOf;

template <> struct RuleIndexOf<CFlatParser::LambdaExpressionContext>
{ static constexpr size_t value = CFlatParser::RuleLambdaExpression; };
template <> struct RuleIndexOf<CFlatParser::JumpStatementContext>
{ static constexpr size_t value = CFlatParser::RuleJumpStatement; };
template <> struct RuleIndexOf<CFlatParser::SelectionStatementContext>
{ static constexpr size_t value = CFlatParser::RuleSelectionStatement; };
template <> struct RuleIndexOf<CFlatParser::IterationStatementContext>
{ static constexpr size_t value = CFlatParser::RuleIterationStatement; };
template <> struct RuleIndexOf<CFlatParser::LabeledStatementContext>
{ static constexpr size_t value = CFlatParser::RuleLabeledStatement; };
template <> struct RuleIndexOf<CFlatParser::ConditionalExpressionContext>
{ static constexpr size_t value = CFlatParser::RuleConditionalExpression; };
template <> struct RuleIndexOf<CFlatParser::MoveExpressionContext>
{ static constexpr size_t value = CFlatParser::RuleMoveExpression; };
template <> struct RuleIndexOf<CFlatParser::FunctionDefinitionContext>
{ static constexpr size_t value = CFlatParser::RuleFunctionDefinition; };
template <> struct RuleIndexOf<CFlatParser::AssignmentExpressionContext>
{ static constexpr size_t value = CFlatParser::RuleAssignmentExpression; };
template <> struct RuleIndexOf<CFlatParser::InitDeclaratorContext>
{ static constexpr size_t value = CFlatParser::RuleInitDeclarator; };
template <> struct RuleIndexOf<CFlatParser::CompoundStatementContext>
{ static constexpr size_t value = CFlatParser::RuleCompoundStatement; };
template <> struct RuleIndexOf<CFlatParser::ParameterDeclarationContext>
{ static constexpr size_t value = CFlatParser::RuleParameterDeclaration; };
template <> struct RuleIndexOf<CFlatParser::StructDefinitionContext>
{ static constexpr size_t value = CFlatParser::RuleStructDefinition; };
template <> struct RuleIndexOf<CFlatParser::ClassDefinitionContext>
{ static constexpr size_t value = CFlatParser::RuleClassDefinition; };

template <typename Ctx>
inline Ctx* AsRuleCtx(antlr4::tree::ParseTree* node)
{
    if (node == nullptr || node->getTreeType() != antlr4::tree::ParseTreeType::RULE)
        return nullptr;
    auto* rule = static_cast<antlr4::RuleContext*>(node);
    return rule->getRuleIndex() == RuleIndexOf<Ctx>::value ? static_cast<Ctx*>(rule) : nullptr;
}

// A block that is an arm of a statement-level `if const` splices into the enclosing scope like
// `#if`, so a `using` / typedef in the taken arm binds for the rest of the body (both passes).
inline bool IsStatementIfConstArm(antlr4::tree::ParseTree* compound)
{
    auto* statement = compound != nullptr
        ? dynamic_cast<CFlatParser::StatementContext*>(compound->parent) : nullptr;
    auto* selection = statement != nullptr
        ? dynamic_cast<CFlatParser::SelectionStatementContext*>(statement->parent) : nullptr;
    return selection != nullptr && selection->If() != nullptr && selection->Const() != nullptr;
}

// A foreign C++ by-value parameter is a real object in the callee. Move only its own final
// bare use; a shadowing local is a different slot. Loop and nested-lambda uses stay conservative.
// Unknown or deleted copy constructors also keep the established copy diagnostic.
bool IsLastUseOfForeignCxxParam(
    LLVMBackend* compiler, antlr4::ParserRuleContext* useCtx,
    const LLVMBackend::NamedVariable& source, bool allowMoveOnlyReturn = false);

// Evaluated in the active generic instance; -1 leaves both branches visible.
using IfConstEvaluator = std::function<int(CFlatParser::ExpressionContext*)>;

// Collect the 'return <expr>;' expressions a function body owns itself. A nested lambda's
// returns belong to the lambda, so its subtree is skipped.
void CollectOwnReturnExpressions(antlr4::tree::ParseTree* node,
                                        std::vector<CFlatParser::ExpressionContext*>& out,
                                        const IfConstEvaluator& evalIfConst = {});

void CollectOwnReturnStatements(antlr4::tree::ParseTree* node,
                                       std::vector<CFlatParser::JumpStatementContext*>& out);

bool HasNonAutomaticStorageClass(CFlatParser::DeclarationSpecifiersContext* declSpecs);

bool IsBareReturnIdentifier(const std::string& text);

// Scope-resolved walk for the named-return candidates. Every plain automatic local is a candidate;
// a scope maps each name to its candidate index, or to kNoCandidate for any other binding
// (parameter, static, for/foreach, destructuring, pattern binding).
struct NamedLocalReturnScan
{
    static constexpr size_t kNoCandidate = static_cast<size_t>(-1);
    using Scope = std::vector<std::pair<std::string, size_t>>;
    struct Candidate
    {
        CFlatParser::DirectDeclaratorContext* declarator = nullptr;
        bool named = false;   // some return names this local
        bool killed = false;  // some return inside its live range names something else
    };
    std::vector<Scope> scopes;
    std::vector<Candidate> candidates;

    void Bind(const std::string& name, CFlatParser::DirectDeclaratorContext* local)
    {
        if (scopes.empty()) return;
        size_t index = kNoCandidate;
        if (local != nullptr)
        {
            index = candidates.size();
            candidates.push_back(Candidate{local});
        }
        scopes.back().emplace_back(name, index);
    }
    // Innermost binding wins. kNoCandidate for an unknown name or a non-automatic binding.
    size_t Resolve(const std::string& name) const
    {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
            for (auto entry = scope->rbegin(); entry != scope->rend(); ++entry)
                if (entry->first == name) return entry->second;
        return kNoCandidate;
    }
    // A return naming `named` (kNoCandidate: no local) ends the live range of every other candidate
    // in scope here. Candidates declared later are not in scope yet, so they are unaffected.
    void Return(size_t named)
    {
        for (const auto& scope : scopes)
            for (const auto& entry : scope)
                if (entry.second != kNoCandidate && entry.second != named)
                    candidates[entry.second].killed = true;
        if (named != kNoCandidate) candidates[named].named = true;
    }
};

void NamedLocalReturnVisit(antlr4::tree::ParseTree* node, NamedLocalReturnScan& scan);

// Pre-scan the function's returns before codegen (clang's NRVO rule): a local is built in the hidden
// sret slot when every return statement inside its live range - after its declarator, in its
// scope - names it. Several such locals may share the slot: their live ranges cannot overlap,
// since a return inside both would have to name both. Returns in declaration order.
std::vector<CFlatParser::DirectDeclaratorContext*> ClassifyNamedLocalReturnCandidates(
    CFlatParser::FunctionDefinitionContext* func);

enum class ValueStructReturnKind { NotApplicable, AllBorrowedParam, Mixed };

// Classify the by-value STRUCT returns of a function whose return type carries no ownership
// annotation: does every 'return' hand back a borrowed by-value parameter of the return type,
// or do the paths disagree? The unique-ownership gate lives in the callers - a copyable struct
// has nothing to duplicate, so mixed returns stay legal for it.
ValueStructReturnKind ClassifyValueStructReturns(
    const LLVMBackend* compiler,
    CFlatParser::FunctionDefinitionContext* func,
    const LLVMBackend::DeclTypeAndValue& returnType,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    std::string* outBorrowedParamName);

// Nodes that open a declaration scope: a name declared under one is visible only inside it.
bool IsDeclScopeNode(antlr4::tree::ParseTree* node);

// Implicit-this fields of a struct body: declarators outside every nested scope (method bodies,
// parameter lists, nested aggregates). A field is visible across the whole struct, so no order test.
void CollectFieldNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out);

// Locals a use can see: a declarator whose every enclosing scope also encloses the use, declared
// before it. 'enclosing' holds the use's ancestors. A 'for (T x in ...)' variable is visible in its loop.
void CollectVisibleLocals(antlr4::tree::ParseTree* node,
                                 const std::unordered_set<antlr4::tree::ParseTree*>& enclosing,
                                 size_t usePos, std::unordered_set<std::string>& out);

bool IsIdentifierText(const std::string& s);

// The head of an 'a' or 'a.b.c' path; empty when the text is not such a path.
std::string DottedPathHead(const std::string& text);

// Parse-tree summary for the borrowed-GLOBAL refusal: does every own 'return' name a module
// global at that return? A returned name, or the head of a dotted path (`return Ns.q;`), is a
// global unless a parameter, an implicit-this field, or a local declared in an enclosing block
// before the return binds it there. 'nullptr' is not one. A base struct can hide an inherited
// field, so it yields no summary. Source-only, so definition order cannot matter.
bool ReturnsOnlyModuleGlobals(CFlatParser::FunctionDefinitionContext* func,
                                     const std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst = {});

std::string DirectReturnIdentifier(CFlatParser::ExpressionContext* expr);

// True when a live return resolves to a non-move pointer parameter. Resolve only a bare identifier
// expression (with transparent parens/casts), then let visible locals shadow parameters.
int ReturnedPointerParameterIndex(
    CFlatParser::FunctionDefinitionContext* func,
    CFlatParser::ExpressionContext* expr,
    const std::vector<LLVMBackend::TypeAndValue>& allParams);

bool ReturnsParameterBorrow(CFlatParser::FunctionDefinitionContext* func,
                                   const std::vector<LLVMBackend::TypeAndValue>& allParams,
                                   const IfConstEvaluator& evalIfConst = {});

int ReturnsSingleParameterIndex(
    CFlatParser::FunctionDefinitionContext* func,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    const IfConstEvaluator& evalIfConst = {});

// Set per-instance borrow summaries on the overload with this exact parameter list.
void SetPointerBorrowReturnSummaries(
    std::vector<LLVMBackend::FunctionSymbol>& overloads,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    bool returnsGlobal, bool returnsParameter, int singleParameterIndex);

bool UniquePointerSourceIsOwned(const LLVMBackend::NamedVariable& source);

// Arrow-normalized form of a lock expression's source text: 'p->m' becomes 'p.m'. The mode
// suffix (if any) is left in place - see NormalizeLockText for the canonical, suffix-free form.
std::string ArrowNormalizeLockText(const std::string& text);

// Canonical form of a lock expression's source text. Lock-set membership is compared as
// strings, so 'p->m' and 'p.m' must canonicalize identically: arrow becomes dot, then the
// trailing '.read' / '.write' / '.optimistic' mode suffix is stripped. Used by every lock
// capture site (lock statement, lock clause, positional lock group) in BOTH passes so they agree.
std::string NormalizeLockText(const std::string& text);

// True when a receiver spelling is a plain lock path: identifiers joined by '.', with optional
// '[i]' subscripts. A call result or a computed receiver is not, and keeps the legacy key.
bool IsSimpleLockPath(const std::string& text);

// The canonical lock path an ARGUMENT (or receiver) spelling names: '&o.inner' -> 'o.inner',
// 'p->inner' -> 'p.inner', 'arr[0]' -> 'arr[0]'. Empty when the spelling is not a plain path
// (a call result, an arithmetic index) - the caller then keeps its legacy root key.
std::string LockPathForOperandText(const std::string& operandText);

// The lock-set key a guarded field reached through `receiverPath` resolves to. `this` names the
// receiver's own struct, whose guardian the method-body seeding records bare - see FindHeldGuard.
std::string GuardKeyForPath(const std::string& receiverPath, const std::string& guard);

// Returns "read", "write", "optimistic", or "" (exclusive) for a raw lock expression's text.
std::string LockTextMode(const std::string& text);

// The held-mode a `.read` / `.write` / `.optimistic` suffix confers. "" and "write" are both
// exclusive - the bare form `lock (m)` must never be demoted to Shared.
LockMode LockModeFromSuffix(const std::string& mode);

// The capability interface a mode suffix demands of the lock's type. IOptimisticLockable is a
// CHECKING capability - it has no kCapabilities row because an optimistic read lowers to
// nothing (it is a method call taking a lambda, not an acquire/release pair).
const char* CapabilityForLockMode(const std::string& mode);

using ReturnScanMemo = std::unordered_map<const antlr4::tree::ParseTree*, bool>;

// True if executing `node` can RETURN from the enclosing function - a `return` (value,
// return-block, or `return default`) that is not inside a nested lambda. `break`/`continue`
// exit a loop, not the function, so they do not count (jump->Return() is null for them).
bool SubtreeContainsFunctionReturn(antlr4::tree::ParseTree* node, ReturnScanMemo* memo = nullptr);


bool TryGetKnownIfConstBranch(CFlatParser::SelectionStatementContext* sel,
                                     const IfConstEvaluator& evalIfConst,
                                     antlr4::tree::ParseTree*& liveBranch);

// Collect names UNCONDITIONALLY moved by the function body: a top-level `move <bare-param>`
// not nested inside any runtime conditional/loop/labeled construct, ternary, or lambda, AND not
// preceded by a statement that can return early. Drives the owning-value move-sink inference;
// a conditional, nested, or possibly-skipped move must NOT mark a param a sink. An `if const`
// whose condition is a compile-time constant for this instantiation is NOT runtime-conditional:
// its live branch is straight-line code, so `evalIfConst` lets us descend into it.
void CollectUnconditionalMovedNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out,
                                           const IfConstEvaluator& evalIfConst = {},
                                           ReturnScanMemo* returnScanMemo = nullptr);

// Collect the bare source name of each POSITIONAL element of a brace list (`{ p, q }`). The
// fixed-array lowering consumes an owning element source exactly as a slot store does
// (ConsumeOwningBraceElementSource), so those names are consumed too. Named (`f = v`) and
// dictionary (`k: v`) forms are skipped - neither reaches that arm.
void CollectPositionalBraceElementNames(CFlatParser::InitializerListContext* list,
                                               std::unordered_set<std::string>& out,
                                               WrappedSourceNames* wrapped = nullptr);

// True when `name` at `use` names a LOCAL declared earlier in an enclosing block (or a `for`
// header) of the same function / lambda body, not the parameter of that name. The consume scans
// are name-based; the write / return triggers must not claim a shadowing local's use.
bool NameShadowedByEnclosingLocal(antlr4::tree::ParseTree* use, const std::string& name,
        const CFlatParser::LambdaExpressionContext* functionLambda = nullptr,
        const antlr4::tree::ParseTree* stopAt = nullptr);

// Collect bare source names CONSUMED by a plain store ANYWHERE in the body (at-least-one-path):
// the RHS of a plain `=` assignment (`X = value`, incl. a slot store `_data[i] = value`), a decl
// initializer (`T x = value`, incl. its brace-list form `T[N] d = { p };` / `T[N] d { p };`), a
// `move <name>` (conditional too), a `return <name>`, or the whole-name DESTINATION of `=` / `+=`. Unlike
// CollectUnconditionalMovedNames this DESCENDS into runtime conditionals/loops - 8a's total
// scope-exit drop makes a not-taken path sound. Lambdas / nested functions are a different scope
// and are not descended into. The caller intersects with the param list; a non-bare RHS (`v.f`,
// `v + 1`) never equals a param name, so it is naturally excluded (only a whole-value move counts).
void CollectConsumedStoreNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out,
                                      WrappedSourceNames* wrapped = nullptr,
                                      std::unordered_set<std::string>* returned = nullptr,
                                      WrappedSourceNames* returnedWrapped = nullptr,
                                      std::unordered_set<std::string>* written = nullptr,
                                      const CFlatParser::LambdaExpressionContext* functionLambda = nullptr,
                                      std::unordered_set<std::string>* explicitMoves = nullptr,
                                      const IfConstEvaluator& evalIfConst = {});

// Collect the bare names a body stores INTO as a whole (`w = v`, `w += k`). Lambdas and nested
// functions are a different scope. Drives the copy-on-entry of a written copyable parameter.
void CollectWholeNameStoreDestinations(antlr4::tree::ParseTree* node,
                                              std::unordered_set<std::string>& out,
                                              bool skipLambdas = false,
                                              const CFlatParser::LambdaExpressionContext* functionLambda = nullptr);

// First character of a rule's source text without building the subtree string (getText()
// concatenates token texts, so it starts with the first token's text).
char FirstSourceChar(antlr4::ParserRuleContext* ctx);

void CollectAddressAliasWholeNameStores(
        antlr4::tree::ParseTree* body,
        const std::vector<LLVMBackend::TypeAndValue>& params,
        std::unordered_set<std::string>& writtenNames,
        bool addressMayWrite = false,
        const CFlatParser::LambdaExpressionContext* functionLambda = nullptr);

// A parameter shape that can carry an owning VALUE (a value struct or `string`). Excludes
// borrows (pointers, interface values, array-views), function pointers, arrays, and params
// already carrying a move/alias/unique qualifier. The definitive "type owns a resource" test
// runs at the call site on the concrete (monomorphized) type, so this only rejects shapes
// that can never be an owning sink.
bool ParamIsOwningSinkEligible(const LLVMBackend::TypeAndValue& p);

std::string SimpleIdentifierExpressionName(antlr4::tree::ParseTree* expression);

bool NameIsImplicitThisField(antlr4::tree::ParseTree* bodyRoot,
        const std::string& name);

bool ViewFieldAliasProvesParam(antlr4::tree::ParseTree* bodyRoot,
        antlr4::tree::ParseTree* use, const std::string& aliasName,
        const LLVMBackend::TypeAndValue& param,
        const CFlatParser::LambdaExpressionContext* functionLambda);

// True when the `move` is the whole initializer of a LOCAL declaration (`T[] r = move p->v;`): the one
// destination where moving a borrowed view copies it. Sinks and owning slots keep the refusal.
bool MoveIsLocalDeclInitializer(antlr4::tree::ParseTree* move);

std::string ViewFieldMovePath(CFlatParser::UnaryExpressionContext* source,
        antlr4::tree::ParseTree* bodyRoot,
        const LLVMBackend::TypeAndValue& param,
        const std::vector<LLVMBackend::TypeAndValue>& allParams,
        const CFlatParser::LambdaExpressionContext* functionLambda);

void InferViewFieldMovePaths(antlr4::tree::ParseTree* node,
        antlr4::tree::ParseTree* bodyRoot,
        const LLVMBackend* compiler,
        std::vector<LLVMBackend::TypeAndValue>& allParams,
        const IfConstEvaluator& evalIfConst = {},
        const CFlatParser::LambdaExpressionContext* functionLambda = nullptr);

// Owning-value move-sink inference: a plain by-value parameter is a synthesized sink (IsOwningSink)
// so the caller's owning source is nulled at the call site. An unconditional top-level move consumes
// any owner; stores and conditional moves remain structural until concrete copyability is known.
// This also runs on generic-class instantiations against the monomorphized param list.
// Body-taking core. A lambda literal has no FunctionDefinitionContext but is a function all the
// same, so its parameter list runs the identical inference against its own body.
void ApplyOwningSinkInferenceToBody(const LLVMBackend* compiler,
                                     antlr4::tree::ParseTree* body,
                                     std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst = {},
                                     const CFlatParser::LambdaExpressionContext* functionLambda = nullptr);

// A declared `Lambda<...>` / `function<...>` spelling cannot express an INFERRED owning sink, so a
// funcptr destination adopts the flags of the lambda literal (or funcptr value) it is bound to.
// Union, never clear: over-approximating a sink leaks at worst, missing one double-frees, and a
// destination rebound to a differently-inferred value must keep the stronger claim.
void AdoptInferredParamSinks(LLVMBackend::TypeAndValue& dest,
                                    const std::vector<LLVMBackend::TypeAndValue::FuncPtrParam>& src);

void ApplyOwningSinkInference(const LLVMBackend* compiler,
                                     CFlatParser::FunctionDefinitionContext* func,
                                     std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst = {});

// True when the function declares a RETURN TYPE, which is what separates an ordinary member
// from a constructor. Not the same as having declarationSpecifiers: per CFlat.g4:783 a ctor
// may carry 'inline'/'static'/'const'/'extern'/'stdcall' - those are functionSpecifier,
// storageClassSpecifier and typeQualifier, never typeSpecifier.
// KNOWN IMPRECISION, message wording only: 'move' IS a typeSpecifier (CFlat.g4:323), and
// 'unique'/'alias'/'bond' are not grammar keywords at all - they parse as genericIdentifier,
// also a typeSpecifier. So a ctor carrying one of those four reads as "member" in the
// duplicate diagnostic. The duplicate is still caught; only the noun is wrong.
/*
 * Const receivers (iteration 1): the structural facts a native method body states about its
 * `this` - writes, member calls, receiver-rooted values handed on. Names resolve in codegen
 * order (parameter / enclosing local first, then a field when the struct declares one, which
 * IsReadOnlyMethod decides against the declared types); expression TEXT never decides anything.
 */
class ReceiverScan
{
public:
    using Path = LLVMBackend::ReceiverPath;
    using Facts = LLVMBackend::ReceiverScanFacts;

    struct Value
    {
        bool Ok = false;
        Path P;
        bool AddressOf = false;     // the value is the ADDRESS of the path (`&f`, `this`)
        bool WholeLocal = false;    // a bare local bound to receiver storage
        bool Borrow = false;        // ... that is a range-for element borrow
    };

    ReceiverScan(const std::vector<LLVMBackend::TypeAndValue>& allParams, const IfConstEvaluator& eval)
        : eval_(eval)
    {
        facts_.StructName = allParams.front().TypeName;
        for (size_t i = 1; i < allParams.size(); ++i)
            if (!allParams[i].VariableName.empty()) params_.insert(allParams[i].VariableName);
    }

    Facts Run(antlr4::tree::ParseTree* body)
    {
        Walk(body);
        // A parameter the body re-seats may hold anything by the time it is returned.
        for (size_t i = 0; i < facts_.Returns.size(); ++i)
            if (facts_.Returns[i].Kind == 1 && !returnedParams_[i].empty()
                && assigned_.count(returnedParams_[i]) != 0)
                facts_.Returns[i].Kind = 2;
        return std::move(facts_);
    }

private:
    // Copy: a by-value binding; IsReadOnlyMethod keeps it rooted only for a pointer or view type.
    // `Decl` is the declaring node, so a block exit can tell its own locals from outer ones.
    struct AliasEntry { Path P; bool AddressOf = false; bool Borrow = false; bool Copy = false;
                        const void* Decl = nullptr; };

    static bool Is(antlr4::tree::ParseTree* node, size_t rule)
    {
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node);
        return ctx != nullptr && ctx->getRuleIndex() == rule;
    }
    static antlr4::tree::TerminalNode* Terminal(antlr4::tree::ParseTree* node)
    {
        return node != nullptr && node->getTreeType() == antlr4::tree::ParseTreeType::TERMINAL
            ? dynamic_cast<antlr4::tree::TerminalNode*>(node) : nullptr;
    }

    static void Step(Value& v, const std::string& step)
    {
        if (v.AddressOf)
        {
            // Through the address of an object: `(&o)->f` is `o.f`, `*&o` and `(&o)[0]` are `o`.
            v.AddressOf = false;
            if (step.starts_with("->")) v.P.Steps.push_back("." + step.substr(2));
            else if (step.starts_with(".")) v.P.Steps.push_back(step);
        }
        else
            v.P.Steps.push_back(step);
        v.WholeLocal = false;
        v.Borrow = false;
    }

    Value Name(antlr4::ParserRuleContext* use, const std::string& name) const
    {
        Value v;
        if (name.empty()) return v;
        if (name == "this")
        {
            v.Ok = true;
            v.P.IsThis = true;
            v.AddressOf = true;
            return v;
        }
        if (params_.count(name) != 0 || NameShadowedByEnclosingLocal(use, name))
        {
            auto it = aliases_.find(name);
            if (it == aliases_.end()) return v;
            v.Ok = true;
            v.P = it->second.P;
            if (it->second.Copy) v.P.Steps.push_back("=");
            v.AddressOf = it->second.AddressOf;
            v.WholeLocal = true;
            v.Borrow = it->second.Borrow;
            return v;
        }
        v.Ok = true;
        v.P.Root = name;
        return v;
    }

    // The receiver-rooted value an expression denotes, peeling only shape-preserving wrappers.
    Value Of(antlr4::tree::ParseTree* node)
    {
        Value none;
        while (node != nullptr)
        {
            auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (ctx == nullptr) return none;
            const size_t rule = ctx->getRuleIndex();
            if (rule == CFlatParser::RulePostfixExpression) return Postfix(ctx, nullptr);
            if (rule == CFlatParser::RuleUnaryExpression && ctx->children.size() == 2)
            {
                auto* op = dynamic_cast<CFlatParser::UnaryOperatorContext*>(ctx->children[0]);
                if (op == nullptr) return none;
                Value inner = Of(ctx->children[1]);
                if (!inner.Ok) return none;
                const std::string text = op->getText();
                if (text == "&")
                {
                    if (inner.AddressOf) return none;
                    inner.AddressOf = true;
                    inner.WholeLocal = false;
                    return inner;
                }
                if (text == "*") { Step(inner, "*"); return inner; }
                return none;
            }
            if (rule == CFlatParser::RuleAdditiveExpression && ctx->children.size() == 3)
            {
                // `f + k` on a pointer field is still a pointer into what the field reaches.
                Value inner = Of(ctx->children[0]);
                inner.WholeLocal = false;
                inner.Borrow = false;
                return inner;
            }
            if (rule == CFlatParser::RulePrimaryExpression)
            {
                auto* primary = static_cast<CFlatParser::PrimaryExpressionContext*>(ctx);
                if (primary->expression() != nullptr && ctx->children.size() == 3)
                {
                    node = primary->expression();
                    continue;
                }
                return none;
            }
            if (rule == CFlatParser::RuleCastExpression && ctx->children.size() != 1) return none;
            if (ctx->children.size() != 1) return none;
            node = ctx->children[0];
        }
        return none;
    }

    /*
     * Walk a postfix chain. With `record` set, calls and `++` / `--` in it are recorded; the
     * returned value is the chain's path when it denotes receiver storage.
     */
    Value Postfix(antlr4::ParserRuleContext* ctx, Facts* record, size_t limit = SIZE_MAX)
    {
        auto* postfix = static_cast<CFlatParser::PostfixExpressionContext*>(ctx);
        Value v;
        auto* primary = postfix->primaryExpression();
        bool bareName = false;
        std::string calleeName;
        if (primary != nullptr && primary->genericIdentifier() != nullptr
            && primary->genericIdentifier()->genericTypeParameters() == nullptr)
        {
            calleeName = primary->genericIdentifier()->Identifier()->getText();
            bareName = true;
            v = Name(primary, calleeName);
        }
        else if (primary != nullptr && primary->expression() != nullptr && primary->children.size() == 3)
            v = Of(primary->expression());
        const auto& children = postfix->children;
        std::string member;      // the member named by the step just taken
        Value beforeMember;      // the receiver that member was taken on
        for (size_t i = 1; i < children.size() && i < limit; ++i)
        {
            auto* term = Terminal(children[i]);
            if (term == nullptr) continue;
            const std::string tok = term->getText();
            if (tok == "[")
            {
                if (v.Ok && record != nullptr) record->Calls.push_back({ v.P, "operator[]", 1, false, term });
                if (v.Ok) Step(v, "[]");
                member.clear();
                bareName = false;
            }
            else if (tok == "." || tok == "->" || tok == "?.")
            {
                if (i + 1 < children.size() && Terminal(children[i + 1]) != nullptr)
                {
                    // `.~()`: an explicit destructor call.
                    v = Value{};
                    member.clear();
                    bareName = false;
                    continue;
                }
                member = i + 1 < children.size() ? children[i + 1]->getText() : std::string();
                beforeMember = v;
                if (v.Ok) Step(v, (tok == "->" ? "->" : ".") + member);
                bareName = false;
                ++i;
            }
            else if (tok == "(")
            {
                CFlatParser::ArgumentExpressionListContext* args = nullptr;
                if (i + 1 < children.size())
                    args = dynamic_cast<CFlatParser::ArgumentExpressionListContext*>(children[i + 1]);
                if (record != nullptr) RecordCall(primary, bareName, calleeName, member, beforeMember, args, term);
                // A call on the receiver: its result may be an alias / pointer into it.
                v = Value{};
                Path receiverPath;
                bool implicit = false;
                if (CallReceiver(primary, bareName, calleeName, member, beforeMember, receiverPath, implicit))
                {
                    facts_.Results.push_back({ receiverPath, implicit ? calleeName : member,
                                               args != nullptr ? args->argumentNamedExpression().size() : 0,
                                               implicit, term });
                    v.Ok = true;
                    v.P.Result = static_cast<int>(facts_.Results.size() - 1);
                }
                member.clear();
                bareName = false;
            }
            else if (tok == "++" || tok == "--")
            {
                if (record != nullptr) RecordWrite(v, ctx);
                v = Value{};
                member.clear();
                bareName = false;
            }
        }
        return v;
    }

    // The postfix expression a single-child chain reduces to.
    static antlr4::ParserRuleContext* PostfixOf(antlr4::tree::ParseTree* node)
    {
        while (auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node))
        {
            if (ctx->getRuleIndex() == CFlatParser::RulePostfixExpression) return ctx;
            if (ctx->children.size() != 1) return nullptr;
            node = ctx->children[0];
        }
        return nullptr;
    }

    void RecordWrite(const Value& v, antlr4::ParserRuleContext* at)
    {
        if (!v.Ok || (v.WholeLocal && !v.Borrow)) return;
        // Writing `&x` itself is not a write; writing through it is (Step already folded it).
        if (v.AddressOf) return;
        facts_.Writes.push_back({ v.P });
    }

    void RecordEscape(const Value& v, int kind, const std::string& callee, const Path& calleeReceiver,
                      size_t argIndex, size_t arity, bool aliasBind, antlr4::ParserRuleContext* at,
                      const void* site = nullptr)
    {
        if (!v.Ok) return;
        LLVMBackend::ReceiverScanFacts::Escape e;
        e.Source = v.P;
        e.AddressOf = v.AddressOf;
        e.AliasBind = aliasBind;
        e.Kind = kind;
        e.Callee = callee;
        e.CalleeReceiver = calleeReceiver;
        e.ArgIndex = argIndex;
        e.Arity = arity;
        e.Site = site;
        facts_.Escapes.push_back(std::move(e));
    }

    /*
     * The object a call is made on: an implicit-this member (or a free function, decided against
     * the declarations) for a bare name that is no local / parameter, else the receiver-rooted
     * object of a member call. A local / parameter callee is a function value: none.
     */
    bool CallReceiver(antlr4::ParserRuleContext* use, bool bareName, const std::string& calleeName,
                      const std::string& member, const Value& receiver, Path& out, bool& implicit) const
    {
        implicit = false;
        if (bareName && !calleeName.empty())
        {
            if (params_.count(calleeName) != 0 || NameShadowedByEnclosingLocal(use, calleeName)) return false;
            out = Path{};
            out.IsThis = true;
            implicit = true;
            return true;
        }
        if (member.empty() || !receiver.Ok) return false;
        Value object = receiver;
        if (object.AddressOf) object.AddressOf = false;
        else if (object.WholeLocal && !object.Borrow)
            object.P.Steps.push_back("*"); // a local pointer bound to receiver storage: its pointee
        out = object.P;
        return true;
    }

    bool IsOutwardAssignmentTarget(CFlatParser::AssignmentExpressionContext* asn) const
    {
        auto* pfx = asn->unaryExpression() != nullptr ? asn->unaryExpression()->postfixExpression() : nullptr;
        auto* primary = pfx != nullptr ? pfx->primaryExpression() : nullptr;
        if (primary == nullptr || primary->genericIdentifier() == nullptr) return false;
        const std::string name = primary->genericIdentifier()->Identifier()->getText();
        return params_.count(name) == 0 && !NameShadowedByEnclosingLocal(asn, name);
    }

    void RecordCall(antlr4::ParserRuleContext* use, bool bareName, const std::string& calleeName,
                    const std::string& member, const Value& receiver,
                    CFlatParser::ArgumentExpressionListContext* args, const void* site)
    {
        std::vector<CFlatParser::ArgumentNamedExpressionContext*> list;
        if (args != nullptr) list = args->argumentNamedExpression();
        const size_t arity = list.size();
        int kind = 3;
        std::string callee;
        Path calleeReceiver;
        bool implicit = false;
        if (CallReceiver(use, bareName, calleeName, member, receiver, calleeReceiver, implicit))
        {
            callee = implicit ? calleeName : member;
            facts_.Calls.push_back({ calleeReceiver, callee, arity, implicit, site });
            kind = implicit ? 1 : 2;
        }
        else
            calleeReceiver = Path{};
        for (size_t a = 0; a < list.size(); ++a)
            if (auto* arg = list[a]->assignmentExpression())
                RecordEscape(Of(arg), kind, callee, calleeReceiver, a, arity, false, arg, site);
    }

    static bool SpecifiersSay(CFlatParser::DeclarationSpecifiersContext* specs, bool& isConst, bool& isAuto,
                              bool& isAlias)
    {
        isConst = isAuto = isAlias = false;
        if (specs == nullptr) return false;
        for (auto* spec : specs->declarationSpecifier())
        {
            if (spec->typeQualifier() != nullptr) isConst = true;
            if (auto* typeSpec = spec->typeSpecifier())
            {
                // `alias` is a soft keyword, matched like both ParseDeclarationSpecifiers do.
                if (typeSpec->getText() == "alias") isAlias = true;
                if (typeSpec->getStart() != nullptr && typeSpec->getStart()->getType() == CFlatParser::Auto
                    && typeSpec->children.size() == 1)
                    isAuto = true;
            }
        }
        return true;
    }

    void Declare(CFlatParser::DeclarationSpecifiersContext* specs, CFlatParser::InitDeclaratorListContext* list)
    {
        if (list == nullptr) return;
        bool isConst = false, isAuto = false, isAlias = false;
        SpecifiersSay(specs, isConst, isAuto, isAlias);
        for (auto* init : list->initDeclarator())
        {
            auto* declarator = init->declarator();
            auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
            const std::string name = direct != nullptr && direct->Identifier() != nullptr
                ? direct->Identifier()->getText() : std::string();
            aliases_.erase(name);
            auto* iz = init->initializer();
            std::vector<antlr4::ParserRuleContext*> elements;
            if (iz != nullptr && iz->assignmentExpression() != nullptr)
            {
                Value v = Of(iz->assignmentExpression());
                if (!v.Ok) continue;
                if (isAlias && !isConst)
                    RecordEscape(v, 0, {}, {}, 0, 0, true, iz);
                else if (isAlias || isConst || isAuto)
                {
                    if (!name.empty())
                        aliases_[name] = AliasEntry{ v.P, v.AddressOf, isAlias || v.Borrow,
                                                     !v.AddressOf && !isAlias && !v.Borrow, init };
                }
                else
                    RecordEscape(v, 0, {}, {}, 0, 0, false, iz);
                continue;
            }
            CFlatParser::InitializerListContext* braces = iz != nullptr ? iz->initializerList()
                                                                         : init->initializerList();
            if (braces != nullptr)
                for (auto* element : braces->fieldInit())
                    for (auto* value : element->assignmentExpression())
                        RecordEscape(Of(value), 0, {}, {}, 0, 0, false, value);
        }
    }

    /*
     * A block (or loop) is a lexical scope for local aliases: on exit, a local it declared is
     * dropped and an outer one it shadowed comes back. An outer local re-seated inside keeps
     * its new value (same declaring node).
     */
    void Walk(antlr4::tree::ParseTree* node)
    {
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (ctx == nullptr) return;
        const size_t rule = ctx->getRuleIndex();
        if (rule != CFlatParser::RuleCompoundStatement && rule != CFlatParser::RuleIterationStatement)
        {
            WalkNode(ctx, rule);
            return;
        }
        auto outer = aliases_;
        WalkNode(ctx, rule);
        for (auto& [name, entry] : outer)
            if (auto it = aliases_.find(name); it != aliases_.end() && it->second.Decl == entry.Decl)
                entry = it->second;
        aliases_ = std::move(outer);
    }

    void WalkNode(antlr4::ParserRuleContext* ctx, const size_t rule)
    {
        if (rule == CFlatParser::RuleFunctionDefinition && depth_ > 0) return;
        if (rule == CFlatParser::RuleSelectionStatement)
        {
            auto* sel = static_cast<CFlatParser::SelectionStatementContext*>(ctx);
            if (sel->If() != nullptr && sel->Const() != nullptr)
            {
                // `if const`: only the live arm of this instantiation; undecided scans both.
                auto arms = sel->statement();
                const int taken = eval_ ? eval_(sel->expression()) : -1;
                ++depth_;
                if (taken != 0 && !arms.empty()) Walk(arms[0]);
                if (taken != 1 && arms.size() > 1) Walk(arms[1]);
                --depth_;
                return;
            }
        }
        if (rule == CFlatParser::RuleDeclaration)
        {
            auto* decl = static_cast<CFlatParser::DeclarationContext*>(ctx);
            ++depth_;
            for (auto* child : ctx->children) Walk(child);
            --depth_;
            Declare(decl->declarationSpecifiers(), decl->initDeclaratorList());
            return;
        }
        if (rule == CFlatParser::RuleForDeclaration)
        {
            auto* decl = static_cast<CFlatParser::ForDeclarationContext*>(ctx);
            ++depth_;
            for (auto* child : ctx->children) Walk(child);
            --depth_;
            Declare(decl->declarationSpecifiers(), decl->initDeclaratorList());
            return;
        }
        if (rule == CFlatParser::RuleIterationStatement)
        {
            auto* loop = static_cast<CFlatParser::IterationStatementContext*>(ctx);
            if (loop->In() != nullptr && loop->Identifier() != nullptr && loop->expression() != nullptr)
            {
                Value iterable = Of(loop->expression());
                const std::string name = loop->Identifier()->getText();
                aliases_.erase(name);
                if (iterable.Ok)
                {
                    Value object = iterable;
                    if (object.AddressOf) object.AddressOf = false;
                    facts_.Calls.push_back({ object.P, "count", 0, false });
                    facts_.Calls.push_back({ object.P, "get", 1, false });
                    Path element = object.P;
                    element.Steps.push_back("[]");
                    aliases_[name] = AliasEntry{ element, false, true, false, loop };
                }
            }
        }
        if (rule == CFlatParser::RuleAssignmentExpression)
        {
            auto* asn = static_cast<CFlatParser::AssignmentExpressionContext*>(ctx);
            if (asn->assignmentOperator() != nullptr && asn->unaryExpression() != nullptr)
            {
                // A parameter re-seated here (a same-named local declared around it is not it).
                if (auto* pfx = asn->unaryExpression()->postfixExpression();
                    pfx != nullptr && pfx->children.size() == 1 && pfx->primaryExpression() != nullptr
                    && pfx->primaryExpression()->genericIdentifier() != nullptr)
                {
                    const std::string name = pfx->primaryExpression()->genericIdentifier()->Identifier()->getText();
                    if (params_.count(name) != 0 && !NameShadowedByEnclosingLocal(asn, name)) assigned_.insert(name);
                }
                Value target = Of(asn->unaryExpression());
                RecordWrite(target, asn);
                if (asn->assignmentOperator()->getText() == "=" && asn->assignmentExpression() != nullptr)
                {
                    Value source = Of(asn->assignmentExpression());
                    if (target.Ok && target.WholeLocal && !target.Borrow && source.Ok)
                    {
                        // Re-seating a local that already views receiver storage keeps it a view.
                        auto* name = asn->unaryExpression()->postfixExpression();
                        if (name != nullptr && name->primaryExpression() != nullptr
                            && name->primaryExpression()->genericIdentifier() != nullptr)
                        {
                            auto it = aliases_.find(name->primaryExpression()->genericIdentifier()->Identifier()->getText());
                            if (it != aliases_.end())
                                it->second = AliasEntry{ source.P, source.AddressOf, it->second.Borrow,
                                                         !source.AddressOf && !it->second.Borrow,
                                                         it->second.Decl };
                        }
                    }
                    else if (!target.Ok || (target.WholeLocal && !target.Borrow))
                        RecordEscape(source, 0, {}, {}, 0, 0, false, asn);
                    if (source.Ok && IsOutwardAssignmentTarget(asn))
                        facts_.OutwardViewStores.push_back({ source.P, source.AddressOf });
                }
            }
        }
        if (rule == CFlatParser::RuleLockStatement)
        {
            // `lock(x) { }` calls x's acquire / release pair for the lock mode, as the lowering does.
            for (auto* expr : static_cast<CFlatParser::LockStatementContext*>(ctx)->lockClause()
                     ->lockArgList()->expression())
            {
                const std::string mode = LockTextMode(expr->getText());
                Value v;
                if (mode.empty()) v = Of(expr);
                else if (auto* pfx = PostfixOf(expr); pfx != nullptr && pfx->children.size() >= 3)
                    v = Postfix(pfx, nullptr, pfx->children.size() - 2);
                Path object;
                bool implicit = false;
                // Any member name selects CallReceiver's member-call form (object = v's path).
                if (!CallReceiver(ctx, false, {}, "lock", v, object, implicit)) continue;
                const std::string wantMode = mode == "read" || mode == "optimistic" ? mode : std::string();
                for (const auto& cap : kCapabilities)
                    if (wantMode == cap.Mode)
                    {
                        facts_.Calls.push_back({ object, cap.Acquire, 0, false });
                        facts_.Calls.push_back({ object, cap.Release, 0, false });
                    }
            }
        }
        if (rule == CFlatParser::RuleJumpStatement)
        {
            auto* jump = static_cast<CFlatParser::JumpStatementContext*>(ctx);
            if (jump->Return() != nullptr && jump->expression() != nullptr) RecordReturn(jump->expression());
            else if (jump->Return() != nullptr)
            {
                // `return default;` is a null pointer; a block return is not followed (unknown).
                LLVMBackend::ReceiverScanFacts::Return r;
                if (jump->Default() != nullptr) r.Kind = 1;
                facts_.Returns.push_back(r);
                returnedParams_.push_back({});
            }
        }
        if (rule == CFlatParser::RuleMoveExpression)
            RecordWrite(Of(static_cast<CFlatParser::MoveExpressionContext*>(ctx)->unaryExpression()), ctx);
        if (rule == CFlatParser::RuleDeleteExpression)
        {
            // `delete p` releases what p reaches.
            Value v = Of(static_cast<CFlatParser::DeleteExpressionContext*>(ctx)->expression());
            if (v.Ok) { v.WholeLocal = false; Step(v, "*"); RecordWrite(v, ctx); }
        }
        if (rule == CFlatParser::RulePostfixExpression)
            Postfix(ctx, &facts_);
        if (ctx->children.size() >= 3
            && (rule == CFlatParser::RuleEqualityExpression || rule == CFlatParser::RuleRelationalExpression
                || rule == CFlatParser::RuleAdditiveExpression || rule == CFlatParser::RuleMultiplicativeExpression
                || rule == CFlatParser::RuleShiftExpression || rule == CFlatParser::RuleAndExpression
                || rule == CFlatParser::RuleExclusiveOrExpression || rule == CFlatParser::RuleInclusiveOrExpression))
        {
            // A member operator on a receiver-rooted left operand is a member call on it.
            if (auto* op = Terminal(ctx->children[1]))
            {
                Value lhs = Of(ctx->children[0]);
                if (lhs.Ok && !lhs.AddressOf)
                    facts_.Calls.push_back({ lhs.P, "operator" + op->getText(), 1, false, ctx });
            }
        }
        ++depth_;
        for (auto* child : ctx->children) Walk(child);
        --depth_;
    }

    // Classify one returned expression (Facts::Return); each arm of `?:` / `??` on its own.
    void RecordReturn(antlr4::tree::ParseTree* expr)
    {
        antlr4::tree::ParseTree* node = expr;
        while (auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node))
        {
            if (ctx->getRuleIndex() == CFlatParser::RuleConditionalExpression && ctx->children.size() == 5)
            {
                RecordReturn(ctx->children[2]);
                RecordReturn(ctx->children[4]);
                return;
            }
            if (ctx->getRuleIndex() == CFlatParser::RuleConditionalExpression && ctx->children.size() == 3)
            {
                RecordReturn(ctx->children[0]);
                RecordReturn(ctx->children[2]);
                return;
            }
            if (ctx->getRuleIndex() == CFlatParser::RulePrimaryExpression && ctx->children.size() == 3
                && static_cast<CFlatParser::PrimaryExpressionContext*>(ctx)->expression() != nullptr)
            {
                RecordReturn(static_cast<CFlatParser::PrimaryExpressionContext*>(ctx)->expression());
                return;
            }
            if (ctx->children.size() != 1) break;
            node = ctx->children[0];
        }
        LLVMBackend::ReceiverScanFacts::Return r;
        std::string param;
        Value v = Of(expr);
        if (v.Ok)
        {
            r.Kind = 0;
            r.Source = v.P;
            r.AddressOf = v.AddressOf;
        }
        else
        {
            node = expr;
            while (auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node))
            {
                if (ctx->getRuleIndex() == CFlatParser::RuleNewExpression) { r.Kind = 1; break; }
                if (ctx->getRuleIndex() == CFlatParser::RulePrimaryExpression)
                {
                    auto* primary = static_cast<CFlatParser::PrimaryExpressionContext*>(ctx);
                    if (ctx->children.size() == 1 && Terminal(ctx->children[0]) != nullptr)
                        r.Kind = 1; // a literal
                    else if (primary->genericIdentifier() != nullptr && ctx->children.size() == 1
                             && primary->genericIdentifier()->genericTypeParameters() == nullptr)
                    {
                        // The parameter itself, not a local of the same name declared around it.
                        const std::string name = primary->genericIdentifier()->Identifier()->getText();
                        if (params_.count(name) != 0 && aliases_.count(name) == 0
                            && !NameShadowedByEnclosingLocal(primary, name))
                        {
                            r.Kind = 1;
                            param = name;
                        }
                    }
                    break;
                }
                if (ctx->children.size() != 1) break;
                node = ctx->children[0];
            }
        }
        facts_.Returns.push_back(std::move(r));
        returnedParams_.push_back(param);
    }

    const IfConstEvaluator& eval_;
    Facts facts_;
    std::vector<std::string> returnedParams_;
    std::unordered_set<std::string> assigned_;
    std::unordered_set<std::string> params_;
    std::unordered_map<std::string, AliasEntry> aliases_;
    int depth_ = 0;
};

// Record the receiver facts of a native method (`allParams[0]` is its synthesized `this`).
// Returns the method's unique name, empty when it is not a native method with a body.
std::string RecordReceiverScan(LLVMBackend* compiler, CFlatParser::FunctionDefinitionContext* func,
                                      const std::string& name, const LLVMBackend::DeclTypeAndValue& returnType,
                                      const std::vector<LLVMBackend::TypeAndValue>& allParams, bool varargs,
                                      const IfConstEvaluator& eval = {});

bool FunctionDeclaresReturnType(CFlatParser::FunctionDefinitionContext* func);

// Render a parameter type list for a diagnostic, e.g. "int, string*". Types only:
// parameter names are not part of a function signature.
std::string DescribeParameterTypes(const std::vector<LLVMBackend::TypeAndValue>& params);

// ForwardRefScanner performs a lightweight pre-pass over the AST to register
// all function signatures and struct type shells before the main code-gen walk.
// This allows functions and types to be used before their definition in source.
class ForwardRefScanner
{
private:
    struct ViewFieldMoveInferenceEntry
    {
        CFlatParser::FunctionDefinitionContext* Definition = nullptr;
        std::string SourceName;
        std::string UniqueName;
        std::vector<LLVMBackend::TypeAndValue> Parameters;
    };
    struct DeferredFunctionDefinition
    {
        CFlatParser::FunctionDefinitionContext* Function = nullptr;
        std::string StructName;
        std::string NamespaceName;
        std::string CurrentNamespace;
        std::vector<std::string> RequiredLocks;
    };
    LLVMBackend* compilerLLVM;
    antlr4::BufferedTokenStream* tokens_ = nullptr;
    std::vector<ViewFieldMoveInferenceEntry> viewFieldMoveInferenceEntries_;
    std::vector<DeferredFunctionDefinition> deferredIncompleteFunctionDefinitions_;
    std::unordered_set<std::string> namespaceFunctionValueNames_;
    std::unordered_map<std::string, size_t> functionSourceOrder_;
    std::unordered_map<std::string, size_t> forwardDeclaredStructTypeSourceOrder_;
    // The eager generic-use scan runs before scoped expect_error handlers are armed.
    unsigned genericTypeUseScanDepth_ = 0;

    // Kept in sync with the codegen parser: only a declaration nested in a function body is a
    // local; return types, parameters, fields, and generic arguments keep their builtin shape.
    bool IsLocalDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs) const;

    LLVMBackend* Compiler(antlr4::ParserRuleContext* ctx);
    std::string ResolveTupleTypeSpecifier(CFlatParser::TupleTypeSpecifierContext* tupleSpec);
    TupleTypeElementResolveContext TupleTypeElementContext(antlr4::ParserRuleContext* site);
    std::string ResolveTupleTypeElementSpecifier(
        CFlatParser::TypeSpecifierContext* typeSpec,
        CFlatParser::MultiWordTypeSuffixContext* suffix,
        const std::unordered_map<std::string, std::string>* substitutions,
        const std::string& namespaceName);

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
        std::string scannedTypeName = ctx->directDeclarator()->getText();
        if (!namespaceName.empty()) scannedTypeName = namespaceName + "." + scannedTypeName;
        auto trackDefinition = [&](auto* definition, const std::string& name)
        {
            if (compiler->TrackAggregateDefinitionSite(name, DefinitionSiteText(compiler, definition),
                    definition->genericTypeParameters() != nullptr))
                Compiler(definition)->LogError(std::format(
                    "redefinition of '{}'", AggregateDefinitionName(definition)));
        };
        trackDefinition(ctx, scannedTypeName);
        const bool scannedCppStruct = IsGeneratedCppStruct(ctx);
        if (scannedCppStruct) compiler->RegisterCppStructName(scannedTypeName);
        // Generic template definitions are not pre-declared; they are instantiated on demand.
        // However, we still register the template name and its method names in the LSP
        // symbol index so hover / go-to-definition can resolve uses through a variable
        // of the instantiated type (e.g. `g_explosions.getPtr` -> `array.getPtr`).
        if (ctx->genericTypeParameters() != nullptr)
        {
            auto scanNestedTemplateDefinitions = [&](auto&& self, auto* aggregate,
                                                       const std::string& owner) -> void
            {
                for (auto* nested : MemberStructDefinitions(aggregate))
                {
                    const std::string name = owner + "." + nested->directDeclarator()->getText();
                    trackDefinition(nested, name);
                    self(self, nested, name);
                }
                for (auto* nested : MemberClassDefinitions(aggregate))
                {
                    const std::string name = owner + "." + nested->directDeclarator()->getText();
                    trackDefinition(nested, name);
                    self(self, nested, name);
                }
            };
            scanNestedTemplateDefinitions(scanNestedTemplateDefinitions, ctx, scannedTypeName);
            if (auto* s = compiler->GetSymbolSink())
            {
                std::string genBaseTypeName = ctx->directDeclarator()->getText();
                std::string typeName = genBaseTypeName;
                if (!namespaceName.empty())
                    typeName = namespaceName + "." + typeName;
                LLVMBackend::TypeAndValue displayType;
                displayType.TypeName = typeName;
                const std::string displayTypeName = SpellType(*compiler, displayType);
                std::string keyword = ctx->getStart()->getText();
                std::string doc = ExtractLeadingDoc(tokens_, ctx->getStart());
                s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            keyword + " " + displayTypeName, {}, doc, displayTypeName);
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
                                getFunctionSignatureText(func), {}, fdoc,
                                displayTypeName + "." + funcName);
                }
            }
            // A generic class's real base clause needs main-pass type-arg substitution, and its
            // instances appear only on demand: never prove anything about the interfaces it names.
            for (auto* spec : BaseClauseIdentifiers(ctx))
            {
                std::vector<std::string> candidates;
                AppendInterfaceNameCandidates(compiler, namespaceName, BaseSpecifierName(spec), candidates);
                for (const auto& c : candidates)
                    compiler->RecordUncertainGenericClassInterfaceImpl(c);
            }
            return;
        }

        std::string baseTypeName = ctx->directDeclarator()->getText();
        std::string typeName = baseTypeName;
        if (!namespaceName.empty())
            typeName = namespaceName + "." + typeName;
        const bool isStructDefinition = dynamic_cast<CFlatParser::StructDefinitionContext*>(ctx) != nullptr;
        const auto baseClauses = BaseClauseIdentifiers(ctx);
        const bool isCppStruct = IsGeneratedCppStruct(ctx);
        if (isCppStruct) compiler->RegisterCppStructName(typeName);

        if (isStructDefinition && !baseClauses.empty())
        {
            if (baseClauses.size() == 1)
            {
                auto* base = baseClauses[0];
                std::string baseName = BaseSpecifierName(base);
                std::string baseIdentity = baseName;
                if (auto* generic = base->genericTypeParameters())
                {
                    std::vector<std::string> typeArgs;
                    for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                        typeArgs.push_back(ResolveForwardTypeArg(entry));
                    const std::string spelledBase = baseName;
                    baseName = ResolveGenericTemplateSpellingAtUse(
                        compiler, spelledBase, typeArgs, generic, spelledBase, false);
                    compiler->CompleteCxxTemplateTypeArgs(baseName, typeArgs);
                    baseIdentity = MangleGenericInstance(*compiler, baseName, typeArgs);
                    std::string cxxError;
                    compiler->TryRequestCxxType(baseName, typeArgs, baseIdentity, cxxError,
                                                /*deferIncompleteCodegen*/ true);
                }
                else
                    baseIdentity = compiler->ResolveTypeAlias(baseIdentity);
                compiler->RecordCppStructBase(typeName, baseIdentity);
            }
        }

        // Record the declared interface list before any codegen, so a conversion site can see
        // implementors declared LATER in the file. Static-check only - see scannedInterfaceImpls.
        {
            std::vector<std::string> scannedIfaces;
            for (auto* spec : (isStructDefinition && !baseClauses.empty()
                               ? std::vector<CFlatParser::BaseSpecifierContext*>{}
                               : baseClauses))
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
            LLVMBackend::TypeAndValue displayType;
            displayType.TypeName = typeName;
            const std::string displayTypeName = SpellType(*compiler, displayType);
            s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        keyword + " " + displayTypeName, {}, doc, displayTypeName);
            // Also register under the unqualified name so Lookup("Point") finds "Geometry.Point".
            size_t dot = typeName.rfind('.');
            if (dot != std::string::npos)
                s->Register(SymbolKind::Struct, typeName.substr(dot + 1), compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            keyword + " " + displayTypeName, {}, doc, displayTypeName);
        }

        // C++ struct constructors are emitted as external thunks after the class request.
        LLVMBackend::TypeAndValue returnType{ .TypeName = typeName };
        if (!isCppStruct)
            compiler->CreateFunctionDeclaration(typeName, returnType, {});

        LLVMBackend::AliasScopeGuard aliasScope(compiler);
        ScanAggregateAliases(ctx->aggregateMember());
        ScanInlineAggregateMembers(ctx->aggregateMember(), typeName);
        // `Owner.name` from outside the body resolves through this frame until the main pass
        // walks the aggregate and saves its authoritative one.
        compiler->SaveAggregateAliasScope(typeName, /*provisional*/ true);

        // Method signatures use nested member types before their full scans run below.
        auto registerNestedShell = [&](auto* nested)
        {
            if (nested->genericTypeParameters() == nullptr)
                compiler->CreateStructType(typeName + "."
                    + nested->directDeclarator()->getText(), {});
        };
        for (auto* nestedStruct : MemberStructDefinitions(ctx)) registerNestedShell(nestedStruct);
        for (auto* nestedClass : MemberClassDefinitions(ctx)) registerNestedShell(nestedClass);

        // Pre-declare member functions (and detect constructor overloads).
        // Ctor signatures seen so far in this body, keyed by mangled name -> declaring line.
        std::map<std::string, size_t> seenCtorSignatures;
        for (auto func : MemberFunctionDefinitions(ctx))
        {
            // Ctors are written with the bare type name - match baseTypeName, not the
            // namespace-qualified typeName, or a namespaced class's ctor falls through below.
            if (!FunctionDeclaresReturnType(func) && getFunctionName(func) == baseTypeName)
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

                if (isCppStruct) continue;
                if (func->parameterTypeList() == nullptr) continue; // no-arg already declared above
                compiler->CreateFunctionDeclaration(typeName, returnType, allCtorParams);
            }
            else
            {
                if (func->genericTypeParameters() == nullptr && func->directDeclarator() != nullptr)
                    compiler->gts.structMethodBodies[typeName][getFunctionName(func)].push_back(func);
                ScanFunctionDefinition(func, typeName);
            }
        }

        // Pre-declare destructor
        for (auto dtor : MemberDestructorDefinitions(ctx))
        {
            if (isCppStruct) continue;
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

    // One `using` declaration considered by the pre-scan. Pure renames and simple pointer aliases
    // are registered before generic uses; other aliases remain authoritative in the main pass.
    void RegisterRenameAlias(CFlatParser::UsingDeclarationContext* ctx);
    void ScanAggregateAliases(const std::vector<CFlatParser::AggregateMemberContext*>& members);
    void ScanInlineAggregateMembers(
        const std::vector<CFlatParser::AggregateMemberContext*>& members,
        const std::string& ownerName);

public:
    ForwardRefScanner(LLVMBackend* compiler);
    void SetTokens(antlr4::BufferedTokenStream* t);
    void FinalizeDeferredFunctionDefinitions();
    std::vector<CFlatParser::FunctionDefinitionContext*> DeferredFunctionDefinitionContexts() const;
    void FinalizeViewFieldMoveInference();
    void CollectGlobalArrayViewNonInitializerStores(CFlatParser::TranslationUnitContext* tu,
                                                      bool allowUnresolvedGlobals = false);

    // Reject statically integral operands of pointer casts before code generation. The scanner
    // uses folded semantic constants and explicit scalar cast types, never source-text matching.
    void ValidateIsolatedIntegralPointerCasts(antlr4::tree::ParseTree* tree);

    // Register generated [cpp] struct names before signatures are scanned, so a declaration that
    // precedes the struct can still park a nested C++ request for retry after codegen emits it.
    void PreRegisterCppStructNames(antlr4::RuleContext* ctx);

    /*
     * Record every file-scope pure-rename or simple pointer `using` alias BEFORE either pass walks
     * the file, so MangleTypeArg folds the same alias set in both. It cannot be left to the walk:
     * ScanGenericTypeUses
     * mangles every generic use in the file BEFORE ScanExternalDeclaration reaches the first `using`,
     * while codegen sees the alias already registered - the two would build a struct shell under
     * `list$MyInt` and look it up as `list$int`.
     *
     * Array aliases and aliases whose target is not a scanned type stay opaque here. Both kinds
     * land in mangling-only tables read by the type-argument encoders, never in typeAliases: an
     * ordinary declaration still sees an alias only once its pass reaches the `using`.
     * Deliberately does NOT descend into `if const` arms or function bodies: the scan cannot tell
     * which arm is taken, and the two arms legitimately bind one alias to different types
     * (core/os.posix.cb binds `win_size` to i64 or i32). Those aliases stay opaque to the mangler,
     * which is what both passes did before this existed.
     */
    void PreRegisterRenameAliases(antlr4::RuleContext* ctx);
    void PreRegisterConditionalAliases(CFlatParser::IfConstDeclarationContext* ctx);

    // Seed file-scope const integer globals into the scanner's compile-time macro set.
    void SeedConstIntGlobals(CFlatParser::TranslationUnitContext* tu);
    void SeedConstFloatingGlobals(CFlatParser::TranslationUnitContext* tu);

    // Resolve a type-argument entry to the same string MainListener::ResolveTypeArgEntry
    // would produce at the top level (no active substitutions during the forward scan),
    // recursing into nested generics so list<int> inside list<list<int>> mangles to
    // "list$int" rather than the literal "list<int>". A trailing pointer becomes "*"
    // (MangleTypeArg later turns it into "ptr"), keeping shell names consistent with
    // the main pass.
    std::string ResolveForwardTypeArg(CFlatParser::TypeParameterEntryContext* entry);

    // Scanner counterpart of ResolveSigComponentCodegen: resolve a closure signature type. The scan
    // pass has no active substitutions, so a plain type stays getText(); a nested generic mangles to
    // match MangledGenericName; a nested closure encodes recursively. Names only (no queue/register).
    std::string ResolveSigComponentScanner(CFlatParser::TypeSpecifierContext* ts, bool& outPointer,
                                           CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr);
    // Resolve a function signature component; pointer aliases become a base name plus depth.
    // This is the scanner twin of the main pass copy.
    std::string ResolveSigComponentConstScanner(CFlatParser::TypeSpecifierContext* ts, bool& outPointer,
        int& stars, uint32_t& constMask, CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr,
        bool preserveAliasReferentConst = false, std::string* aliasSpelling = nullptr);

    /*
     * Scanner counterpart of MainListener::SigComponentResolvedKey. Both are one line onto the
     * shared LLVMBackend::FuncPtrComponentDeclaringKey, so the two ParseDeclarationSpecifiers
     * copies cannot drift. This copy is the load-bearing one: a call site reads the parameter
     * signature the SCANNER registered into the function table, not the main pass's.
     */
    std::string SigComponentResolvedKeyScanner(const std::string& name);

    // Scanner counterpart of EncodeClosureCodegen: builds the encoded name only (the main pass owns
    // registration, so the copy-overload is not lost to RegisterEncodedClosureType's idempotency).
    std::string EncodeClosureScanner(CFlatParser::FunctionPointerSpecifierContext* fpSpec);
    std::string EncodePlainFunctionTypeScanner(CFlatParser::FunctionTypeArgumentContext* fnSpec);

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
    void ScanGenericTypeUses(antlr4::RuleContext* ctx,
        const std::unordered_set<std::string>* localEnumNames = nullptr);
    std::string ResolveGenericTemplateBaseAtUse(LLVMBackend* compiler,
        const std::string& spelledBase, antlr4::tree::ParseTree* useSite,
        const std::string& lookupName = {}, NonGenericRefusal* refusal = nullptr);
    std::string ResolveGenericTemplateSpellingAtUse(LLVMBackend* compiler,
        const std::string& spelledBase, std::vector<std::string>& args,
        antlr4::tree::ParseTree* useSite, const std::string& lookupName, bool report,
        NonGenericRefusal* refusal = nullptr);

    // Build the function-pointer TypeAndValue for a `function<R(Args)>` spec - the resolved form
    // a `using Cb = function<R(Args)>;` alias stores and that ParseDeclarationSpecifiers expands.
    // The closure value rides IsFunctionPointer + the signature; TypeName is the backing fat type.
    LLVMBackend::TypeAndValue BuildFuncPtrAliasType(CFlatParser::FunctionPointerSpecifierContext* fpSpec);

    bool RegisterGenericAlias(const std::string& alias, CFlatParser::TypeSpecifierContext* typeSpec,
        const std::string& parsedSpelling, const std::string& suffix, uint32_t constMask,
        bool genericDerived, antlr4::ParserRuleContext* ctx,
        CFlatParser::GenericTypeParametersContext* genericCtx);
    void ScanUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx);
    // `Spec<args>.name` in type position: the member type's spelling, or the written text when
    // the scan cannot resolve it yet (the main pass reports).
    std::string ResolveMemberTypeSpecifier(CFlatParser::MemberTypeSpecifierContext* ctx,
        uint32_t* constMask = nullptr);
    // Collects the `using` members of the CFlat generic instantiation `owner` (template `base`,
    // arguments `args`) as a provisional frame, binding the template parameters textually.
    bool ScanTemplateAggregateAliases(const std::string& owner, const std::string& base,
                                      const std::vector<std::string>& args, bool scanSignatures = false);
    // Template parameter -> argument while ScanTemplateAggregateAliases evaluates a body's aliases.
    std::unordered_map<std::string, std::string> scanTypeSubstitutions_;
    // Generic struct/class templates of this file by qualified name, from the name scan, and
    // the namespace each is declared in (its alias targets resolve there).
    std::unordered_map<std::string, antlr4::ParserRuleContext*> scannedGenericAggregates_;
    std::unordered_map<std::string, std::string> scannedGenericAggregateNs_;
    // Owners given a provisional alias frame during ScanGenericTypeUses, before record shells
    // exist; RefreshProvisionalAggregateAliases re-collects them once every shell is registered.
    struct ProvisionalAliasOwner { std::string owner, base; std::vector<std::string> args; };
    std::vector<ProvisionalAliasOwner> provisionalAliasOwners_;
    // Set while ScanTemplateAggregateAliases runs: the template's namespace ("" = file scope).
    std::optional<std::string> provisionalAliasNs_;
    void RefreshProvisionalAggregateAliases();

    void ScanProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx);

    void ScanImportedProgramDefinition(const std::string& name);

    void ScanAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx);

    void ScanExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {});
    void PreRegisterAggregateTypeShells(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {});
    void PreRegisterEnums(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {});
    void PreRegisterGlobals(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {});

    // Prepare all forward-scanned [winrt] classes before the main code-generation walk.
    void PrepareWinrtClasses(antlr4::RuleContext* ctx, const std::string& namespaceName = {});

    void ScanNamespace(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {});
};




struct FoldConstructedValueState;

/*
 * Where a pointer value's storage root sits (MainListener::PointerRootKind). Ordered by how
 * much the root is a problem for an interface return; Undecided is "cannot tell" (refuses for
 * arithmetic, accepted by the return-dangle check).
 */
enum class PointerValueRootKind { Borrowed, Undecided, Owned, OwnedLocal, AliasOfOwnedLocal, FrameLocal };

class MainListener : public CFlatBaseListener
{
private:
    // Names the function whose body is being emitted, so block-scope enums are keyed to it.
    struct ActiveLocalEnumFunctionScope
    {
        LLVMBackend* backend;
        std::string saved;
        ActiveLocalEnumFunctionScope(LLVMBackend* b, const std::string& name)
            : backend(b), saved(std::move(b->activeLocalEnumFunction_))
        {
            backend->activeLocalEnumFunction_ = name;
        }
        ~ActiveLocalEnumFunctionScope() { backend->activeLocalEnumFunction_ = std::move(saved); }
    };
    CFlatParser* parser;
    LLVMBackend* compilerLLVM;
    std::function<void(antlr4::ParserRuleContext*, std::string)> savedContextualErrorReporter_;
    std::string sourceFileName;
    std::string importNamespace_;
    unsigned addressOfOperandDepth_ = 0;
    // A moved-slot VALUE read seen while an equality operand is parsed: logged only after the
    // comparison settles (ParseEqualityExpression exempts the one read that is the whole operand).
    struct DeferredMovedRead
    {
        // The value the read produced (its load, or the get() call of a unique slot); matched by identity.
        llvm::Value* value = nullptr;
        std::string subject;
        int line = 0;
        int column = 0;
        // The moved path is this read's own tracked element slot (not a whole moved owner it lives in).
        bool ownSlot = false;
        bool pointeeMove = false;
    };
    std::vector<DeferredMovedRead>* deferredMovedReads_ = nullptr;
    // Owning rvalue receivers of operator-> forwarding (`mk()->v`) registered for cleanup.
    struct ArrowOwnedTemp
    {
        llvm::Value* Storage = nullptr;
        CFlatParser::PostfixExpressionContext* Origin = nullptr;
        CFlatParser::PostfixExpressionContext* PendingCallResult = nullptr;
        llvm::Value* PendingCallValue = nullptr;
        LLVMBackend::TypeAndValue PendingCallReturnType;
        CFlatParser::AdditiveExpressionContext* PendingPointerDifference = nullptr;
        const antlr4::tree::TerminalNode* PendingCallParen = nullptr;
        size_t ArgumentIndex = SIZE_MAX;
        size_t PointerDifferenceOperatorIndex = SIZE_MAX;
        bool NamedArgument = false;
        bool HasPendingCallValue = false;
    };
    std::vector<ArrowOwnedTemp> arrowOwnedTemps_;
    std::unordered_map<CFlatParser::FunctionDefinitionContext*,
        std::unordered_set<std::string>> functionLocalEnumNamesCache_;
    struct ReturnedStructFrameCandidate
    {
        antlr4::ParserRuleContext* Context = nullptr;
        LLVMBackend::NamedVariable Value;
        llvm::ReturnInst* Return = nullptr;
        std::unordered_map<std::string, std::string> RootNames;
    };
    std::unordered_map<llvm::Function*, std::vector<ReturnedStructFrameCandidate>> returnedStructFrameCandidates_;
    enum class CxxReferenceMemberLifetime : uint8_t { Frame, Stable, Unknown };
    struct CxxReferenceMemberProvenance
    {
        llvm::Value* Root = nullptr;
        std::vector<int64_t> ObjectPath;
        std::string AggregateType;
        std::string FieldName;
        CxxReferenceMemberLifetime Lifetime = CxxReferenceMemberLifetime::Unknown;
        antlr4::ParserRuleContext* Context = nullptr;
        // Seq is the source-order position; Regions the structured nesting (branch, loop,
        // switch case) of the write. Whole overwrites every member under ObjectPath.
        uint64_t Seq = 0;
        std::vector<uint32_t> Regions;
        bool Whole = false;
        // A join kill made where every branch path wrote the same member: it kills only facts
        // whose Seq lies in KillRanges ([lo, hi) pairs), never live by itself.
        bool Join = false;
        std::vector<std::pair<uint64_t, uint64_t>> KillRanges;
    };
    struct CxxFactGroup
    {
        uint32_t Id = 0;
        uint64_t Start = 0;
        uint64_t End = 0;
        uint64_t BreakSeq = UINT64_MAX;
        size_t Depth = 0;
        llvm::Function* Function = nullptr;
    };
    struct CxxFactPoint
    {
        uint64_t Seq = 0;
        std::vector<uint32_t> Regions;
    };
    // A query inside a loop body is re-run when the loop closes, to see writes on the back edge.
    struct CxxDeferredFactQuery
    {
        bool Check = false;
        bool Done = false;
        std::function<bool(bool& done)> Run;
    };
    struct CxxFactRegion
    {
        uint32_t Id = 0;
        bool Loop = false;
        bool SwitchCase = false;
        bool AlwaysEnters = false;
        bool ContinueExits = false;
        bool HasDefault = false;
        uint64_t Start = 0;
        uint64_t GroupStart = 0;
        uint64_t BreakSeq = UINT64_MAX;
        std::vector<CxxFactGroup> Groups;
        std::vector<CxxDeferredFactQuery> Deferred;
    };
    CxxFactGroup lastPoppedCxxFactRegion_;
    std::unordered_map<llvm::Function*, std::vector<CxxReferenceMemberProvenance>>
        cxxReferenceMemberProvenance_;
    std::unordered_map<llvm::Function*, std::vector<CxxFactRegion>> cxxFactRegions_;
    std::vector<bool> cxxFactRegionIsLoop_;
    uint64_t cxxFactSeq_ = 0;
    std::unordered_map<llvm::Function*, std::vector<std::pair<uint64_t, uint32_t>>> cxxFactJumps_;
    bool rejectCxxReferenceMemberReturnBrace_ = false;

    LLVMBackend* Compiler(antlr4::ParserRuleContext* ctx);
    inline LLVMBackend* Compiler() { return compilerLLVM; }
    std::string BorrowedParameterCallName(
        const LLVMBackend::NamedVariable& source, llvm::Value* value);
    const std::unordered_set<std::string>& FunctionLocalEnumNames(
        CFlatParser::FunctionDefinitionContext* function);

public:
    void enterEveryRule(antlr4::ParserRuleContext* ctx) override
    {
        RequireStackHeadroom(ctx);
    }

    void enterTranslationUnit(CFlatParser::TranslationUnitContext* ctx) override
    {
        cxxReferenceMemberProvenance_.clear();
        cxxFactRegions_.clear();
        cxxFactRegionIsLoop_.clear();
        cxxFactSeq_ = 0;
        cxxFactJumps_.clear();
        lastPoppedCxxFactRegion_ = {};
        rejectCxxReferenceMemberReturnBrace_ = false;
        ForwardRefScanner scanner(Compiler(ctx));
        scanner.ValidateIsolatedIntegralPointerCasts(ctx);
    }

    void exitTranslationUnit(CFlatParser::TranslationUnitContext* ctx) override;

    void enterProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx) override
    {
        Compiler(ctx)->ValidateIsolatedProgram(ctx);
    }

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
    struct IncrementWork
    {
        int Amount = 0;
        llvm::Type* ElemType = nullptr;
        llvm::Type* LoadType = nullptr;
        std::vector<int> FloatingSteps;
    };
    std::unordered_map<llvm::Value*, IncrementWork> PlusPlus;
    /*
     * Isolates postfix work created while lowering a conditional operand or user call arguments.
     * Flush() emits only the isolated entries; the outer pending work is restored on exit.
     */
    struct BranchPostfixScope
    {
        MainListener* self;
        std::unordered_map<llvm::Value*, IncrementWork> saved;
        BranchPostfixScope* previousCallScope = nullptr;
        explicit BranchPostfixScope(MainListener* s)
            : self(s), previousCallScope(s->activeCallPostfixScope_)
        {
            saved.swap(s->PlusPlus);
            s->activeCallPostfixScope_ = this;
        }
        BranchPostfixScope(const BranchPostfixScope&) = delete;
        BranchPostfixScope& operator=(const BranchPostfixScope&) = delete;
        void Flush() { self->ProcessPlusPlus(); }
        ~BranchPostfixScope()
        {
            self->activeCallPostfixScope_ = previousCallScope;
            saved.swap(self->PlusPlus);
            for (auto& [destination, w] : saved)
            {
                auto& outer = self->PlusPlus[destination];
                outer.Amount += w.Amount;
                outer.FloatingSteps.insert(outer.FloatingSteps.end(),
                                           w.FloatingSteps.begin(), w.FloatingSteps.end());
                if (outer.ElemType == nullptr) outer.ElemType = w.ElemType;
                if (outer.LoadType == nullptr) outer.LoadType = w.LoadType;
            }
        }
    };
    using CallPostfixScope = BranchPostfixScope;
    BranchPostfixScope* activeCallPostfixScope_ = nullptr;
    void FlushCallPostfix()
    {
        if (activeCallPostfixScope_ != nullptr)
            activeCallPostfixScope_->Flush();
        else
            ProcessPlusPlus();
    }
    /*
     * Isolates the pending postfix ++/-- of the expression being lowered from a function BODY
     * compiled mid-expression (lambda, first-use generic instantiation, pending struct/method
     * instantiation): the body's statement flushes must not emit the outer work into it. The
     * outer work is restored untouched on exit; the body's own leftovers are dropped.
     */
    struct PendingPostfixScope
    {
        MainListener* self;
        std::unordered_map<llvm::Value*, IncrementWork> saved;
        BranchPostfixScope* savedCallScope = nullptr;
        explicit PendingPostfixScope(MainListener* s)
            : self(s), savedCallScope(s->activeCallPostfixScope_)
        {
            saved.swap(s->PlusPlus);
            s->activeCallPostfixScope_ = nullptr;
        }
        PendingPostfixScope(const PendingPostfixScope&) = delete;
        PendingPostfixScope& operator=(const PendingPostfixScope&) = delete;
        ~PendingPostfixScope()
        {
            self->PlusPlus.swap(saved);
            self->activeCallPostfixScope_ = savedCallScope;
        }
    };
    bool global_scope = true; // true when parsing an entity in the global scope.

    struct PendingGlobalDefaultConstruction
    {
        llvm::GlobalVariable* Global = nullptr;
        LLVMBackend::DeclTypeAndValue TypeValue;
        antlr4::ParserRuleContext* Context = nullptr;
        // `T g{}` / `T g = {}`: an empty brace reaches the list constructor when there is no
        // default constructor. CopyListInit is the `= {}` spelling (explicit ctors refused).
        bool EmptyBraceInit = false;
        bool CopyListInit = false;
        // C++-class array whose every element is an empty brace `{}` (each element is built
        // through the same empty-brace route at startup).
        bool EmptyBraceElements = false;
        // `S[N] g = default;` / `= {}` whose element fold failed at the declaration: the retry folds
        // only, and a failing retry keeps the zero constant (no runtime construction, no note).
        bool FoldOnly = false;
        // A constant initializer naming a thread_local's address (`S* p = &s`) is not a link-time
        // constant; the global starts zero and this value is stored at its turn in the init order.
        llvm::Constant* StoreValue = nullptr;
        // Nonempty C++ aggregate brace lists are built in the ordered module initializer.
        CFlatParser::InitializerListContext* BraceInitializer = nullptr;
        // Declaration order across BOTH queues (native and C++), so one module initializer runs
        // the constructions in the order the globals were declared.
        size_t Sequence = 0;
    };
    std::vector<PendingGlobalDefaultConstruction> pendingGlobalDefaultConstructions_;

    // File-scope objects (and arrays of them) of a foreign C++ class whose default constructor
    // is nontrivial: the call cannot be a constant initializer, so it runs from a module ctor.
    std::vector<PendingGlobalDefaultConstruction> pendingGlobalCxxConstructions_;
    size_t pendingGlobalSequence_ = 0;
    // Set by GenerateDefaultValue when a file-scope struct default did not fold; the declarator
    // reads and clears it to queue the construction for the module initializer.
    bool globalDefaultDeferred_ = false;
    static bool ConstantNamesThreadLocal(const llvm::Constant* constant);
    void QueuePendingGlobalConstruction(std::vector<PendingGlobalDefaultConstruction>& queue,
        PendingGlobalDefaultConstruction item);
    bool HasGlobalCxxFieldDefaultConstruction(const LLVMBackend::TypeAndValue& typeValue);
    // True when a field default parse tree names one of the members isMember accepts.
    static bool FieldDefaultNamesMember(antlr4::tree::ParseTree* node,
        const std::function<bool(const std::string&)>& isMember);
    void EmitGlobalCxxFieldDefaultConstruction(llvm::Value* slot, llvm::Type* type,
        const LLVMBackend::TypeAndValue& typeValue, antlr4::ParserRuleContext* context);

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
    std::string lambdaEnclosingAggregateOwner_;
    std::string fieldDefaultAggregateOwner_;
    std::string fieldDefaultAggregateFieldName_;
    // Destination type of the initializer/assignment being evaluated, for ANY type - not just a
    // function pointer. An immediately-invoked lambda literal reads its return type from here
    // instead of inheriting the enclosing literal's. Empty (and never "auto") when no context.
    LLVMBackend::TypeAndValue declExpectedType;
    CFlatParser::CastExpressionContext* pendingCxxCastInitContext_ = nullptr;
    // Parser ambiguity for `decl = () => _ = expr`: the outer assignment can claim the discard
    // tail, leaving the lambda body as `_`. The named-expression path temporarily hands that RHS
    // to ParseLambdaExpression so the discard remains inside the lambda.
    CFlatParser::AssignmentExpressionContext* lambdaDiscardRhs_ = nullptr;
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
    std::unordered_set<const antlr4::tree::ParseTree*> namespaceFunctionIdentifierNodes_;
    std::unordered_set<std::string> namespaceLambdaFunctionNames_;

    // Lock-set analysis: the canonical lock expressions currently held, and HOW each is held.
    // Cleared at function entry, seeded from the function's lock clause, and pushed/popped
    // around lock statement bodies. Membership grants a read of a guarded field; only
    // LockMode::Exclusive grants a write (see CheckGuardedWrite).
    std::unordered_map<std::string, LockMode> currentLockSet;

    // State a failed parse can leave behind when LogError unwinds out of a nested lambda.
    // Scoped expect_error recovery restores this checkpoint before resuming the outer walk.
    struct ExpectErrorRecoveryMark
    {
        std::unordered_set<llvm::Function*> openFunctions;
        LLVMBackend::TypeAndValue lambdaExpectedType;
        LLVMBackend::TypeAndValue lastLambdaType;
        LLVMBackend::TypeAndValue declExpectedType;
        CFlatParser::CastExpressionContext* pendingCxxCastInitContext = nullptr;
        CFlatParser::AssignmentExpressionContext* lambdaDiscardRhs = nullptr;
        std::string lambdaLockThisReceiver;
        LockMode lambdaLockThisMode = LockMode::Exclusive;
        std::unordered_map<std::string, LockMode> currentLockSet;
        llvm::Function* lambdaReturnInferredFn = nullptr;
        std::string lambdaReturnInferredName;
        bool straightLineReturned = false;
        bool currentFunctionIsVariadic = false;
        unsigned addressOfOperandDepth = 0;
        size_t arrowOwnedTempsSize = 0;
        LLVMBackend::TypeAndValue lastParenExprType;
        llvm::Value* lastParenExprStorage = nullptr;
        bool lastParenExprFromOwningTempField = false;
        bool lastParenExprOwningTempParent = false;
        std::string lastParenExprOwningStructName;
        std::string lastParenExprFieldName;
        std::string lastParenExprCallerName;
        LLVMBackend::NamedVariable lastParenExprNamed;
        CFlatParser::TypeCheckExpressionContext* lastRedundantAsCtx = nullptr;
        LLVMBackend::NamedVariable lastRedundantAsNamed;
        llvm::Value* pendingCxxSretDest = nullptr;
        std::string pendingCxxSretTypeName;
        bool pendingCxxSretForFixedArray = false;
        bool pendingCxxSretReturn = false;
        llvm::Value* pendingCxxTernaryDeclDest = nullptr;
        std::string pendingCxxTernaryDeclTypeName;
        bool pendingCxxTernaryDeclConsumed = false;
        bool pendingCxxTernaryDeclFailed = false;
        std::string pendingCxxTernaryDeclArmType;
        int pendingCxxTernaryDeclArmCat = 0;
        llvm::Value* lastCxxRetTemp = nullptr;
        llvm::Value* lastCxxRetValue = nullptr;
        llvm::Value* lastLoweredRetTemp = nullptr;
        llvm::Value* lastLoweredRetValue = nullptr;
        llvm::Value* lastLoweredCopyRetTemp = nullptr;
    };
    ExpectErrorRecoveryMark MarkExpectErrorRecoveryState() const;
    void RestoreExpectErrorRecoveryState(const ExpectErrorRecoveryMark& mark,
                                         llvm::Function* resumedFunction);

    // Generic template state lives on the backend so each LLVMBackend instance
    // owns its own copy. References below alias compilerLLVM->gts.<field> and are
    // bound in the constructor's member initializer list.
    using PendingInstantiation = GenericTemplateState::PendingInstantiation;

    std::unordered_map<std::string, CFlatParser::StructDefinitionContext*>&     genericStructTemplates;
    std::unordered_map<std::string, CFlatParser::ClassDefinitionContext*>&      genericClassTemplates;
    std::unordered_map<std::string, std::vector<LLVMBackend::AliasScopeFrame>> genericLexicalAliasScopes_;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericStructTypeParams;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericStructValueParams;
    std::unordered_set<std::string>&                                            instantiatedGenerics;
    std::unordered_map<std::string, std::vector<CFlatParser::FunctionDefinitionContext*>>
        failedAggregateMethodBodies_;
    // Constraints: templateName -> { typeParamName -> [requiredInterface, ...] }
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericStructConstraints;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericClassConstraints;
    // Active type parameter substitutions during generic instantiation (e.g. "T" -> "int")
    TypeSubstitutionMap activeTypeSubstitutions;
    // Active compile-time value substitutions during generic instantiation (e.g. "N" -> "8").
    std::unordered_map<std::string, std::string> activeValueSubstitutions;
    // >0 while a generic free-function template's signature is parsed at its definition: its own
    // type parameters are unbound there, so a tuple return/param naming one must not instantiate.
    // Names holds exactly that template's type parameter names (suppressed only when mentioned).
    int genericTemplateSignatureDepth_ = 0;
    std::unordered_set<std::string> genericTemplateSignatureNames_;

    // Pack param index per template: index of the variadic param, or npos if not variadic
    std::unordered_map<std::string, size_t>& genericStructPackIndex;
    std::unordered_map<std::string, size_t>& genericClassPackIndex;
    std::unordered_map<std::string, size_t>& genericFunctionPackIndex;
    std::unordered_map<std::string, size_t>& genericInterfacePackIndex;

    // Active pack substitutions during instantiation: pack-param-name -> ["int", "float", "string"]
    std::unordered_map<std::string, std::vector<std::string>> activePackSubstitutions;

    enum class DeferredGenericMemberKind : uint8_t { Function, Constructor, Destructor };
    struct DeferredGenericMemberBody
    {
        DeferredGenericMemberKind kind = DeferredGenericMemberKind::Function;
        CFlatParser::FunctionDefinitionContext* function = nullptr;
        CFlatParser::DestructorDefinitionContext* destructor = nullptr;
        std::string ownerName;
        std::string templateName;
        std::string declaringNamespace;
        std::string nameOverride;
        bool suppliesNoArgCtor = false;
        size_t cppCtorIndex = SIZE_MAX;
        bool hasExplicitNoArgCtor = false;
        bool waitForEnclosingLayout = false;
        TypeSubstitutionMap typeSubstitutions;
        std::unordered_map<std::string, std::string> valueSubstitutions;
        std::unordered_map<std::string, std::vector<std::string>> packSubstitutions;
    };
    std::vector<DeferredGenericMemberBody> deferredGenericMemberBodies_;
    bool GenericMemberNeedsDeferredBody(antlr4::ParserRuleContext* member);
    void DeferGenericMemberBody(antlr4::ParserRuleContext* member,
        antlr4::ParserRuleContext* aggregate, const std::string& ownerName,
        DeferredGenericMemberKind kind, const std::string& nameOverride = {},
        bool suppliesNoArgCtor = false, size_t cppCtorIndex = SIZE_MAX,
        bool hasExplicitNoArgCtor = false, bool waitForEnclosingLayout = false);
    void ProcessDeferredGenericMemberBodies(size_t first = 0, bool enclosingLayoutReady = false,
        const std::string& ownerPrefix = {});
    bool preparsingAggregateLayouts_ = false;

    // True while monomorphizing a generic template body. The body's tokens come from the
    // template's own file (e.g. core/list.cb), but currentSourceFilePath_ is the use-site
    // file that triggered the instantiation, so a token's line number does not map onto it.
    // Unused-decl candidates must not be recorded here: the faded "never used" hint would
    // land at a bogus location in the instantiating file (e.g. on an import line).
    bool InGenericInstantiation() const;
    // True when a resolved element name mentions (whole token, '$'-split) a signature type parameter.
    bool MentionsSignatureTypeParam(const std::string& name) const;
    // Definition-time check of a tuple signature: reports unknown element names the instance would hit.
    void ReportUnknownSignatureTupleElements(CFlatParser::TupleTypeSpecifierContext* tts);

    // Struct scope stack: pushed when parsing fields/methods of a struct/class so that
    // unqualified nested type names (e.g. "Inner") resolve to "Outer.Inner".
    std::vector<std::string> structScopeStack;
    std::unordered_map<std::string, std::string> aggregateParentScopes_;
    std::unordered_map<std::string, antlr4::ParserRuleContext*> aggregateDefinitionContexts_;

    std::vector<std::string> EnclosingAggregateScopesForType(
        CFlatParser::TypeSpecifierContext* typeSpec)
    {
        auto* compiler = Compiler(typeSpec);
        const auto contextScopes = EnclosingAggregateTypeScopesFromContext(compiler, typeSpec);
        std::unordered_map<antlr4::ParserRuleContext*, std::string> activeByDefinition;
        std::unordered_set<std::string> visited;
        for (auto it = structScopeStack.rbegin(); it != structScopeStack.rend(); ++it)
        {
            std::string scope = *it;
            while (!scope.empty() && visited.insert(scope).second)
            {
                auto definition = aggregateDefinitionContexts_.find(scope);
                if (definition != aggregateDefinitionContexts_.end() && definition->second != nullptr)
                    activeByDefinition.try_emplace(definition->second, scope);
                auto parent = aggregateParentScopes_.find(scope);
                if (parent == aggregateParentScopes_.end()) break;
                scope = parent->second;
            }
        }

        std::vector<std::string> scopes;
        scopes.reserve(contextScopes.size());
        for (const auto& contextScope : contextScopes)
        {
            auto active = activeByDefinition.find(contextScope.Definition);
            scopes.push_back(active != activeByDefinition.end() ? active->second : contextScope.Name);
        }
        return scopes;
    }

    // C++ struct definitions parsed early so their generated C++ is available to later member
    // bodies, while the current struct's fields still observe source-order completeness.
    std::unordered_set<const CFlatParser::StructDefinitionContext*> preparsedCppStructDefinitions_;
    // Source aggregate definitions whose layout/template registration is completed before codegen.
    std::unordered_set<const CFlatParser::StructDefinitionContext*> preparsedAggregateDefinitions_;
    std::unordered_set<const CFlatParser::ClassDefinitionContext*> preparsedAggregateClassDefinitions_;
    std::unordered_set<const CFlatParser::InterfaceDefinitionContext*> preparsedInterfaceDefinitions_;

    // Set only while ParseDeclarationList parses a field's declarationSpecifiers. Pointer-form
    // `unique` fields desugar to core unique<T>; unique void* remains builtin.
    bool inStructFieldDecl_ = false;
    // Interface fields retain their builtin ownership contract for vtable layout.
    bool inInterfaceFieldDecl_ = false;
    // Set only while a declaration parser is handling a function-body local. Pointer-form
    // `unique X*` is rewritten to the core `unique<X>` value; interface locals remain builtin.
    bool inLocalDecl_ = false;
    struct PendingCallVisibleLocal
    {
        std::string Name;
        LLVMBackend::TypeAndValue Type;
        llvm::Function* Function = nullptr;
        size_t ScopeDepth = 0;
        antlr4::ParserRuleContext* Initializer = nullptr;
        llvm::Value* AddressStorage = nullptr;
    };
    std::vector<PendingCallVisibleLocal> pendingCallVisibleLocals_;
    const PendingCallVisibleLocal* FindPendingCallVisibleLocal(
        LLVMBackend* backend, const std::string& name, antlr4::tree::ParseTree* use,
        bool requireInitializer = false) const
    {
        if (backend == nullptr) return nullptr;
        const size_t visibleDepth = backend->FindVariableScopeDepth(name);
        for (auto it = pendingCallVisibleLocals_.rbegin(); it != pendingCallVisibleLocals_.rend(); ++it)
        {
            if (it->Name != name) continue;
            bool inInitializer = false;
            bool inLambda = false;
            for (auto* node = use; node != nullptr; node = node->parent)
            {
                if (node == it->Initializer)
                {
                    inInitializer = true;
                    break;
                }
                if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr)
                    inLambda = true;
            }
            const bool sameFunction = it->Function == backend->currentFunction;
            if (requireInitializer)
            {
                if (!inInitializer || (!sameFunction && !inLambda)
                    || (sameFunction && visibleDepth != SIZE_MAX && visibleDepth > it->ScopeDepth)
                    || NameShadowedByEnclosingLocal(use, name, nullptr, it->Initializer)) continue;
            }
            else if (sameFunction)
            {
                if (visibleDepth != SIZE_MAX && visibleDepth > it->ScopeDepth) continue;
            }
            else if (!inInitializer || !inLambda
                || NameShadowedByEnclosingLocal(use, name, nullptr, it->Initializer)) continue;
            return &*it;
        }
        return nullptr;
    }
    const PendingCallVisibleLocal* FindPendingInitializerReadLocal(
        LLVMBackend* backend, const std::string& name, antlr4::tree::ParseTree* use) const
    {
        while (use != nullptr && dynamic_cast<antlr4::ParserRuleContext*>(use) == nullptr) use = use->parent;
        return FindPendingCallVisibleLocal(backend, name, use, true);
    }
    struct PendingCallVisibleLocalScope
    {
        MainListener* self = nullptr;
        bool active = false;
        PendingCallVisibleLocalScope(MainListener* owner, LLVMBackend* backend,
            const std::string& name, const LLVMBackend::TypeAndValue& type,
            antlr4::ParserRuleContext* initializer)
        {
            if (owner == nullptr || backend == nullptr || owner->global_scope
                || backend->currentFunction == nullptr || backend->stackNamedVariable.empty())
                return;
            self = owner;
            self->pendingCallVisibleLocals_.push_back({
                name, type, backend->currentFunction, backend->stackNamedVariable.size() - 1,
                initializer });
            active = true;
        }
        ~PendingCallVisibleLocalScope()
        {
            if (active) self->pendingCallVisibleLocals_.pop_back();
        }
        PendingCallVisibleLocalScope(const PendingCallVisibleLocalScope&) = delete;
        PendingCallVisibleLocalScope& operator=(const PendingCallVisibleLocalScope&) = delete;
    };
    struct LocalDeclGuard
    {
        bool& flag;
        bool saved;
        LocalDeclGuard(bool& f, bool active) : flag(f), saved(f) { flag = active; }
        ~LocalDeclGuard() { flag = saved; }
    };
    // Saves and restores rather than clearing: if a specifier parse ever nests (a generic
    // instantiation parsing its own fields), clearing would spuriously reject the outer field.
    struct StructFieldDeclGuard
    {
        bool& flag;
        bool  prev;
        explicit StructFieldDeclGuard(bool& f) : flag(f), prev(f) { flag = true; }
        ~StructFieldDeclGuard() { flag = prev; }
    };
    struct InterfaceFieldDeclGuard
    {
        bool& flag;
        bool prev;
        explicit InterfaceFieldDeclGuard(bool& f) : flag(f), prev(f) { flag = true; }
        ~InterfaceFieldDeclGuard() { flag = prev; }
    };

    // Set while ParseStructDefinition parses the members of a UNION body. `unique` is rejected
    // there (ValidateUniqueField): a synthesized destructor cannot know which member is active.
    bool inUnionFieldDecl_ = false;
    struct PendingNontrivialCxxUnionMember
    {
        std::string UnionName;
        LLVMBackend::DeclTypeAndValue Field;
        antlr4::ParserRuleContext* Context = nullptr;
        antlr4::ParserRuleContext* UnionContext = nullptr;
        std::unordered_set<std::string> IncompleteTypes;
    };
    // Recheck only after one of the member's opaque by-value types completes.
    std::vector<PendingNontrivialCxxUnionMember> pendingNontrivialCxxUnionMembers_;
    std::string FindNontrivialCxxTypeForUnionMember(
        const LLVMBackend::DeclTypeAndValue& field,
        std::unordered_set<std::string>& incompleteTypes);
    void RecheckPendingNontrivialCxxUnionMembers(const std::string& completedTypeName);
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
    std::unordered_map<std::string, std::vector<std::string>>&                  genericInterfaceValueParams;
    std::unordered_set<std::string>&                                            instantiatedInterfaces;

    std::unordered_map<std::string, std::vector<CFlatParser::FunctionDefinitionContext*>>& genericFunctionTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericFunctionTypeParams;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericFunctionValueParams;
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
        bool valueIsUnsigned = false;             // integer label came from an unsigned-typed constant
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
        // Moved state before the body, and the exit states that reach resume (breaks, arm ends).
        // Labels merge preSwitchMoved in (dispatch may skip earlier cases); resume merges all exits.
        LLVMBackend::MovedStateSnapshot preSwitchMoved;
        std::vector<LLVMBackend::MovedStateSnapshot> exitMoved;
        LLVMBackend::OwnFlagJoinSnapshot preSwitchOwnFlag;
        std::vector<LLVMBackend::OwnFlagJoinSnapshot> exitOwnFlag;
        size_t loopDepth = 0;                     // loopBreakMovedStates_ depth at the switch
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
    llvm::BasicBlock* expectErrorRecoveryBlock_ = nullptr;

    struct NamedReturnElisionContext
    {
        llvm::Function* function = nullptr;
        // The locals every return names within their live ranges (scope-resolved declarators);
        // they all share the sret slot, and their live ranges never overlap.
        std::vector<CFlatParser::DirectDeclaratorContext*> declarators;
        llvm::Value* sretStorage = nullptr;
    } namedReturnElision_;

    struct NamedReturnElisionScope
    {
        MainListener& owner;
        NamedReturnElisionContext saved;
        NamedReturnElisionScope(MainListener& listener, NamedReturnElisionContext next)
            : owner(listener), saved(listener.namedReturnElision_)
        {
            owner.namedReturnElision_ = std::move(next);
        }
        ~NamedReturnElisionScope() { owner.namedReturnElision_ = std::move(saved); }
    };

    // Set by the break/continue handlers. Like a return, a break/continue branch does NOT reach
    // the if's resume block, so its moves must not leak onto the fall-through path. Unlike a
    // return the moves are not dead: they are collected in loopBreakMovedStates_ and merged
    // back at loop exit, so a use-after-move past the loop is still diagnosed.
    bool straightLineJumped_ = false;
    bool straightLineContinued_ = false;

    // Per-loop stack of moved-state snapshots taken at each break/continue inside the body.
    std::vector<std::vector<LLVMBackend::MovedStateSnapshot>> loopBreakMovedStates_;
    // Parallel to loopBreakMovedStates_: the loop's condition folds to true, so `continue` never exits it.
    std::vector<bool> loopConditionAlwaysTrue_;

    struct RangeForContext
    {
        std::string variableName;
        LLVMBackend::NamedVariable collection;
        llvm::Value* indexStorage = nullptr;
        llvm::Value* collectionStorage = nullptr;
        llvm::Value* collectionValue = nullptr;
        llvm::Value* elementStorage = nullptr;
        llvm::Type* elementBaseType = nullptr;
        LLVMBackend::TypeAndValue elementType;
        llvm::Value* interfaceStorage = nullptr;
        bool isFixedArray = false;
        bool isArrayView = false;
        bool isInterface = false;
        // list<T> (_data/_size) / array<T> (_ptr/_len) buffer and count field indices: the live slot is
        // recomputed from the container since its buffer may move. -1 = no live slot.
        int headerDataField = -1;
        int headerSizeField = -1;
        // The element as the iteration bound it, loaded before the body: the adopted-value test.
        llvm::Value* elementValue = nullptr;
    };
    std::vector<RangeForContext> rangeForStack_;

    RangeForContext* FindActiveRangeForVariable(const LLVMBackend::NamedVariable& nv);
    bool IsActiveRangeCollectionElement(const LLVMBackend::NamedVariable& nv) const;

    // Save/restore straightLineReturned_ across a construct that falls through (loop, switch):
    // a return in its body must not mark the enclosing straight-line as returned.
    struct ReturnFlagGuard {
        bool* flag; bool saved;
        explicit ReturnFlagGuard(bool* f) : flag(f), saved(*f) {}
        ~ReturnFlagGuard() { *flag = saved; }
    };

    // One structured region (branch, loop, switch case group) for C++ reference-member facts.
    struct CxxFactRegionScope {
        MainListener& listener;
        llvm::Function* function;
        CxxFactRegionScope(MainListener& l, bool loop, bool switchCase = false, bool hasDefault = false)
            : listener(l), function(l.compilerLLVM != nullptr ? l.compilerLLVM->currentFunction : nullptr)
        {
            listener.PushCxxFactRegion(function, loop, switchCase, hasDefault);
        }
        // Close() runs a loop's deferred return checks, which may report (and so throw); the
        // destructor only pops.
        void Close()
        {
            llvm::Function* closing = function;
            function = nullptr;
            if (closing != nullptr) listener.PopCxxFactRegion(closing, true, true);
        }
        ~CxxFactRegionScope()
        {
            if (function != nullptr)
                listener.PopCxxFactRegion(function, std::uncaught_exceptions() == 0, false);
        }
        CxxFactRegionScope(const CxxFactRegionScope&) = delete;
        CxxFactRegionScope& operator=(const CxxFactRegionScope&) = delete;
    };

    struct OwnershipControlScope {
        LLVMBackend* compiler;
        explicit OwnershipControlScope(LLVMBackend* c, uint8_t frameKind = 0) : compiler(c)
        {
            compiler->EnterOwnershipControlRegion(frameKind);
        }
        ~OwnershipControlScope() { compiler->LeaveOwnershipControlRegion(); }
        OwnershipControlScope(const OwnershipControlScope&) = delete;
        OwnershipControlScope& operator=(const OwnershipControlScope&) = delete;
    };

    struct LambdaExpectedTypeRestoreGuard {
        LLVMBackend::TypeAndValue* value;
        LLVMBackend::TypeAndValue saved;
        explicit LambdaExpectedTypeRestoreGuard(LLVMBackend::TypeAndValue* v) : value(v), saved(*v) {}
        ~LambdaExpectedTypeRestoreGuard() { *value = saved; }
    };

    // Scopes declExpectedType to one destination position - a declarator, assignment target,
    // field, return type, or matched call parameter. Function signatures are opt-in at bind sites.
    struct DeclExpectedTypeScope {
        LLVMBackend::TypeAndValue* slot;
        LLVMBackend::TypeAndValue saved;
        DeclExpectedTypeScope(LLVMBackend::TypeAndValue* s, const LLVMBackend::TypeAndValue& dest,
                             bool includeFunctionPointer = false)
            : slot(s), saved(*s)
        {
            *slot = {};
            const bool hasDestination = (!dest.TypeName.empty() && dest.TypeName != "auto")
                || (dest.IsFunctionPointer && !dest.FuncPtrReturnTypeName.empty());
            if ((!dest.IsFunctionPointer || includeFunctionPointer) && hasDestination)
                *slot = dest;
        }
        ~DeclExpectedTypeScope() { *slot = saved; }
    };


    // Marks an expression position where a ternary owning temp may need per-arm cleanup
    // before its value is joined with a borrowed consumer.
    // Refuses a construct once the thread's C stack is nearly used up (see kMinStackHeadroomBytes).
    void RequireStackHeadroom(antlr4::ParserRuleContext* ctx)
    {
        if (StackBytesRemaining() >= kMinStackHeadroomBytes) return;
        LogErrorContext(ctx, "expression or statement is nested too deeply; split it into named temporaries");
    }
    bool inCallArgument_ = false;
    const CFlatParser::PostfixExpressionContext* directViewCallArgument_ = nullptr;
    bool inCxxCallArgument_ = false;
    LLVMBackend::TypeAndValue cxxCallArgumentExpectedType_;
    llvm::Function* cxxTernaryCallSink_ = nullptr;
    llvm::Value* cxxTernaryCallSinkEnvironment_ = nullptr;
    bool cxxTernaryCallSinkTakesReference_ = false;
    LLVMBackend::TypeAndValue cxxTernaryCallSinkReturnType_;
    bool cxxTernaryCallPrefixIncompatible_ = false;
    llvm::Value* cxxTernaryCallResult_ = nullptr;
    LLVMBackend::TypeAndValue cxxTernaryCallResultType_;
    bool cxxTernaryCallConsumed_ = false;
    CFlatParser::AssignmentExpressionContext* cxxDirectConstructionArgument_ = nullptr;
    LLVMBackend::TypeAndValue cxxDirectConstructionTargetType_;
    bool cxxDirectConstructionCanDefer_ = true;
    bool cxxDirectConstructionHasLaterSibling_ = false;
    bool* cxxDirectConstructionCallOrderSafe_ = nullptr;
    // The one postfix expression whose flexible-array member stays an array (the direct operand
    // of `sizeof` or unary `&`); every other value context decays it to `T*`, as C does.
    std::vector<antlr4::ParserRuleContext*> flexibleArrayKeep_;
    int ternaryCallArgumentDepth_ = 0;
    // Set by an `auto` declaration whose whole initializer is a `?:`; the top conditional takes
    // it (and clears it) before any operand is parsed, so no nested expression sees it.
    bool autoDeclTernaryInit_ = false;
    bool autoDeclTernaryActive_ = false;
    // The whole `move x` initializer of an `auto` local / operand of a register return: a
    // trivially copyable C++ class there is built through clang's pick for std::move(x).
    CFlatParser::MoveExpressionContext* trivialCxxMoveSite_ = nullptr;
    CFlatParser::MoveExpressionContext* loweredBraceMoveSite_ = nullptr;
    std::string loweredBraceMoveTypeName_;
    llvm::Value* loweredBraceMoveDestination_ = nullptr;
    struct LoweredBraceMoveScope {
        MainListener& listener;
        CFlatParser::MoveExpressionContext* savedSite;
        std::string savedTypeName;
        llvm::Value* savedDestination;
        LoweredBraceMoveScope(MainListener& owner, CFlatParser::MoveExpressionContext* site,
                              const std::string& typeName, llvm::Value* destination)
            : listener(owner), savedSite(owner.loweredBraceMoveSite_),
              savedTypeName(std::move(owner.loweredBraceMoveTypeName_)),
              savedDestination(owner.loweredBraceMoveDestination_)
        {
            listener.loweredBraceMoveSite_ = site;
            listener.loweredBraceMoveTypeName_ = typeName;
            listener.loweredBraceMoveDestination_ = destination;
        }
        ~LoweredBraceMoveScope()
        {
            listener.loweredBraceMoveSite_ = savedSite;
            listener.loweredBraceMoveTypeName_ = std::move(savedTypeName);
            listener.loweredBraceMoveDestination_ = savedDestination;
        }
        LoweredBraceMoveScope(const LoweredBraceMoveScope&) = delete;
        LoweredBraceMoveScope& operator=(const LoweredBraceMoveScope&) = delete;
    };
    // T258: a plain field (not pointer, alias, view, or array) of a lowered CFlat struct type. A
    // `move` of such a field's initializer is move-constructed member-wise into the slot.
    static bool IsLoweredMoveFieldTarget(LLVMBackend* compiler,
                                         const LLVMBackend::DeclTypeAndValue& field)
    {
        return !field.Pointer && !field.ElemPointer && !field.IsAlias && !field.IsArrayView
            && field.ConstArraySize == 0 && compiler->IsLoweredCFlatOnlyStruct(field.TypeName);
    }
    // Builds the deferred `move` source (see LoweredBraceMoveScope) into a field slot member-wise.
    // False when rightNV is not that move; the caller then runs its normal store path.
    bool EmitDeferredLoweredMoveIntoField(llvm::Value* slot, const std::string& typeName,
                                          const LLVMBackend::NamedVariable& rightNV,
                                          CFlatParser::MoveExpressionContext* moveSite,
                                          bool destinationIsFresh,
                                          const LLVMBackend::OwnedTempMark& ownedTempMark);
    struct CallArgumentScope {
        bool& slot;
        int& ternaryDepth;
        bool saved;
        int savedTernaryDepth;
        explicit CallArgumentScope(bool& s, int& d)
            : slot(s), ternaryDepth(d), saved(s), savedTernaryDepth(d) {
            slot = true;
            ternaryDepth = 0;
        }
        ~CallArgumentScope() { ternaryDepth = savedTernaryDepth; slot = saved; }
    };

    // A nested function body is NOT part of the enclosing expression: a ternary declared inside
    // a lambda owns its temp, so the arm registration must not fire.
    struct CallArgumentSuspendScope {
        bool& slot;
        int& ternaryDepth;
        bool saved;
        int savedTernaryDepth;
        CallArgumentSuspendScope(bool& s, int& d)
            : slot(s), ternaryDepth(d), saved(s), savedTernaryDepth(d) {
            slot = false;
            ternaryDepth = 0;
        }
        ~CallArgumentSuspendScope() { ternaryDepth = savedTernaryDepth; slot = saved; }
    };
    struct CxxCallArgumentScope {
        bool& slot;
        bool saved;
        CxxCallArgumentScope(bool& s, bool active) : slot(s), saved(s) { slot = active; }
        ~CxxCallArgumentScope() { slot = saved; }
    };
    struct CxxCallArgumentExpectedTypeScope {
        LLVMBackend::TypeAndValue& slot;
        LLVMBackend::TypeAndValue saved;
        CxxCallArgumentExpectedTypeScope(LLVMBackend::TypeAndValue& s,
                                         LLVMBackend::TypeAndValue active)
            : slot(s), saved(s) { slot = std::move(active); }
        ~CxxCallArgumentExpectedTypeScope() { slot = std::move(saved); }
    };
    struct CxxCallArgumentParseStateScope {
        LLVMBackend* backend;
        llvm::Value* savedRetTemp;
        llvm::Value* savedRetValue;
        llvm::Value* savedSretDest;
        std::string savedSretType;
        bool savedSretForFixedArray;
        explicit CxxCallArgumentParseStateScope(LLVMBackend* b)
            : backend(b), savedRetTemp(b->lastCxxRetTemp_), savedRetValue(b->lastCxxRetValue_),
              savedSretDest(b->pendingCxxSretDest_), savedSretType(b->pendingCxxSretTypeName_),
              savedSretForFixedArray(b->pendingCxxSretForFixedArray_)
        {
            backend->lastCxxRetTemp_ = nullptr;
            backend->lastCxxRetValue_ = nullptr;
            backend->pendingCxxSretDest_ = nullptr;
            backend->pendingCxxSretTypeName_.clear();
            backend->pendingCxxSretForFixedArray_ = false;
        }
        ~CxxCallArgumentParseStateScope()
        {
            backend->lastCxxRetTemp_ = savedRetTemp;
            backend->lastCxxRetValue_ = savedRetValue;
            backend->pendingCxxSretDest_ = savedSretDest;
            backend->pendingCxxSretTypeName_ = std::move(savedSretType);
            backend->pendingCxxSretForFixedArray_ = savedSretForFixedArray;
        }
    };

    // A destination type describes the WHOLE expression, never one operand of it: `string s =
    // "n=" + (() => 2)()` must not type the literal 'string' (measured: module verification
    // failure). Every binary/ternary level withdraws the context unless it is a pure pass-through.
    struct DeclExpectedTypeGate {
        LLVMBackend::TypeAndValue* slot;
        LLVMBackend::TypeAndValue saved;
        bool armed;
        DeclExpectedTypeGate(LLVMBackend::TypeAndValue* s, bool passthrough)
            : slot(s), armed(!passthrough)
        {
            if (armed) { saved = *slot; *slot = {}; }
        }
        ~DeclExpectedTypeGate() { if (armed) *slot = saved; }
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
    // A loop that may run zero times (mayRunZero) also exits from its pre-loop state, so a
    // re-init in the body does not revive a field for the zero-trip path.
    struct LoopMovedStateGuard {
        MainListener* self; LLVMBackend* compiler;
        bool mayRunZero; LLVMBackend::MovedStateSnapshot preLoop;
        LoopMovedStateGuard(MainListener* s, LLVMBackend* c, bool zeroTrip, bool alwaysTrue)
            : self(s), compiler(c), mayRunZero(zeroTrip)
            {
                self->loopBreakMovedStates_.emplace_back(); self->loopConditionAlwaysTrue_.push_back(alwaysTrue);
                if (mayRunZero) preLoop = compiler->SaveMovedState();
            }
        ~LoopMovedStateGuard() {
            if (mayRunZero) compiler->MergeMovedStateInto(preLoop);
            for (const auto& snap : self->loopBreakMovedStates_.back())
                compiler->MergeMovedStateInto(snap);
            self->loopBreakMovedStates_.pop_back(); self->loopConditionAlwaysTrue_.pop_back();
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
    std::string ResolveTypeArgEntry(CFlatParser::TypeParameterEntryContext* entry,
                                    bool pointerOnly = false,
                                    bool* outUnresolvedTypeArgument = nullptr);
    LLVM_ATTRIBUTE_NOINLINE void ResolveDefaultedCxxAliasTypeArgument(
        CFlatParser::TypeParameterEntryContext* entry, std::string& typeName);
    LLVM_ATTRIBUTE_NOINLINE int ResolveDefaultedCxxAliasQualifier(
        antlr4::ParserRuleContext* ctx, const std::string& alias,
        std::string& namespaceName, std::string& primaryName);
    // `struct G<const T>`: a template parameter is a bare name, never const-qualified (#14).
    void RefuseConstTemplateParameters(CFlatParser::GenericTypeParametersContext* params)
    {
        if (params == nullptr || params->typeParameterList() == nullptr) return;
        for (auto* entry : params->typeParameterList()->typeParameterEntry())
            if (entry->Const() != nullptr)
                LogErrorContext(entry, "'const' is not allowed on a generic type argument; "
                    "use it on the pointee ('const T*') or on the declaration");
    }
    // Pointee const depth of the argument a type parameter spelled at `ctx` is bound to (0: none).
    int SubstitutedConstDepth(CFlatParser::TypeSpecifierContext* ctx,
                              CFlatParser::MultiWordTypeSuffixContext* suffix);
    // A function-type parameter spelled `T` with T bound to a fully pointee-const argument
    // (`(const char)*`) keeps it as IsPointeeConst, the one const a FuncPtrParam carries.
    void MarkSubstitutedPointeeConst(LLVMBackend::TypeAndValue::FuncPtrParam& p,
        CFlatParser::TypeSpecifierContext* ctx, CFlatParser::MultiWordTypeSuffixContext* suffix,
        int writtenStars)
    {
        if (writtenStars == 0 && p.Pointer
            && SubstitutedConstDepth(ctx, suffix) >= std::max(p.PointerDepth, 1))
            p.IsPointeeConst = true;
    }
    std::string ResolveTypeArgEntryRaw(CFlatParser::TypeParameterEntryContext* entry,
                                       bool pointerOnly = false,
                                       bool* outUnresolvedTypeArgument = nullptr);

    // Queue a generic instantiation for a known template (dedup via instantiatedGenerics).
    // For struct/class templates the struct shell + default-ctor declaration are created
    // immediately so the mangled type name resolves even before the body is emitted;
    // interface templates are instantiated later by ProcessPendingInstantiations.
    // No-op for unknown base names (e.g. an unresolved type parameter like "T").
    // pointerOnly: the use is spelled `T<..>*`; a later by-value use promotes the record.
    void QueueGenericInstantiation(const std::string& baseName,
                                   const std::vector<std::string>& typeArgs,
                                   const std::string& mangledName,
                                   antlr4::ParserRuleContext* site = nullptr,
                                   bool pointerOnly = false);

    // Append a pending instantiation, stamping the USER-side origin: the argument site in the
    // file being walked, or the outer record's origin when queued from inside a template body.
    void QueuePendingInstantiation(const std::string& templateName,
                                   const std::vector<std::string>& typeArgs,
                                   const std::string& mangledName,
                                   antlr4::ParserRuleContext* site = nullptr,
                                   bool pointerOnly = false);

    // True for a type use spelled as a plain pointer, `T<..>*` (declarationSpecifier with a pointer,
    // no array suffix, no '?'). Such a use names the instance; it does not need the instance's body.
    static bool IsPointerOnlyTypeUse(antlr4::tree::ParseTree* typeSpecNode);
    // A pointer-only record waits (is not drained) while a struct argument is a CFlat struct that
    // is declared but not yet defined. Its body would otherwise name sizeof(arg) on an opaque shell.
    bool PendingWaitsOnStructBody(const GenericTemplateState::PendingInstantiation& pending);
    void PrepareDeferredCoreUniqueLayout(const GenericTemplateState::PendingInstantiation& pending);
    // A by-value use of an already-queued instance: the pending record's body is needed now.
    void PromotePointerOnlyInstantiation(const std::string& mangledName);
    // A member is named through a receiver of this instance: a waiting record drains now.
    void DrainWaitingPointerInstance(const std::string& mangledName);
    std::function<void(const std::string&)> savedDrainWaitingInstance_;

    // Resolve a type in a function-pointer signature position (return or param) the way an ordinary
    // type position resolves: apply active substitutions, and if it names a generic instantiation
    // (e.g. list<string>) mangle it + queue the instantiation. A nested closure encodes recursively.
    // Trailing '*'s from a substitution contribute to the exact signature depth. Non-generic scalar types stay
    // byte-identical to the pre-existing raw-getText() behavior (no alias resolution added).
    std::string ResolveSigComponentCodegen(CFlatParser::TypeSpecifierContext* ts, bool& outPointer,
        int& stars, CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr);
    // Resolve a function signature component; pointer aliases become a base name plus depth.
    // This is the main pass twin of the scanner copy.
    std::string ResolveSigComponentConstCodegen(CFlatParser::TypeSpecifierContext* ts, bool& outPointer,
        int& stars, uint32_t& constMask, CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr,
        bool preserveAliasReferentConst = false, std::string* aliasSpelling = nullptr);

    /*
     * The declaring-scope-resolved key for a signature component, recorded ALONGSIDE the raw
     * spelling (which still owns the mangled name). Empty = the spelling named no registered
     * type in this scope -> no proof, and the comparison falls back to the spelling itself.
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
     * instantiation arrives mangled ("list$int"), so each one goes through SpellType.
     */
    static std::string ClosureArgSpelling(LLVMBackend* compiler, const std::string& encodedName);

    // Substitution-path twin of RejectFatClosurePointerArg: fires only when `baseName` is a
    // REGISTERED FAT closure type, so every non-closure type argument is left untouched.
    void RejectFatEncodedClosurePointerArg(antlr4::ParserRuleContext* ctx, const std::string& baseName);

    // Encode a closure type (Lambda<...>/function<...>) that appears as a generic argument or in a
    // nested signature position into a symbol-safe name (BuildEncodedClosureName), resolving its
    // signature component types, and register the call descriptor. Returns the encoded name.
    void ValidateConstAfterPointerStar(CFlatParser::PointerContext* pointer,
                                       const std::string& spelledBaseType);
    std::string EncodeClosureCodegen(CFlatParser::FunctionPointerSpecifierContext* fpSpec);
    std::string EncodePlainFunctionTypeCodegen(CFlatParser::FunctionTypeArgumentContext* fnSpec);

    // Encode + register a closure type from an already-resolved signature (used when a function-type
    // alias `using IntFn = Lambda<int(int)>` appears as a generic arg, so it unifies with the direct
    // spelling). Declared public below - the forward scan calls it too.

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
    // Byte count of a `= embed("path")` initializer, read from the initializer's SYNTAX so a
    // declaration can size its storage before the expression is walked. nullopt when the
    // initializer is not a direct embed, or the asset cannot be read (the walk reports that).
    std::optional<uint64_t> DirectEmbedByteCount(CFlatParser::AssignmentExpressionContext* expr);

    // `new T[n]` desugar into the core `array<T>`: a decl-init, direct assignment or return
    // whose destination is `array<T>` and whose RHS is a DIRECT array `new` arms this one-shot
    // slot; ParseNewExpression consumes it for that exact node and yields the array value.
    CFlatParser::NewExpressionContext* arrayNewDesugarCtx = nullptr;
    LLVMBackend::TypeAndValue arrayNewDesugarTarget;
    void ArmArrayNewDesugar(antlr4::tree::ParseTree* rhs, const LLVMBackend::TypeAndValue& target);

    LLVMBackend::DeclTypeAndValue getFunctionReturnType(CFlatParser::FunctionDefinitionContext* ctx);

    // Returns the default value for a type:
    //   - struct types (local scope): calls the default constructor.
    //   - struct types (global scope): the constant value of that same construction, if it has one.
    //   - everything else: zero-initializes.
    llvm::Value* GenerateDefaultValue(const LLVMBackend::DeclTypeAndValue& typeValue,
        antlr4::ParserRuleContext* expressionContext = nullptr);
    // Out-of-line so a recursive assignment frame does not grow by the DeclTypeAndValue copy.
    LLVM_ATTRIBUTE_NOINLINE llvm::Value* DefaultValueOfType(const LLVMBackend::TypeAndValue& type,
        antlr4::ParserRuleContext* expressionContext);
    bool EmitNontrivialCxxDefaultAt(llvm::Value* destination,
                                   const LLVMBackend::DeclTypeAndValue& typeValue);
    // Default-constructs CFlat struct typeName. When its constructor returns via C++ sret (it has
    // a C++ field) the object is built directly in `destination` and nullptr comes back; otherwise
    // the returned value, which the caller stores. Never bit-copies a self-referential member.
    llvm::Value* EmitStructDefaultInto(llvm::Value* destination, const std::string& typeName);

    // Constant value of a type's default construction, or nullptr when it is not constant.
    llvm::Constant* TryFoldGlobalDefaultConstruction(const LLVMBackend::DeclTypeAndValue& typeValue);
    // True when the default construction only writes zeros (the zero image already holds it).
    bool DefaultConstructionIsZero(const LLVMBackend::DeclTypeAndValue& typeValue);
    llvm::Constant* TryFoldGlobalDefaultConstruction(
        const LLVMBackend::DeclTypeAndValue& typeValue, FoldConstructedValueState& foldState);
    // Declared-shape default of a value (a global or a native struct field); null when not foldable.
    // expr is the field's `= <expr>` (null = default-constructed).
    llvm::Constant* StructuralDefaultConstant(const LLVMBackend::DeclTypeAndValue& typeValue,
        const std::string& ownerName, CFlatParser::AssignmentExpressionContext* expr,
        FoldConstructedValueState& foldState, unsigned depth);
    llvm::Constant* EvaluateFieldDefaultConstant(const std::string& ownerName,
        const LLVMBackend::DeclTypeAndValue& typeValue, CFlatParser::AssignmentExpressionContext* expr,
        FoldConstructedValueState& foldState);
    bool FieldDefaultSpelling(const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::AssignmentExpressionContext*& expr);
    llvm::Constant* SplatConstantOverFixedArray(llvm::Constant* elemConst, llvm::Type* arrType);

public:
    // Encode + register a closure type from an already-resolved signature (a function-type alias
    // `using IntFn = Lambda<int(int)>` as a generic arg). BOTH passes call this, so an aliased
    // closure gets one name; produces what BuildEncodedClosureName gives for the direct form.
    static std::string EncodeClosureFromSig(LLVMBackend* compiler, const LLVMBackend::TypeAndValue& sig);

    MainListener(CFlatParser* parser, LLVMBackend* compilerLLVM, const std::string& filename);
    // Restores the drain hook the constructor replaced, so no touch reaches a dead listener.
    ~MainListener() override;

    // Retry global default folds after all declarations and deferred instantiations are emitted.
    // Returns the native constructions that did not fold (run by the emit step below).
    std::vector<PendingGlobalDefaultConstruction> ResolvePendingGlobalDefaultConstructions();

    // Emit ONE module initializer running this file's runtime global constructions - the native
    // ones handed in and the C++ ones queued here - merged in declaration order (Sequence). No
    // matching global_dtors: globals follow the Rust rule.
    void EmitPendingGlobalCxxConstructions(
        const std::vector<PendingGlobalDefaultConstruction>& nativeRuntime);

    void SetImportNamespace(const std::string& ns);

    void PreparseAggregateDefinitions(
        CFlatParser::TranslationUnitContext* ctx,
        const std::vector<CFlatParser::FunctionDefinitionContext*>& deferredFunctions = {});
    void PrepareDeferredFunctionDefinitions(
        const std::vector<CFlatParser::FunctionDefinitionContext*>& functions);

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
                                     const std::unordered_map<std::string, std::vector<std::string>>& packSubstitutions = {},
                                     const std::unordered_map<std::string, std::string>& valueSubstitutions = {});

    /*
     * Parks enclosing generic value macros and binds this instantiation's values in their place.
     * Restore() is idempotent; each caller restores where it restores the substitution maps.
     */
    struct GenericValueMacroScope
    {
        LLVMBackend* compiler = nullptr;
        std::vector<std::pair<std::string, LLVMBackend::CompileTimeMacro>> shadowed;
        std::vector<std::string> introduced;
        std::unordered_set<std::string> touched;

        explicit GenericValueMacroScope(LLVMBackend* c) : compiler(c) {}
        ~GenericValueMacroScope() { Restore(); }

        void Record(const std::string& name)
        {
            if (!touched.insert(name).second) return;
            auto it = compiler->compileTimeMacros.find(name);
            if (it != compiler->compileTimeMacros.end()) shadowed.push_back({ name, it->second });
            else introduced.push_back(name);
        }

        void Park(const std::string& name)
        {
            Record(name);
            compiler->compileTimeMacros.erase(name);
        }

        void Park(const std::unordered_map<std::string, std::string>& values)
        {
            for (const auto& entry : values) Park(entry.first);
        }

        void Bind(const std::string& name, int64_t value)
        {
            Record(name);
            auto* intType = llvm::Type::getInt32Ty(*compiler->context);
            compiler->SetCompileTimeMacro(name,
                llvm::ConstantInt::get(intType, (uint64_t)value, true), "int");
        }

        void Restore()
        {
            for (const auto& name : introduced) compiler->compileTimeMacros.erase(name);
            for (const auto& [name, prior] : shadowed) compiler->compileTimeMacros[name] = prior;
            introduced.clear();
            shadowed.clear();
            touched.clear();
        }
    };

    bool ValidateGenericArgumentKinds(const std::string& templateName,
        const std::vector<std::string>& typeParams, const std::vector<std::string>& valueParams,
        const std::vector<std::string>& typeArgs);

    // Owner struct of a generic template keyed "Owner.method", or "" when the template is a
    // free function or a static member (a static has no receiver, so it stays free-shaped).
    std::string GenericMethodOwner(const std::string& baseName, CFlatParser::FunctionDefinitionContext* tmplCtx);
    // Declaring aggregate of a generic member template, including static templates.
    std::string GenericMemberTemplateOwner(const std::string& baseName,
        CFlatParser::FunctionDefinitionContext* tmplCtx);
    void BindGenericOwnerSubstitutions(const std::string& owner, GenericValueMacroScope& valueMacros);

    // Template key for a generic method called on a receiver ("Holder" + "get" -> "Holder.get"),
    // or "" when the receiver's type declares no such template.
    std::string GenericMethodTemplateKey(const std::string& receiverType, const std::string& methodName);
    std::string LexicalAggregateMemberOwner(LLVMBackend* compiler,
        const std::vector<EnclosingAggregateLookupScope>& aggregateScopes, const std::string& memberName,
        const std::string& preferredOwner, antlr4::ParserRuleContext* callContext);

    // True when the child right after `child` is a '.' - i.e. the primary is used as a member
    // qualifier (`Holder<i64>.sget(...)`) rather than being called or indexed.
    bool IsFollowedByDot(CFlatParser::PostfixExpressionContext* ctx, antlr4::tree::ParseTree* child);
    // True when the child right after `child` is a '(' - the primary is being called.
    bool IsFollowedByCall(CFlatParser::PostfixExpressionContext* ctx, antlr4::tree::ParseTree* child);

    // Instantiate a generic function template with concrete type arguments.
    // Returns the mangled name of the instantiated function, or empty string on failure.
    std::string InstantiateGenericFunction(const std::string& baseName,
        const std::vector<std::string>& typeArgs, size_t candidateIndex = 0);

    // Infer the type arguments for a generic function template from the receiver's
    // interface type name (e.g. "Container__int" -> T="int") and instantiate it.
    // Returns the mangled name of the instantiated function, or empty string on failure.
    std::string InferAndInstantiateGenericFunction(const std::string& funcName, const std::string& receiverType);

    // Infer type arguments from call arguments; optionally return substituted direct-T parameter types.
    // Set instantiate=false to query those types without generating the generic function body.
    std::string TryInferAndInstantiateFromArgs(const std::string& funcName,
                                               const std::vector<LLVMBackend::NamedVariable>& args,
                                               CFlatParser::ArgumentExpressionListContext* argList = nullptr,
                                               std::vector<std::string>* deducedParameterTypes = nullptr,
                                               bool instantiate = true,
                                               std::string* refusal = nullptr);
    std::string TryInferAndInstantiateFromArgsCandidate(const std::string& funcName,
        size_t candidateIndex, const std::vector<LLVMBackend::NamedVariable>& args,
        CFlatParser::ArgumentExpressionListContext* argList,
        std::vector<std::string>* deducedParameterTypes,
        std::vector<std::string>* deducedTypeArgs, bool instantiate,
        std::vector<LLVMBackend::TypeAndValue>* deducedParameterValues = nullptr,
        bool rejectDeductionConflicts = false);
    std::string TryInferAndInstantiateFromFuncPtrCandidate(const std::string& funcName,
        size_t candidateIndex, const LLVMBackend::TypeAndValue& destSig,
        std::string& refusal);

    // Infer a bare function template's arguments from a function-pointer destination.
    std::string TryInferAndInstantiateFromFuncPtr(const std::string& funcName,
        const LLVMBackend::TypeAndValue& destSig, std::string& refusal,
        const std::vector<size_t>* allowedCandidateIndices = nullptr);

    std::vector<size_t> DeduceGenericFunctionCallbackCandidates(
        const std::string& funcName,
        const std::vector<LLVMBackend::TypeAndValue>& callbackCandidates);

    // Build the function-pointer TypeAndValue for a `function<R(Args)>` spec (the resolved form a
    // `using Cb = function<R(Args)>;` alias stores). Duplicated from the ForwardRefScanner copy -
    // the two passes are separate classes, mirroring the duplicated ParseDeclarationSpecifiers.
    LLVMBackend::TypeAndValue BuildFuncPtrAliasType(CFlatParser::FunctionPointerSpecifierContext* fpSpec);

    bool RegisterGenericAlias(const std::string& alias, CFlatParser::TypeSpecifierContext* typeSpec,
        const std::string& parsedSpelling, const std::string& suffix, uint32_t constMask,
        bool genericDerived, bool pointerOnly, int displayPointerDepth, antlr4::ParserRuleContext* ctx,
        CFlatParser::GenericTypeParametersContext* genericCtx, bool emitSymbol,
        bool isTypedef = false);
    void ParseUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx);
    // `Spec<args>.name[.name...]` in type position: instantiates the owner and returns the
    // member type's spelling; reports a name that is not a member type of the owner.
    std::string ResolveMemberTypeSpecifier(CFlatParser::MemberTypeSpecifierContext* ctx,
        uint32_t* constMask = nullptr);
    // The shared walk: instantiate `base<ownerArgs>`, then resolve each member in turn. Returns ""
    // when a member is not a member type (reported only when `report`).
    std::string ResolveMemberTypeChain(
        std::string base, CFlatParser::GenericTypeParametersContext* ownerArgs, const std::string& ownerText,
        const std::vector<std::pair<std::string, antlr4::ParserRuleContext*>>& members,
        antlr4::ParserRuleContext* site, bool report, std::string* lastOwner = nullptr);
    // A sizeof/typeof operand the parser read as an expression (`Spec<args>.name`): the member
    // type it names by lookup, or "" when it names none (it then stays a value expression).
    std::string TryPostfixAsMemberType(CFlatParser::PostfixExpressionContext* postfix);
    // True when `Spec<args>.name` names a member type (no diagnostics; a value member is false).
    bool MemberTypeSpecifierNamesType(CFlatParser::MemberTypeSpecifierContext* ctx);
    // `Owner.name` written as a dotted name (`Plain.Id`, `T.value_type` in a generic body) that
    // the ordinary lookup left unknown. Returns the member type's spelling, "" when `text` does
    // not start with a known record; reports a member that is missing or not a type.
    std::string ResolveDottedMemberType(const std::string& text, antlr4::ParserRuleContext* site, bool report = true);
    // Collects the `using` members of a queued generic struct/class instantiation `owner` under
    // its template substitutions (the backend's ensureAggregateAliases_ hook).
    bool EnsureAggregateAliases(const std::string& owner);
    std::unordered_set<std::string> aggregateAliasesInProgress_;
    // > 0 inside the generic-use pre-scan: member-type misses stay silent there, the real
    // walk reports them inside whatever expect_error scope the spelling sits in.
    int memberTypeSilentDepth_ = 0;
    void RecordAggregateValueMembers(const std::string& owner,
        const std::vector<CFlatParser::AggregateMemberContext*>& members);

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
    std::unordered_set<std::string> explicitNoArgConstructors_;

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

    template <typename TCtx>
    void CollectAggregateAliases(TCtx* ctx, const std::string& aggregateName)
    {
        auto* compiler = Compiler(ctx);
        struct AggregateNameScope
        {
            std::vector<std::string>& scopes;
            AggregateNameScope(std::vector<std::string>& target, const std::string& name)
                : scopes(target) { scopes.push_back(name); }
            ~AggregateNameScope() { scopes.pop_back(); }
        } aggregateNameScope(structScopeStack, aggregateName);
        const auto& members = ResolveAggregateMembers(ctx);
        std::unordered_set<std::string> typeParams;
        if (auto* generic = ctx->genericTypeParameters())
            for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                if (entry->valueParameterDeclaration() == nullptr && entry->typeSpecifier() != nullptr)
                    if (auto* gid = entry->typeSpecifier()->genericIdentifier(); gid != nullptr)
                        if (gid->Identifier() != nullptr && gid->genericTypeParameters() == nullptr)
                            typeParams.insert(gid->Identifier()->getText());

        std::unordered_set<std::string> membersByName;
        for (auto* member : members)
        {
            if (auto* decl = member->declaration())
            {
                if (auto* list = decl->initDeclaratorList())
                    for (auto* init : list->initDeclarator())
                        if (auto* declarator = init->declarator())
                            membersByName.insert(getDirectDeclName(declarator->directDeclarator()));
            }
            else if (auto* func = member->functionDefinition())
                membersByName.insert(getFunctionName(func));
            else if (auto* nested = member->structDefinition())
                membersByName.insert(nested->directDeclarator()->getText());
            else if (auto* nested = member->classDefinition())
                membersByName.insert(nested->directDeclarator()->getText());
        }

        for (auto* member : members)
        {
            auto* usingDecl = member->usingDeclaration();
            if (usingDecl == nullptr) continue;
            std::string alias = usingDecl->String() != nullptr
                ? usingDecl->String()->getText()
                : usingDecl->Identifier()->getText();
            if (typeParams.count(alias) != 0)
                LogErrorContext(usingDecl, compiler->LocalizeMessage(
                    "alias '{}' reuses the name of type parameter '{}' of '{}'",
                    { alias, alias, aggregateName }));
            if (membersByName.count(alias) != 0)
                LogErrorContext(usingDecl, compiler->LocalizeMessage(
                    "alias '{}' collides with member '{}' of '{}'",
                    { alias, alias, aggregateName }));
            ParseUsingDeclaration(usingDecl);
        }
    }

    template <typename TCtx>
    void ValidateGenericAggregateAliasNames(TCtx* ctx, const std::string& aggregateName)
    {
        auto* compiler = Compiler(ctx);
        std::unordered_set<std::string> typeParams;
        if (auto* generic = ctx->genericTypeParameters())
            for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                if (entry->valueParameterDeclaration() == nullptr && entry->typeSpecifier() != nullptr)
                    if (auto* gid = entry->typeSpecifier()->genericIdentifier(); gid != nullptr)
                        if (gid->Identifier() != nullptr && gid->genericTypeParameters() == nullptr)
                            typeParams.insert(gid->Identifier()->getText());

        std::function<void(const std::vector<CFlatParser::AggregateMemberContext*>&)> visit;
        std::function<void(CFlatParser::IfConstMemberContext*)> visitIfConst;
        visitIfConst = [&](CFlatParser::IfConstMemberContext* ifConst) {
            for (auto* block : ifConst->ifConstMemberBlock())
                visit(block->aggregateMember());
            if (auto* elseIf = ifConst->ifConstMember())
                visitIfConst(elseIf);
        };
        visit = [&](const std::vector<CFlatParser::AggregateMemberContext*>& members) -> void
        {
            for (auto* member : members)
            {
                if (auto* usingDecl = member->usingDeclaration())
                {
                    std::string alias = usingDecl->String() != nullptr
                        ? usingDecl->String()->getText()
                        : usingDecl->Identifier()->getText();
                    if (typeParams.count(alias) != 0)
                        compiler->LogError(compiler->LocalizeMessage(
                            "alias '{}' reuses the name of type parameter '{}' of '{}'",
                            { alias, alias, aggregateName }));
                    continue;
                }
                if (auto* ifConst = member->ifConstMember())
                    visitIfConst(ifConst);
            }
        };
        visit(ctx->aggregateMember());
    }

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
    void PrepareLoopConditionalDropFlags(CFlatParser::StatementContext* body);
    void ParseControlledBody(CFlatParser::StatementContext* body);
    void MarkLoopBackEdgeCxxConstPointers(
        LLVMBackend* compiler, CFlatParser::IterationStatementContext* loop);

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
                                  const std::string& ifaceName, bool unproven = false);

    // The shared `return <expr>;` lowering, used by the return statement and by an
    // expression-body lambda (`=> expr`, which is `=> { return expr; }`).
    // `defaultValue` is the `return default;` spelling: there is no expression node, so the
    // operand is built by the shared default-value emitter under the return type.
    void EmitReturnExpression(antlr4::ParserRuleContext* errCtx,
                              CFlatParser::AssignmentExpressionContext* assignExpr,
                              const std::string& retText,
                              bool defaultValue = false,
                              CFlatParser::ReturnBraceInitializerContext* braceInitializer = nullptr);
    void CheckCxxReferenceAggregateReturn(
        const LLVMBackend::NamedVariable& value,
        const std::string& returnTypeName,
        antlr4::ParserRuleContext* context);

    void ParseStatement(CFlatParser::StatementContext* statement);
    void ParseStatementBody(CFlatParser::StatementContext* statement);

    void GenerateDefaultParamOverloads(
        const std::string& name,
        const LLVMBackend::DeclTypeAndValue& returnType,
        const std::vector<LLVMBackend::DeclTypeAndValue>& params,
        bool varargs,
        size_t line,
        bool isMethod = false);

    std::string InterfaceDefaultWrapperFunctionName(
        const std::string& interfaceName, const std::string& methodName) const;
    void GenerateInterfaceDefaultParamOverloads(
        const std::string& interfaceName, CFlatParser::InterfaceMethodContext* method);

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
    void ParseFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName = {}, const std::string& namespaceName = {}, const std::string& nameOverride = {}, std::optional<std::string> bodyNamespace = std::nullopt);

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
        // Unwind-partial stack depth before this guard's StaticFlag entry; Close pops back to it.
        size_t UnwindMark = 0;
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

    std::optional<std::string> FoldConstLiteral(
        antlr4::ParserRuleContext* ctx, const std::string& intrinsicName,
        const std::string& typeName, CFlatParser::InitializerListContext* init,
        std::vector<LLVMBackend::ManifestFragment::Leaf>* manifestLeaves = nullptr);

    // Walks the compile-time literal of an `application AppInfo x = { ... };` declaration into
    // the plain data the .res / Info.plist writers consume. Reports and returns false on the
    // first shape or validation failure; host-independent, so --check sees every rule.
    bool FoldApplicationLiteral(antlr4::ParserRuleContext* ctx, const std::string& typeName,
        CFlatParser::InitializerListContext* init, cflat::appres::AppInfoData& out);

    // Classifies the icon list into a PNG set or a single .ico / .icns container, filling
    // IconKind and each image's pixel size. Reports the offending entry index on failure.
    bool ValidateApplicationIcons(antlr4::ParserRuleContext* ctx, cflat::appres::AppInfoData& out);

    void ParseEnumSpecifier(CFlatParser::EnumSpecifierContext* ctx, const std::string& namespaceName = {});

    // Descend a single-child expression chain from `ctx` to a top-level `move` expression, if one
    // is the entire expression. Returns null when any operator (binary, unary, sizeof, cast) sits
    // above the move, since then the move is not the whole RHS.
    static CFlatParser::MoveExpressionContext* TopLevelMoveExpression(antlr4::tree::ParseTree* node);
    // The assignment an initializer is, through a single-child chain and plain parentheses
    // (`T b = a = x;`, `T b = (a += f(x));`), or null. Shape probe only.
    static CFlatParser::AssignmentExpressionContext* SoleAssignmentExpression(
        antlr4::tree::ParseTree* node);
    // The `c ? a : b` an expression is, through a single-child chain and plain parentheses
    // (`return (f ? x : y);`), or null. Shape probe only.
    static CFlatParser::ConditionalExpressionContext* SoleTernaryExpression(
        antlr4::tree::ParseTree* node);
    // Signature of one arm of a bare `function f = c ? a : b;`: a plain local/argument name (its
    // declared function type) or a plain function name with exactly one overload. False otherwise.
    bool ResolveBareFunctionArmSignature(antlr4::ParserRuleContext* arm,
        LLVMBackend::TypeAndValue& out);

    /*
     * M4b - declare a local of a foreign NONTRIVIAL C++ class by CONSTRUCTING INTO ITS SLOT.
     *
     * CFlat's ordinary declaration path materializes an initializer VALUE and stores it, which
     * for a C++ class with a user copy/move constructor or destructor is exactly the byte copy
     * the language forbids. This path allocates the local first and then calls a C++ constructor
     * on that address, so no value of the class ever exists. Returns true when it owned the
     * declaration (including when it reported an error); false leaves the ordinary path in charge.
     *
     * Accepted initializer forms: `T(args)`, `= default`, none at all (default constructor),
     * a T lvalue (copy constructor), `move <T lvalue>` (move constructor, source consumed), and
     * a call returning T by value (constructed straight into the slot through the sret pointer).
     */
    bool HasForeignCxxAggregateReferenceMember(const std::string& ownerTypeName,
        antlr4::ParserRuleContext* context);
    bool HasUninitializedForeignCxxAggregateReferenceMember(const std::string& ownerTypeName,
        antlr4::ParserRuleContext* context);
    bool TryEmitForeignCxxAggregateBraceInit(const std::string& typeName,
        CFlatParser::InitializerListContext* list,
        antlr4::ParserRuleContext* errorContext,
        llvm::Value* slotValue,
        bool copyListInit,
        bool rejectEscapingReferences = false,
        llvm::GlobalVariable* referenceLifetimeOwner = nullptr,
        bool rejectFrameBoundReferences = false);
    bool TryEmitForeignCxxAggregateBraceInitFields(const std::string& typeName,
        const std::vector<CFlatParser::FieldInitContext*>& fields,
        antlr4::ParserRuleContext* errorContext,
        llvm::Value* slotValue,
        bool copyListInit,
        bool rejectEscapingReferences = false,
        llvm::GlobalVariable* referenceLifetimeOwner = nullptr,
        bool rejectFrameBoundReferences = false);
    bool TryDeclareForeignCxxLocal(CFlatParser::InitDeclaratorContext* initDeclarator,
                                   CFlatParser::DirectDeclaratorContext* direct,
                                   const LLVMBackend::DeclTypeAndValue& declType,
                                   const std::string& name, size_t line,
                                   std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList);
    /*
     * `auto x = <lvalue of foreign nontrivial C++ class T>` after the initializer is evaluated:
     * behaves as `T x = <lvalue>` - a slot of T, copy-constructed (move-constructed for `move`,
     * a by-value parameter's last use, or an rvalue reference to a move-only class). False
     * leaves every other source (prvalues keep the sret elision path) to the ordinary path.
     */
    bool TryDeclareForeignCxxAutoLocal(CFlatParser::DirectDeclaratorContext* direct,
                                       CFlatParser::AssignmentExpressionContext* assign,
                                       const LLVMBackend::DeclTypeAndValue& autoType,
                                       LLVMBackend::NamedVariable& sourceNV,
                                       llvm::Value* sourceCxxRetTemp,
                                       bool constQualified,
                                       const std::string& name, size_t line,
                                       std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList);
    // The `T(args)` construction form: the argument list of a postfix call whose callee spelling
    // names the declared foreign class itself. Null for every other expression shape.
    // Not static: the class-template form re-mangles the callee spelling through the backend.
    CFlatParser::ArgumentExpressionListContext* ForeignCxxConstructArgs(
        antlr4::tree::ParseTree* node, const std::string& typeName);

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
    // Nulling the global's storage after DropValue is the q11 point-3 re-initialization: the
    // storage outlives the frame, so leaving a released husk behind would hand the next read (or
    // a second discard of the same global) a freed value. There is no exit-time destructor to
    // coordinate with - q11 point 4 removed it - so this is the ONLY release a global ever gets.
    // Only covers the shapes OwnsDroppableResource recognizes (string, owning value struct); a
    // `unique T*` / unique-interface / closure global is NOT released here.
    void ReleaseOwningGlobalNow(antlr4::ParserRuleContext* ctx, const std::string& name);
    void ReleaseOwningGlobalNow(antlr4::ParserRuleContext* ctx,
                                const LLVMBackend::NamedVariable& global);

    // Recover ownership of a local/temp that was `move`d out of a container element slot. The slot
    // read demoted the element's `unique`-ness (a plain read hands out a borrow), so its TRUE
    // ownership is taken from `elemOwnershipType`: the DECLARED destination type for
    // `T tmp = move _data[i]`. An owning thin
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

    bool IsOwningTempRawPointerFieldEscape(const LLVMBackend::NamedVariable& nv) const;

    void GuardOwningTempRawPointerFieldIntoUnique(
        const LLVMBackend::NamedVariable& source,
        const LLVMBackend::TypeAndValue& destinationType,
        antlr4::ParserRuleContext* ctx,
        const std::string& calleeName);

    void GuardRawPointerIntoOwningTempField(
        const LLVMBackend::NamedVariable& source,
        const LLVMBackend::TypeAndValue& destinationType,
        bool destinationIsLocal, bool destinationIsAlias,
        const std::string& destinationName,
        antlr4::ParserRuleContext* ctx,
        bool calleeArgument = false);

    /*
     * The 2026-08-21 string-from-a-temporary ruling. A `string` (or string-typed field) read out
     * of a TEMPORARY is a BORROW in rvalue position and an IMPLICIT COPY when it is BOUND to
     * something that outlives the full expression (local, assignment, field store, brace-init
     * member, return). This is the one owning type with a cheap, total deep copy, so the sibling
     * rejects - which stay for `unique` pointer fields and other owning value types - are
     * needlessly hostile here. Returns true when it emitted the copy, in which case `value` has
     * been replaced by an OWNED string temp and `nv`'s temp/borrow provenance has been retired so
     * no downstream reject fires and no second copy is emitted.
     */
    bool AdoptImplicitStringTempCopy(LLVMBackend::NamedVariable& nv, llvm::Value*& value,
                                     antlr4::ParserRuleContext* ctx,
                                     bool forceCopy = false);

    /*
     * The whole-ELEMENT sibling of the temp-FIELD case above: `fields.get(0)` / `fields[0]` on a
     * `list<string>` hands back an `alias` view of a buffer the CONTAINER owns, with no field
     * access to mark it. Bound to a local, an assignment target or a return slot it dangles the
     * moment the container is cleared, so it takes the same implicit copy. A NAMED `alias` string
     * local is excluded: that borrow is one the user spelled, and copying it silently would make a
     * second owner.
     */
    static bool IsImplicitCopyableStringTemp(const LLVMBackend::NamedVariable& nv);

    // The DECLARED half of the predicate above, read straight off the NamedVariable the
    // member-access branch built. Also the condition under which the read is ledgered.
    bool DeclaredOwningTempUniqueFieldRead(const LLVMBackend::NamedVariable& nv);

    /*
     * The diagnostic for the predicate above. `move <temp>.<field>` is not a remedy (a temporary
     * has no address), so it names the one that works: bind the whole call result to a local.
     */
    void RejectOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& rightNV,
                                           const std::string& destDesc,
                                           antlr4::ParserRuleContext* ctx);

    /*
     * The single door every destination goes through. Rejects immediately when the ledger already
     * proves the escape, and otherwise records the SITE when the value is a CANDIDATE launder (a
     * callee defined below its call site), for the end-of-module resolve to answer.
     */
    // `storeSite`: a store (not a return) also refuses an address into an owning pointer temp.
    void GuardOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& nv,
                                          const std::string& destDesc,
                                          antlr4::ParserRuleContext* ctx,
                                          bool storeSite = true);

    /*
     * True when a destination field PROVABLY owns a raw pointer whose synthesized destructor
     * frees it, by either spelling: the written `unique` qualifier, or generic substitution of a
     * `unique T*` type argument. Destructor synthesis already ORs the two (LLVMBackend.h ~4614),
     * but the borrow-provenance rejects were keyed on IsUnique alone, so the generic spelling
     * (`Box<unique Item*>::t`) reached that free with no diagnostic at all (silent abort).
     * The type-arg arm mirrors the `uniqueAutoSink` ownership rule (LLVMBackend.h ~3581) and is
     * further restricted to the exact scalar-pointer shape whose free is synthesized, so it can
     * only reject a slot that is provably freed. Everything else is let through: an `alias` type
     * argument and a borrow-of-alias-element never set IsUnique in the first place, and a
     * container's own `T* _data` buffer has it cleared by the explicit-star rule (~3974).
     */
    static bool IsOwningUniquePointerField(const LLVMBackend::TypeAndValue& tv);
    bool IsOwningUniqueField(const LLVMBackend::TypeAndValue& tv) const;

    // A raw `new T[n]` carries delete[] provenance, but a unique field's destructor frees one T.
    // Reject the store before the field adopts the pointer, for both wrapper and builtin spellings.
    void RejectRawHeapArrayIntoUniqueField(
        const LLVMBackend::NamedVariable& source,
        const LLVMBackend::TypeAndValue& fieldType,
        const std::string& fieldName,
        antlr4::ParserRuleContext* ctx);
    void RejectRawHeapArrayIntoUniqueLocal(
        const LLVMBackend::NamedVariable& source,
        const LLVMBackend::TypeAndValue& localType,
        const std::string& localName,
        antlr4::ParserRuleContext* ctx);

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
                                     bool isInit, antlr4::ParserRuleContext* ctx,
                                     const std::string& uniqueFieldViaCall = {});

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
     * RULED 2026-08-10 (uniform implicit move): a plain `=` between two `unique` fields MOVES.
     * Null the source field so the destination is the sole owner. Emitted before the drop-old that
     * releases the destination's previous pointee - that order is what makes a self-assign, which
     * no compile-time proof can rule out for a runtime index, free nothing.
     */
    void EmitImplicitUniqueFieldMove(LLVMBackend::NamedVariable& rightNV, llvm::Value* right,
                                     llvm::Value* destination, const std::string& destIfaceName,
                                     antlr4::ParserRuleContext* ctx);

    /*
     * Reject an allocation-alignment disagreement storing into a struct field. The alignment of an
     * over-aligned `new T[n] alignas(0, N)` is a property of the ALLOCATION, not of the type, so a
     * free through the field can only recover it when the FIELD declares the same clause; a missing
     * or mismatched one frees the block wrong and corrupts the heap. (Alignment carried by the TYPE
     * is not tagged here and escapes freely, as in C++.) Shared by the two field-store paths - `=`
     * in ParseAssignmentExpression and brace-init / `<Tag attr=...>` sugar via EmitOneFieldInit - so
     * neither spelling can reach a mismatched free site. `fieldTV` is the desugared destination
     * type, `fieldAlign` its clause, and `right` the value being stored. Returns true when an error
     * was logged.
     */
    bool RejectFieldAllocAlignMismatch(
        const LLVMBackend::TypeAndValue& fieldTV,
        uint64_t fieldAlign,
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        const std::string& fieldDesc,
        antlr4::ParserRuleContext* ctx,
        bool slotIsAuthoritativeHolder = false,
        const char* destNoun = "field");

    /*
     * The local-reassignment twin of RejectFieldAllocAlignMismatch. A local whose OWN declared type
     * carries `alignas(0, N)` promises every block it ever holds is N-aligned; the declaration path
     * already verifies the initializer, but a later `local = rhs` re-derived the tracked alignment
     * from the RHS with no check, so the clause became a lie. Gated on the DECLARED clause
     * (TypeAndValue.AllocAlignValue) for the clause-bearing arm; the clause-free arms use positive
     * provenance only: an over-aligned RHS into an unclaused local, or a known ordinary RHS into a
     * local with an INFERRED alignment (tracked NamedVariable.AllocAlignment), whose max-merge would
     * keep the stale aligned free site. Null stores and an over-aligned pointee TYPE
     * are exempt for the field path's reasons. Returns true when an error was logged.
     */
    bool RejectLocalAllocAlignMismatch(
        const LLVMBackend::NamedVariable& namedVar,
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
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
     * flag. Returns true when the owning source slot was nulled (ownership left it).
     */
    bool TransferPointerOwnershipOnStore(
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

    // Turn a null-conditional owning read into an independent by-value result. Pointer-borrow
    // call arguments keep the original field address and are materialized by their selected sink.
    void MaterializeNullConditionalOwningValue(
        LLVMBackend::NamedVariable& value,
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
    static bool IsMoveArgumentStorage(LLVMBackend* compiler,
        const LLVMBackend::NamedVariable& nv);

    bool IsProgramLifetimeStorage(const LLVMBackend::NamedVariable& nv) const;
    bool CxxReferenceSourceOutlivesFunction(const LLVMBackend::NamedVariable& nv) const;
    void RecordCxxReferenceMemberProvenance(
        llvm::Value* objectSlot, const std::string& aggregateType,
        const std::string& fieldName, CxxReferenceMemberLifetime lifetime,
        antlr4::ParserRuleContext* context);
    bool CopyCxxReferenceMemberProvenance(llvm::Value* destination, llvm::Value* source);
    static llvm::CallBase* CxxCallResultProducer(llvm::Value* value);
    void RecordCxxCallResultReferenceProvenance(
        llvm::Value* objectSlot, const std::string& aggregateType, llvm::Value* value,
        antlr4::ParserRuleContext* context);
    std::map<std::pair<std::string, std::string>, size_t> CollectCxxReferenceMembers(
        const std::string& resolvedType) const;
    bool CxxArgumentRootedInFrame(llvm::Value* arg, unsigned depth) const;
    bool CxxFactReachesPoint(const std::vector<CxxReferenceMemberProvenance>& facts,
                             size_t index, const CxxFactPoint& at) const;
    CxxFactPoint CurrentCxxFactPoint() const;
    void PushCxxFactRegion(llvm::Function* function, bool loop, bool switchCase, bool hasDefault);
    void PopCxxFactRegion(llvm::Function* function, bool runDeferred, bool runChecks);
    void RenewCxxFactSwitchCase();
    void NoteCxxFactJump(bool isContinue);
    void MarkCxxFactLoopEntersBody(bool continueExits);
    bool TakeCxxFactBranch(CxxFactGroup& group, size_t depth) const;
    void JoinCxxFactKills(const std::vector<CxxFactGroup>& groups, uint64_t outerStart);
    bool InsideCxxFactLoop() const;
    void DeferCxxFactQuery(bool check, std::function<bool(bool& done)> run);
    const CxxReferenceMemberProvenance* CxxLiveFrameFact(
        llvm::Value* root, const std::vector<int64_t>& path, const CxxFactPoint& at) const;
    bool CxxStorageHasProvenance(llvm::Value* storage) const;
    bool CxxStorageFrameBound(llvm::Value* storage, const CxxFactPoint& at) const;
    bool CxxValueFrameBound(llvm::Value* value, const CxxFactPoint& at, unsigned depth) const;
    bool CxxCallResultFrameBound(llvm::CallBase* call, const CxxFactPoint& at,
                                 unsigned depth) const;
    static llvm::Value* CxxUniqueStoredValue(llvm::Value* storage);
    bool CxxStorageValueFrameBound(llvm::Value* storage, const CxxFactPoint& at,
                                   unsigned depth) const;
    static bool PointsIntoStackFrame(llvm::Value* value);
    static bool IsFrameOwnedBorrowingMemberCall(const LLVMBackend* compiler, llvm::Value* value);
    void RejectLambdaReferenceCaptureEscape(
        bool destinationHasProgramLifetime,
        const LLVMBackend::NamedVariable& source,
        antlr4::ParserRuleContext* ctx);

    /*
     * Stamp the unique-field borrow provenance onto a call RESULT that was never bound to a local.
     * The declaration and '=' paths classify while BUILDING a binding, so a temporary RHS
     * (`o = w->get();`, `s.q = w->get();`, `delete w->get();`) carried no borrow facts and every
     * door saw an ordinary pointer. Called on the consumer's OWN copy of the NamedVariable, so
     * nothing outside that statement changes. Unknown accepts: no ledger entry, no stamp; an
     * already-borrowed or `move` source is left exactly as it was.
     */
    static void ApplyCallResultBorrowProvenance(LLVMBackend* compiler,
                                                LLVMBackend::NamedVariable& nv);
    static bool AliasResultMayBorrowOwnedTemp(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& nv,
        const LLVMBackend::OwnedTempMark& mark);

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
    static bool DestinationIsAliasBorrowLocal(const LLVMBackend::NamedVariable& destination);

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
    static void RetireAliasBorrowOnRebind(LLVMBackend* compiler,
        const LLVMBackend::NamedVariable& destination, llvm::BasicBlock* assignmentBlock);

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

    // Name a global destination for the allocation-alignment diagnostic. A global store leaves
    // CallerName empty on some paths, so fall back to the declarator's recorded name.
    static std::string GlobalSlotDescription(const LLVMBackend::NamedVariable& nv)
    {
        const std::string& name = !nv.CallerName.empty()
            ? nv.CallerName : nv.TypeAndValue.VariableName;
        return name.empty() ? std::string("a global slot") : std::format("global '{}'", name);
    }

    /*
     * The POINTER sibling of RejectMixedOwnershipTernaryJoin. One arm PROVABLY owns a fresh block
     * (`new`, a `move` of an owner, a move-returning call) while the other is a PROVEN borrow, so
     * the join owns nothing the receiver may free and the owning arm leaks whenever it is taken
     * (measured: 16 bytes, no destructor, on every destination - declaration, assignment, field
     * store, call argument, return). Both sides must be proven: an arm whose ownership is merely
     * unrecorded (a plain `T* p = new T();` local read back by name) is NOT the borrow side.
     */
    bool RejectMixedOwnershipPointerTernaryJoin(
        llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined,
        llvm::Value* trueStorage, llvm::Value* falseStorage,
        antlr4::ParserRuleContext* trueCtx, antlr4::ParserRuleContext* falseCtx,
        antlr4::ParserRuleContext* ctx, std::string* deferredDiagnostic = nullptr);

    // Shared declaration/assignment predicate for borrowing a named owning pointer source.
    bool ShouldBorrowPlainPointerBinding(
        const LLVMBackend::TypeAndValue& destination,
        const LLVMBackend::NamedVariable& source,
        const LLVMBackend::NamedVariable* existingDestination,
        bool sourceMovedFromSlot) const;

    /*
     * Is the RHS's allocation alignment PRECISE at a global store? Only a recorded alignment
     * (AllocAlignKnown) or an OWNING fresh/raw-new binding qualifies: new-provenance flags are
     * copied onto plain borrows (`E* b = owner;`) whose alignment is not determinate. A null
     * literal, a proven borrow, a direct `&expr`, a parameter and a call result are unknown,
     * and the clause-bearing direction of the global door must let those through unchallenged.
     */
    static bool GlobalStoreAllocAlignIsPrecise(const LLVMBackend* compiler,
                                               const LLVMBackend::NamedVariable& rightNV,
                                               llvm::Value* right)
    {
        if (right == nullptr || llvm::isa<llvm::ConstantPointerNull>(right)) return false;
        if (rightNV.PointsToBorrowedAddress || rightNV.IsAliasBorrow) return false;
        if (compiler->IsBorrowedAddressValue(right)) return false;
        if (rightNV.AllocAlignKnown) return true;
        return rightNV.IsOwning && (rightNV.TypeAndValue.IsArrayView
            || rightNV.IsNewAllocated || rightNV.AllocatedByRawNewArray);
    }

    /*
     * A '?:' whose two arms disagree on OWNERSHIP of an owning-value struct (one arm an owning
     * value or temporary, the other a borrow of an existing value) has no safe join: adopting it
     * double-frees the borrow arm's owner, borrowing it leaks the owning arm. Reject at the join
     * so every destination (declaration, assignment, field, argument) reports the same thing.
     * Returns true when an error was logged.
     */
    bool RejectMixedOwnershipTernaryJoin(
        llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined,
        antlr4::ParserRuleContext* trueCtx, antlr4::ParserRuleContext* falseCtx,
        antlr4::ParserRuleContext* ctx);
    // The '?:' ownership-join diagnostic, owning arm first. Shared by every refusal of a mixed join.
    void ReportMixedOwnershipTernaryJoin(antlr4::ParserRuleContext* owningCtx,
        antlr4::ParserRuleContext* borrowCtx, antlr4::ParserRuleContext* ctx);

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
    bool NamedVarIsStringView(const LLVMBackend::NamedVariable& nv);
    void CarryStringViewConstructionBorrow(CFlatParser::PostfixExpressionContext* ctx,
                                           LLVMBackend::NamedVariable& namedVar);
    void ComposeMovableTempField(CFlatParser::PostfixExpressionContext* ctx,
        const LLVMBackend::NamedVariable& source, LLVMBackend::NamedVariable& result,
        llvm::Type* accessedType, llvm::Value* fieldIndex);
    void CarryStackCharBufferBorrow(LLVMBackend::NamedVariable& destination,
                                    const LLVMBackend::NamedVariable& source,
                                    llvm::Value* sourcePointer = nullptr);
    void UpdateAssignmentStackCharBufferBorrow(
        LLVMBackend::NamedVariable* result,
        const LLVMBackend::NamedVariable& destination,
        const LLVMBackend::NamedVariable* source,
        std::string_view assignmentOperator);
    static bool IsStackCharBufferBorrow(const LLVMBackend::NamedVariable& nv);
    static bool StackCharBufferOutlives(const LLVMBackend::NamedVariable& source,
                                        const LLVMBackend::NamedVariable& destination);
    static std::string StackCharBufferSource(const LLVMBackend::NamedVariable& nv);
    bool RejectStackCharBufferEscape(const LLVMBackend::NamedVariable& nv,
                                     antlr4::ParserRuleContext* ctx);
    bool IsOwningArrayStringElementRead(const LLVMBackend::NamedVariable& nv, llvm::Value* value);
    bool IsRawHeapStringElementRead(const LLVMBackend::NamedVariable& nv, llvm::Value* value);
    bool RawHeapBaseIsNewArrayLocal(llvm::Value* gepBase);
    // Return-position consume/copy of an owning value read out of another object's storage.
    bool ReturnSourceIsIndirectOwningLvalue(const LLVMBackend::NamedVariable& nv, llvm::Value* value);
    bool FieldPathRootIsFrameLocal(llvm::Value* storage);
    void CheckReturnedStructFramePointerFields(LLVMBackend* compiler, llvm::Function* function);
    bool CoreUniqueOwnerPathIsFrameLocal(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& owner);

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
    LLVMBackend::NamedVariable ParseAssignmentExpressionNamedBody(CFlatParser::AssignmentExpressionContext* ctx,
                                                                  ResultUse use);

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

    // Re-ask the owner slots behind a boxed provenance proof. A later plain assignment may have
    // nulled or rebound an arm, making the interface box the sole owner.
    bool InterfaceBoxBorrowStillKeepsOwner(const LLVMBackend::NamedVariable& nv) const;

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
                                             std::vector<std::string>& sourceNames,
                                             std::vector<llvm::Value*>& ownerSlots);

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
    bool ReturnLeaksOwnershipIntoInterface(llvm::Value* right, LLVMBackend* compiler, bool pointerArithmetic = false);
    bool PointerArithmeticRootLeaks(llvm::Value* value, LLVMBackend* compiler);
    // Classifies the storage root of a pointer value (GEPs and casts stripped, aliases followed).
    PointerValueRootKind PointerRootKind(llvm::Value* value, LLVMBackend* compiler);
    // T522 interface returns: later-defined callees are collected into `deferred`, not borrowed.
    PointerValueRootKind PointerRootKindDeferring(llvm::Value* value, LLVMBackend* compiler,
                                                  std::vector<LLVMBackend::DeferredCallBorrow>& deferred);
    bool PointerRootKindEveryArmLocal(llvm::Value* value, LLVMBackend* compiler,
                                      bool frameLocalContainer = false);

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
    LLVM_ATTRIBUTE_NOINLINE bool IsSharedInterfaceClassJoin(llvm::Value* value);

    // Resolve one leaf arm once, retaining both its concrete class and the binding that supplied it.
    // A call result has no binding; nested joins are handled by the recursive callers below.
    struct ResolvedJoinArmClass
    {
        std::string Name;
        const LLVMBackend::NamedVariable* Source = nullptr;
    };

    bool ResolveJoinArmClass(llvm::Value* value, ResolvedJoinArmClass& result) const;

    llvm::Value* BuildJoinArmPredicate(llvm::Value* value, const std::string& targetTypeName,
                                       bool targetIsInterface, LLVMBackend* compiler,
                                       std::string& failure, int depth = 0);

    /*
     * Box the arms of a pointer JOIN ('?:' or '??') into an interface fat pointer. A join carries
     * no NamedVariable TypeName, so the ordinary upcast is skipped and a raw `ptr` would be bitcast
     * into the fat struct type - invalid IR. Box each arm inside the arm's OWN block, which also
     * covers arms with DIFFERENT concrete classes (each gets its own vtable), and join the fat
     * pointers with a phi inserted at `joinPoint`. A null arm contributes a null fat pointer. An
     * arm's concrete class comes from ResolveJoinArmClass, which answers a BORROWED arm (a plain
     * load, absent from the `new`-site ledger) from the loaded binding's declared type and a direct
     * call arm from its registered pointer return type - otherwise that arm would fail to resolve
     * and the caller would bitcast a raw `ptr` into the fat struct. Returns nullptr when this does
     * not apply, leaving the caller's normal path
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
                                     const LLVMBackend::NamedVariable& rhs);

    // `unique<T>* p = move us;` on a unique<T>[N] source: a pointer cannot own the array. Logs and
    // returns true when the bind is rejected.
    bool RejectUniqueArrayMoveIntoPointer(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhs);

    // Element-axis sibling of RejectRawPointerToArrayView, same two binding doors.
    bool RejectArrayViewElementMismatch(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV);

    // Primitive name for an unnamed constant source of the guard below; "" when it is not scalar.
    std::string SpellPrimitiveConstantType(llvm::Type* type);

    // Primitive-scalar sibling of the two above: only a folded constant zero is a null view.
    bool RejectPrimitiveValueIntoArrayView(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV);

    // C++ axis: a const C++ object's address into a mutable C++ record pointer slot (clang refuses).
    bool RejectConstRecordPointeeStore(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV,
                                     const std::string& kind, const std::string& name);

    // Primitive-to-pointer sibling: a nonzero scalar is a value, not an address.
    bool RejectImplicitPrimitiveToPointer(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV,
                                     llvm::Value* value,
                                     const std::string& action,
                                     const std::string& destination);
    void RejectNewPointerSlotDepthMismatch(antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& source, const LLVMBackend::TypeAndValue& target);

    // #5: an implicit pointer conversion of `value` to `target` that discards const.
    bool RejectConstDiscardingConversion(antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& source, llvm::Value* value,
        const LLVMBackend::TypeAndValue& target);
    bool RejectImplicitPointerToNumber(antlr4::ParserRuleContext* ctx,
                                       const LLVMBackend::TypeAndValue& target,
                                       const LLVMBackend::NamedVariable& rhsNV,
                                       llvm::Value* value,
                                       const std::string& action,
                                       const std::string& destination);

    // Element identity of a view read out of a struct FIELD, however the field was addressed.
    bool ViewFieldElementForRead(llvm::Value* value, llvm::Value* storage,
                                 LLVMBackend::TypeAndValue& out) const;

    // Element identity of a '?:' arm that is an array view, and its propagation onto the join.
    bool TernaryArmViewType(llvm::Value* value, llvm::Value* storage,
                            LLVMBackend::TypeAndValue& out) const;
    // `nullCoalesce` only picks the operator the diagnostic names; '??' joins the same way.
    void PropagateTernaryViewElement(antlr4::ParserRuleContext* ctx,
                                     llvm::Value* trueValue, llvm::Value* trueStorage,
                                     llvm::Value* falseValue, llvm::Value* falseStorage,
                                    llvm::Value* join, bool nullCoalesce = false,
                                    const LLVMBackend::TypedValue* trueShape = nullptr,
                                    const LLVMBackend::TypedValue* falseShape = nullptr);

    llvm::Value* ParseAssignmentExpression(CFlatParser::AssignmentExpressionContext* ctx,
                                           LLVMBackend::NamedVariable* assignmentResult = nullptr);
    bool IsDeclaredPointerSubscriptString(
        CFlatParser::AssignmentExpressionContext* expression,
        llvm::Value* value, llvm::Value* storage);

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
                              LLVMBackend::TypedValue& trueShape,
                              LLVMBackend::TypedValue& falseShape,
                              llvm::Value*& trueStorage, llvm::Value*& falseStorage,
                              const std::function<void()>& atTrue,
                              const std::function<void()>& atFalse,
                              size_t trueOccurrence, size_t falseOccurrence,
                              bool trueIsUnsigned = false, bool falseIsUnsigned = false,
                              const std::string& interfaceDestination = {},
                              bool destinationPointer = false);
    bool TernaryPointerJoinIsRejected(LLVMBackend* compiler,
        const LLVMBackend::TypeAndValue& trueType, const LLVMBackend::TypeAndValue& falseType,
        bool trueSimd, bool falseSimd, bool trueArrayView, bool falseArrayView,
        bool trueNullArm, bool falseNullArm, const std::string& interfaceDestination,
        bool destinationPointer);
    // A `?:` call argument over two different classes that BoxPointerJoinArgument did not box into
    // an interface goes in as a raw pointer: refused like clang's unrelated A*/B* join.
    void RefuseUnboxedClassJoinArgument(antlr4::ParserRuleContext* ctx, llvm::Value* argValue);
    struct DeferredClassJoinArgument
    {
        size_t argIdx;
        antlr4::ParserRuleContext* ctx;
        llvm::Value* value;
    };
    // Probe all deferred joins as one overload candidate set, then emit boxing only after viability
    // is settled. The receiver offset comes from the assembled arguments and explicit binding map.
    LLVM_ATTRIBUTE_NOINLINE bool BoxDeferredJoinArguments(
        std::vector<LLVMBackend::NamedVariable>& arguments,
        const std::vector<int64_t>& declaredIdx, const std::string& functionName,
        const std::vector<DeferredClassJoinArgument>& pending,
        CFlatParser::ArgumentExpressionListContext* argList,
        const std::vector<LLVMBackend::FunctionSymbol>* candidateFamily,
        const LLVMBackend::NamedVariable* implicitReceiver);
    // Indirect calls have one fixed signature; defer their interface boxing until every call
    // argument is typed so ordinary-argument failures cannot trigger speculative boxing.
    LLVM_ATTRIBUTE_NOINLINE llvm::Value* BoxFunctionPointerJoinArgument(
        const LLVMBackend::TypeAndValue::FuncPtrParam& parameter, size_t paramIndex,
        llvm::Value* argValue, std::string& ifaceNameOut);
    LLVM_ATTRIBUTE_NOINLINE void ResolveFunctionPointerClassJoins(
        std::vector<LLVMBackend::NamedVariable>& arguments,
        std::vector<llvm::Value*>& values,
        const LLVMBackend::TypeAndValue& functionType,
        antlr4::ParserRuleContext* callCtx);
    bool ClassesShareInterface(const std::string& a, const std::string& b) const;

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
     * kept unless the copy above already consumed it. `hoistTo` is the block holding the arm
     * branch: an alloca-based struct temp is zeroed and re-keyed there instead of freed here, so
     * a joined pointer INTO it stays live until the statement ends (see FlushOwnedTempsSince).
     * movedString (both arms explicit moves): a string arm is the moved-out buffer, not copied.
     */
    void FinishTernaryArm(LLVMBackend* compiler, llvm::Value*& value,
                          const LLVMBackend::OwnedTempMark& mark, bool& deepCopied,
                          llvm::BasicBlock* hoistTo, bool movedString = false);

    llvm::Value* CloneTernaryClosureValue(
        llvm::Value* value, antlr4::ParserRuleContext* ctx);

    /*
     * Lower `cond ? a : b` as a real branch, mirroring the '??' lowering directly below: the true
     * arm is emitted into its own block, the false arm into its own, and the two join through a PHI
     * in the resume block. Only the selected arm's code runs, so a `move` or a dereference in the
     * other arm has no effect - the eager CreateSelect form executed both unconditionally.
     */
    static void OperandPointerDepth(const LLVMBackend::NamedVariable& namedVar, llvm::Value* value,
                                    int& depth, bool& elemPointer);
    void JoinTernaryClassPointee(LLVMBackend::TypedValue& result,
                                 const LLVMBackend::TypedValue& trueArm,
                                 const LLVMBackend::TypedValue& falseArm);
    // `c ? a : b` over two function values: resolves an overloaded name arm against the other
    // arm, and refuses arms whose signatures provably differ (every consumer sees that refusal).
    void UnifyTernaryFunctionArms(antlr4::ParserRuleContext* ctx,
                                  antlr4::ParserRuleContext* trueArm,
                                  antlr4::ParserRuleContext* falseArm,
                                  llvm::Value*& trueValue, llvm::Value*& falseValue,
                                  LLVMBackend::TypedValue& trueShape,
                                  LLVMBackend::TypedValue& falseShape);
    // `c ? a : b` with a closure arm and a named-function / thin function<> arm of the same signature:
    // lifts the function arm into the closure form (in its own arm block) so the join is closure-typed.
    // Returns true when an arm was lifted: the join is then a fresh value, never an lvalue.
    bool LiftTernaryFunctionArmToClosure(antlr4::ParserRuleContext* ctx,
                                         llvm::Value*& trueValue, llvm::Value*& falseValue,
                                         LLVMBackend::TypedValue& trueShape,
                                         LLVMBackend::TypedValue& falseShape,
                                         llvm::Value*& trueStorage, llvm::Value*& falseStorage,
                                         const std::function<void()>& atTrue,
                                         const std::function<void()>& atFalse);
    static void JoinTernaryPointerShape(LLVMBackend::TypedValue& result,
                                        const LLVMBackend::TypedValue& trueArm,
                                        const LLVMBackend::TypedValue& falseArm);
    // Appends a raw owning LOCAL pointer arm of a '?:' / '??' join as a conditional move source.
    // Returns false (nothing appended) for every other shape, which keeps the existing path.
    bool CollectOwningLocalPointerArm(const LLVMBackend::TypedValue& shape, llvm::Value* value,
        llvm::Value* storage, llvm::BasicBlock* armBlock, antlr4::ParserRuleContext* armCtx,
        std::vector<LLVMBackend::ConditionalMoveSource>& out);
    bool CollectBorrowedPointerArm(const LLVMBackend::TypedValue& shape, llvm::Value* value,
        llvm::Value* storage, llvm::BasicBlock* armBlock,
        std::vector<LLVMBackend::ConditionalMoveSource>& out);
    LLVMBackend::TypedValue ParseTernaryBranches(
        CFlatParser::ConditionalExpressionContext* ctx,
        const LLVMBackend::TypedValue& condTv,
        CFlatParser::ExpressionContext* expressionTrueCtx,
        CFlatParser::ConditionalExpressionContext* expressionFalseCtx,
        ResultUse use,
        const LLVMBackend::TypeAndValue& outerExpected,
        llvm::Value* cxxTernaryDeclDest,
        const std::string& cxxTernaryDeclType,
        bool collapseLvalueArms,
        bool collapseLvalueSink);
    // Body of ParseTernaryBranches; ownedCallArms receives the per-arm owning-call bits.
    LLVMBackend::TypedValue ParseTernaryBranchesBody(
        CFlatParser::ConditionalExpressionContext* ctx,
        const LLVMBackend::TypedValue& condTv,
        CFlatParser::ExpressionContext* expressionTrueCtx,
        CFlatParser::ConditionalExpressionContext* expressionFalseCtx,
        ResultUse use,
        const LLVMBackend::TypeAndValue& outerExpected,
        llvm::Value* cxxTernaryDeclDest,
        const std::string& cxxTernaryDeclType,
        bool collapseLvalueArms,
        bool collapseLvalueSink,
        int& ownedCallArms);
    struct TernaryArrayCountState
    {
        llvm::Value* Count = nullptr;
        bool Known = false;
        bool Available = false;
        std::vector<size_t> SourceParams;
    };
    struct TernaryArrayJoinState
    {
        bool IsView = false;
        bool IsFixed = false;
        bool ViewMayOwn = false;
        bool TrueViewMayOwn = false;
        bool FalseViewMayOwn = false;
        bool ViewAlwaysOwns = false;
        TernaryArrayCountState TrueCount;
        TernaryArrayCountState FalseCount;
        llvm::Value* TrueViewOwns = nullptr;
        llvm::Value* FalseViewOwns = nullptr;
    };
    TernaryArrayCountState GetTernaryArrayCountState(llvm::Value* value,
        llvm::Value* storage, const LLVMBackend::TypedValue& shape);
    std::unique_ptr<TernaryArrayJoinState> PrepareTernaryArrayJoinState(
        llvm::Value* trueValue, llvm::Value* falseValue,
        llvm::Value* trueStorage, llvm::Value* falseStorage,
        const LLVMBackend::TypedValue& trueShape,
        const LLVMBackend::TypedValue& falseShape,
        const std::function<void()>& atTrue, const std::function<void()>& atFalse);
    void RegisterSelectArrayViewJoin(bool expectedIsView, llvm::Value* selectValue,
        llvm::Value* selectCond, llvm::Value* trueValue, llvm::Value* falseValue,
        const LLVMBackend::TypedValue& trueShape,
        const LLVMBackend::TypedValue& falseShape);
    const LLVMBackend::NamedVariable* TernaryArrayCountSource(llvm::Value* storage,
        bool includeGlobals);
    llvm::Value* TernaryArrayArmCount(llvm::Value* value, llvm::Value* storage,
        const LLVMBackend::TypedValue& shape);
    bool TernaryArrayArmCountKnown(llvm::Value* value, llvm::Value* storage,
        const LLVMBackend::TypedValue& shape);
    void AppendTernaryArrayArmCountSources(llvm::Value* value, llvm::Value* storage,
        std::vector<size_t>& out);
    llvm::Value* TernaryArrayArmRuntimeOwns(llvm::Value* value);
    bool TernaryArmIsView(llvm::Value* value, const LLVMBackend::TypedValue& shape);
    bool TernaryArmMayOwn(llvm::Value* value);

    LLVMBackend::TypedValue ParseConditionalExpression(CFlatParser::ConditionalExpressionContext* ctx,
                                                        ResultUse use = ResultUse::Value);

    LLVMBackend::TypedValue ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx,
                                                      ResultUse use = ResultUse::Value);

    LLVMBackend::TypedValue ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx,
                                                       ResultUse use = ResultUse::Value);

    void EnsureOperatorBoolForValue(llvm::Value* value);

    LLVMBackend::TypedValue ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx,
                                                        ResultUse use = ResultUse::Value);

    LLVMBackend::TypedValue ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx,
                                                        ResultUse use = ResultUse::Value);

    LLVMBackend::TypedValue ParseAndExpression(CFlatParser::AndExpressionContext* ctx,
                                                ResultUse use = ResultUse::Value);

    /*
     * C reads `(X) op y` as a cast only when X names a TYPE; the grammar has no symbol table, so
     * `(s.x) + 1`, `(Spec<int>.value) * 2` parse as a cast of the unary `op y`. Lookup decides:
     * when X names a value, the and-expression is re-parsed from its own tokens with `(X)` wrapped
     * as `((X))` (a parenthesized expression), so every binary operator keeps C precedence.
     */
    struct ValueCastReparse {
        std::unique_ptr<antlr4::ListTokenSource> source;
        std::unique_ptr<antlr4::CommonTokenStream> tokens;
        std::unique_ptr<CFlatParser> parser;
        CFlatParser::AndExpressionContext* root = nullptr;
        CFlatParser::CastExpressionContext* hazard = nullptr;
        std::string text;   // the original and-expression text, guards a recycled ctx address
    };
    std::unordered_map<const antlr4::ParserRuleContext*, std::unique_ptr<ValueCastReparse>> valueCastReparses_;
    CFlatParser::CastExpressionContext* FindValueCastHazard(antlr4::tree::ParseTree* node);
    bool CastTypeNameNamesValue(CFlatParser::CastTypeNameContext* typeName);
    CFlatParser::AndExpressionContext* ReparseValueCast(CFlatParser::AndExpressionContext* ctx,
                                                        CFlatParser::CastExpressionContext* hazard);

    // Interface comparisons use the data pointer: against nullptr, or between two fat values.
    // This keeps aggregate interface values out of LLVM icmp while preserving object identity.
    void LowerInterfaceNullCompare(antlr4::ParserRuleContext* ctx,
                                   LLVMBackend::TypedValue& lv, LLVMBackend::TypedValue& rv);

    LLVMBackend::TypedValue ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx,
                                                     ResultUse use = ResultUse::Value);

    // The single-child multiplicative body: reduce a parsed cast-expression binding to the
    // TypedValue the arithmetic chain hands upward. Shared so an 'as' operand can keep the
    // NamedVariable and still produce byte-identical TypedValue state.
    LLVMBackend::TypedValue TypedValueOfNamedOperand(LLVMBackend::NamedVariable& namedVar,
                                                     antlr4::ParserRuleContext* ctx);

    // A generic function instantiation used as an arithmetic operand (`wrap<int> * 0.5`) publishes
    // only its CallerName; resolve it to the function so the operand is refused as a function value.
    void ResolveGenericFunctionValueOperand(LLVMBackend::NamedVariable& namedVar,
                                            antlr4::ParserRuleContext* ctx);

    /*
     * The castExpression an 'is'/'as' operand reduces to when relational -> shift -> additive ->
     * multiplicative is a pure single-child passthrough. Parsing it through ParseCastExpression
     * keeps the source's NamedVariable (Storage, IsOwning, CallerName, TypeAndValue), which every
     * boxing guard needs and which the TypedValue-returning chain discards. Returns nullptr for
     * any other shape (a '?:', an arithmetic operand); those legitimately have no named source.
     */
    CFlatParser::CastExpressionContext* SoleCastOperandOf(CFlatParser::RelationalExpressionContext* relCtx);

    LLVMBackend::TypedValue ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx,
                                                      ResultUse use = ResultUse::Value);

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
                                     LLVMBackend::TypeAndValue& shape,
                                     const LLVMBackend::NamedVariable* srcBinding = nullptr);

    /*
     * Classify an 'is'/'as' source. `structName` is set for the two concrete kinds; `shape` is
     * set for PointerShaped and is the spelling used in the shared rejection message.
     *
     * Unknown is REACHABLE and is a hard error, not a dead bucket. Known spellings that land
     * there: the `select` a chained `(x as C) as I` lowers to and any other bare `ptr` with no
     * elemType. All of them miscompiled into a bogus fat pointer before
     * this classification existed, so the error is the improvement - but do not assume the bucket
     * is empty.
     */
    CastSourceKind ClassifyCastSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                      std::string& structName, LLVMBackend::TypeAndValue& shape,
                                      const std::string& srcTypeName = {},
                                      const LLVMBackend::NamedVariable* srcBinding = nullptr);

    // Boxing a class value into an interface needs the object's ADDRESS. A loaded value came
    // straight from its storage, so reuse that; anything else (a call result) is spilled.
    llvm::Value* AddressOfClassValueOperand(llvm::Value* value, LLVMBackend* compiler);

    llvm::Value* GenerateIsCheck(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType = nullptr,
                                  const std::string& srcTypeNameIn = {},
                                  const LLVMBackend::NamedVariable* srcBinding = nullptr);

    llvm::Value* GenerateSafeCast(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType = nullptr,
                                  const LLVMBackend::NamedVariable* srcBinding = nullptr,
                                  const std::string& srcTypeNameIn = {});

    LLVMBackend::TypedValue ParseSpaceshipExpression(CFlatParser::SpaceshipExpressionContext* ctx,
                                                       ResultUse use = ResultUse::Value);
    static llvm::StructType* OrderingCategoryStruct(LLVMBackend* compiler, const std::string& category);
    LLVMBackend::TypedValue ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx,
                                                       ResultUse use = ResultUse::Value);

    // Walk a single-child expression chain to the leaf Identifier terminal.
    // Returns the identifier name, or "" if the expression is complex (e.g., arithmetic, member access).
    std::string TryGetSimpleIdentifier(antlr4::ParserRuleContext* ctx);
    static int8_t CxxArgumentLvalueKind(antlr4::tree::ParseTree* tree);

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

    struct ShiftOperand
    {
        LLVMBackend::TypedValue value;
        LLVMBackend::NamedVariable named;
        std::string name;
        bool accumulated = false;
        // "char" / "int" / "long" when the operand is written as a literal: its C++ type.
        std::string literalType;
        // A dotted name in an imported C++ namespace that is no CFlat value (`std.cout`,
        // `std.endl`): spelled into the C++ infix expression instead of being evaluated.
        std::string cxxName;
        // The operand's own syntax (null once accumulated): proves a scalar lvalue.
        antlr4::ParserRuleContext* expr = nullptr;
    };
    llvm::Value* TryCxxInfixShift(CFlatParser::ShiftExpressionContext* ctx, const std::string& op,
                                  const ShiftOperand& lhs, const std::string& lhsClass,
                                  llvm::Value* lhsStorage,
                                  const ShiftOperand& rhs, const std::string& rhsClass,
                                  llvm::Value* rhsStorage);
    bool ReportCxxConstPointerOperatorFailure(
        LLVMBackend* compiler, antlr4::ParserRuleContext* ctx,
        const std::string& sourceName,
        const std::vector<LLVMBackend::NamedVariable>& arguments,
        const std::string& op, const std::string& lhsCxxName = {},
        const std::string& rhsCxxName = {},
        bool lhsPointeeConst = false, bool rhsPointeeConst = false);

    struct ShiftPairResult
    {
        LLVMBackend::TypedValue value;
        LLVMBackend::NamedVariable named;
        std::string name;
    };

    /*
     * A C++ operator declared to return `T&` hands back a POINTER at the CFlat boundary. Both
     * helpers keep that reference pointing at the real object instead of a copy:
     * CarryCxxOperatorResult reloads it into the accumulator when another operand follows at the
     * SAME precedence level (and records the referenced type otherwise), and
     * NormalizeCxxReferenceOperand does the same for an operand handed UP from an inner level.
     */
    void CarryCxxOperatorResult(antlr4::ParserRuleContext* ctx, llvm::Value* result,
                                bool moreOperands, llvm::Value*& accValue,
                                llvm::Value*& accStorage, bool& accIsRvalue,
                                llvm::Type*& accRefType, std::string& accSourceTypeName);

    void NormalizeCxxReferenceOperand(antlr4::ParserRuleContext* ctx,
                                      LLVMBackend::TypedValue& operand);

    void NormalizeCxxReferenceNamed(antlr4::ParserRuleContext* ctx,
                                    LLVMBackend::NamedVariable& operand);

    // The referenced value type when `value` is the pointer an operator returned for a `T&`
    // result, else null. Fed to TypedValue::cxxRefValueType.
    llvm::Type* CxxReferenceResultType(antlr4::ParserRuleContext* ctx,
                                       const LLVMBackend::NamedVariable& result,
                                       llvm::Value* value);

    ShiftPairResult ParseShiftPair(const ShiftOperand& lhs, const ShiftOperand& rhs,
                                   const std::string& op,
                                   CFlatParser::ShiftExpressionContext* ctx,
                                   ResultUse use, bool moreOperands);

    LLVMBackend::TypedValue ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx,
                                                  ResultUse use = ResultUse::Value);

    void LogVoidPointerArithmetic(antlr4::ParserRuleContext* ctx);
    LLVMBackend::TypedValue ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx,
                                                     ResultUse use = ResultUse::Value);

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

    // Check a receiver use at call sites that do not load the receiver first.
    void CheckMovedReceiver(const LLVMBackend::NamedVariable& receiver);
    // Value read of a named variable (LoadNamedVariable), given the value it produced: deferred inside an
    // equality operand.
    void CheckMovedValueRead(const LLVMBackend::NamedVariable& nv, llvm::Value* readValue);
    void CheckMovedUse(const LLVMBackend::NamedVariable& nv, std::vector<DeferredMovedRead>* deferred,
                       llvm::Value* readValue);
    bool IsPointeeMoveUse(const LLVMBackend::NamedVariable& source);
    void LogMovedUse(const std::string& subject, int line, int column, bool pointeeMove = false,
                     bool nonDerefUse = false);
    // Same text, reported at ctx (the position LogErrorContext gives the moved-use refusal).
    void LogMovedUse(antlr4::ParserRuleContext* ctx, const std::string& subject, bool pointeeMove = false,
                     bool nonDerefUse = false);
    static std::string MovedUseMessage(const std::string& subject, bool pointeeMove, bool nonDerefUse);

    // Give an untyped literal argument the shape a C++ constructor overload can match on.
    // sourceTwin: the argument's own TypeAndValue that lowering reads; a typed join is mirrored there.
    void TypeUntypedCtorArg(LLVMBackend::TypeAndValue& argType, llvm::Value* argValue,
                            const std::string& suffixedLiteralIdentity = {},
                            LLVMBackend::TypeAndValue* sourceTwin = nullptr);

    llvm::Value* LoadNamedVariableImpl(LLVMBackend::NamedVariable& namedVar);

    // If lvalue is a struct type with a user-defined operator, dispatch to it.
    bool ReportMissingUnaryStructOperator(antlr4::ParserRuleContext* ctx, llvm::Value* operand,
                                          const std::string& op);
    // Returns the result Value*, or nullptr to fall back to built-in CreateOperation.
    // Returns a non-null value if a user-defined operator overload was found and called,
    // nullptr if the operand type has no matching operator (fall back to primitive handling).
    llvm::Value* TryUnaryOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx, llvm::Value* operandStorage = nullptr,
        const LLVMBackend::NamedVariable* operandSource = nullptr,
        bool operandIsConst = false);
    // Built-in unary operator on a C++ class through one implicit arithmetic conversion function.
    // 0 = not applicable, 1 = converted (value / namedVar updated), 2 = error reported.
    int ConvertUnaryOperandViaImplicitConversion(
        llvm::Value*& value, LLVMBackend::NamedVariable& namedVar, const std::string& op,
        antlr4::ParserRuleContext* ctx);
    // Free `operator@(const T&)` / free operator template on an imported C++ class.
    llvm::Value* TryUnaryFreeOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx, llvm::Value* operandStorage,
        bool operandIsConst = false, bool operandExplicitMove = false,
        bool operandIsConstPrvalue = false, bool operandIsVolatile = false,
        bool operandIsVolatileLvalue = false, bool operandIsLvalueRefResult = false);

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
    // rhsPointerDepth/rhsElemPointer carry the right OPERAND's recorded depth (0 = not
    // recorded); only the receiver reaches the scorer with a type, so without them a
    // 'T**' operand binds a 'T*' operator parameter under opaque pointers.
    // C++ classes may overload && and ||; both operands are evaluated (C++ drops short-circuit
    // for an overloaded operator too) and the chain folds left to right. Null when the first
    // operand is not a C++ class carrying that operator.
    LLVMBackend::TypedValue TryClassLogicalOperatorChain(
        const std::string& op, antlr4::ParserRuleContext* ctx,
        const LLVMBackend::TypedValue& first,
        const std::function<LLVMBackend::TypedValue(size_t)>& parseOperand,
        size_t operandCount);

    // > 0 while a C++20 rewrite re-enters TryBinaryOperatorOverload; the ADL infix request is
    // made only for the operator as written, where clang applies the rewrites itself.
    int cxxOperatorRewriteDepth_ = 0;
    // Set when the last reversed C++20 rewrite (`y <=> x` for `x op y`) produced the result, so
    // rewriteFromSpaceship mirrors its relation: `x > y` reversed is `0 > (y <=> x)`.
    bool lastRewriteWasReversed_ = false;
    // > 0 while a compound assignment tries CFlat's `a = a op b` desugaring: that fallback keeps
    // the registered-operator / builtin-conversion resolution and never asks clang.
    int cxxAdlSuppressDepth_ = 0;
    // The operator expression whose clang ADL request was AMBIGUOUS (refused, logged): its
    // registered and rewritten fallbacks stay silent so no overload is picked behind clang's back.
    const antlr4::ParserRuleContext* cxxAdlAmbiguousCtx_ = nullptr;
    struct AdlSuppressScope
    {
        int* depth = nullptr;
        AdlSuppressScope(int& d, bool active) { if (active) { depth = &d; ++d; } }
        ~AdlSuppressScope() { Release(); }
        void Release() { if (depth != nullptr) { --*depth; depth = nullptr; } }
        AdlSuppressScope(const AdlSuppressScope&) = delete;
        AdlSuppressScope& operator=(const AdlSuppressScope&) = delete;
    };
    llvm::Value* TryBinaryOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType = nullptr,
        int rhsPointerDepth = 0, bool rhsElemPointer = false,
        llvm::Value* lhsStorage = nullptr, llvm::Value* rhsStorage = nullptr,
        bool reportMissing = true, bool allowReversed = true,
        bool lhsIsRvalue = false, bool rhsIsRvalue = false,
        const std::string& lhsTypeName = std::string(),
        const std::string& rhsTypeName = std::string(),
        int lhsPointerDepth = 0, bool lhsElemPointer = false,
        const std::string& lhsCallerName = std::string(),
        const std::string& lhsFieldName = std::string(), bool lhsIsElementAccess = false,
        const std::string& rhsCallerName = std::string(),
        const std::string& rhsFieldName = std::string(), bool rhsIsElementAccess = false,
        const std::vector<LLVMBackend::ConditionalMoveSource>& lhsConditionalMoves = {},
        const std::vector<LLVMBackend::ConditionalMoveSource>& rhsConditionalMoves = {},
        bool rhsCxxPointeeConst = false,
        // NamedVariable::CxxArithIdentity of each operand (`1e3 + 1` is C++ double), for
        // operator-template deduction.
        const std::string& lhsArithIdentity = std::string(),
        const std::string& rhsArithIdentity = std::string(),
        // The rhs was written `move x`: it reaches the operator as an explicit move, so the
        // same borrow-parameter diagnostic a call's `move x` gets applies to the operator.
        bool rhsExplicitMove = false,
        bool lhsExplicitMove = false,
        bool lhsCxxNullptrT = false,
        bool rhsCxxNullptrT = false,
        // lhsStorage is a materialized copy of a prvalue left operand, not the caller's lvalue.
        bool lhsMaterializedPrvalue = false,
        bool lhsCxxPointeeConst = false,
        // CxxOperandObjectCv of each operand: a reference parameter is judged against the
        // operand's own cv as a call argument is. Every C++-reachable caller passes both.
        int lhsObjectCv = 0, int rhsObjectCv = 0,
        // An alias-returned native struct is already an addressable receiver, not a value to copy.
        bool lhsIsAlias = false,
        bool refuseConstWriteOnNoViable = false,
        const LLVMBackend::NamedVariable* lhsSourceNV = nullptr,
        antlr4::ParserRuleContext* rhsOperandRule = nullptr,
        // Bitfield word of a bitfield rhs operand: a non-const reference never binds it.
        llvm::Value* rhsBitfieldStorage = nullptr);

    LLVMBackend::TypedValue ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx,
                                                           ResultUse use = ResultUse::Value);

    LLVMBackend::NamedVariable ParseCastExpression(CFlatParser::CastExpressionContext* ctx, bool lvalue = false,
                                                   ResultUse use = ResultUse::Value);
    // Parser over re-parsed call text. Owned by the listener so the tree outlives the call that made it.
    struct CallFragmentParse
    {
        antlr4::ANTLRInputStream input;
        CFlatLexer lexer;
        antlr4::CommonTokenStream tokens;
        CFlatParser parser;
        explicit CallFragmentParse(const std::string& text)
            : input(text), lexer(&input), tokens(&lexer), parser(&tokens) {}
    };
    std::deque<std::unique_ptr<CallFragmentParse>> callFragmentParses_;
    // `(name)(args)` is a call, not a conversion to type `name`. Decided from the current scope; every
    // caller that would read the cast name as a type asks this first.
    bool CastIsCallSpelling(CFlatParser::CastExpressionContext* ctx);
    std::string CastCallFragment(CFlatParser::CastExpressionContext* ctx);
    CFlatParser::AssignmentExpressionContext* CastCallInitializer(CFlatParser::AssignmentExpressionContext* assign);
    CallFragmentParse* NewCallFragment(LLVMBackend* compiler, const std::string& text);
    bool TryReparseCastAsCall(CFlatParser::CastExpressionContext* ctx, bool lvalue, ResultUse use,
                              LLVMBackend::NamedVariable& out);
    bool TryReparseParenthesizedGenericCallee(CFlatParser::PostfixExpressionContext* ctx, bool lValue,
                                              ResultUse use, LLVMBackend::NamedVariable& out);
    bool ReparseFragmentAsCast(LLVMBackend* compiler, const std::string& fragment, bool lvalue,
                               ResultUse use, LLVMBackend::NamedVariable& out);
    LLVMBackend::NamedVariable ParseCastExpressionImpl(CFlatParser::CastExpressionContext* ctx, bool lvalue,
                                                       ResultUse use);

    /*
     * Give a cast operand that carries no llvm::Value a value or a diagnostic, so CreateCast is
     * never handed a null operand (which SIGSEGV'd the compiler with no output). See the
     * definition in MainListener_Expressions.cpp for which operands are resolved vs rejected.
     */
    void ResolveValuelessCastOperand(CFlatParser::CastExpressionContext* ctx,
                                     CFlatParser::CastExpressionContext* operandCtx,
                                     LLVMBackend::NamedVariable& namedVar,
                                     const LLVMBackend::TypeAndValue& destTypeName);

    LLVMBackend::TypeAndValue ParseTypeName(CFlatParser::TypeNameContext* ctx,
        bool allowSizedArray = false, antlr4::ParserRuleContext* useSite = nullptr);
    void ValidateDeclaratorPointerDepth(antlr4::ParserRuleContext* site, int depth);
    LLVMBackend::TypeAndValue ParseCastTypeName(CFlatParser::CastTypeNameContext* ctx,
        bool allowSizedArray = false, antlr4::ParserRuleContext* useSite = nullptr);
    LLVMBackend::TypeAndValue ParseTypeNameParts(
        const std::vector<CFlatParser::TypeSpecifierContext*>& typeSpecs,
        CFlatParser::TypeQualifierContext* typeQualifier,
        CFlatParser::AbstractDeclaratorContext* abstractDecl, bool allowSizedArray,
        antlr4::ParserRuleContext* typeContext, antlr4::ParserRuleContext* useSite);

    LLVMBackend::NamedVariable ParseUnaryExpression(CFlatParser::UnaryExpressionContext* ctx,
                                                    ResultUse use = ResultUse::Value);
    bool IsCxxTemporaryPrvalue(const LLVMBackend::NamedVariable& value) const;
    void ClearCxxPrvalueProvenance(LLVMBackend::NamedVariable& value) const;
    bool IsPointerLikeParent(const LLVMBackend::TypeAndValue& parent) const;
    void PropagateParentConstToField(LLVMBackend::NamedVariable& field,
        const LLVMBackend::TypeAndValue& parent,
        const LLVMBackend::TypeAndValue& fieldType) const;
    void PropagateCxxPrvalueSubobject(LLVMBackend::NamedVariable& field,
        const LLVMBackend::NamedVariable& parent,
        const LLVMBackend::TypeAndValue& fieldType, bool fieldIsPointer) const;
    // `sizeofs` = leading `sizeof` prefixes still to apply to the operand (0 = plain operand).
    LLVMBackend::NamedVariable ParseUnaryExpressionImpl(CFlatParser::UnaryExpressionContext* ctx,
                                                        ResultUse use, size_t sizeofs);
    LLVMBackend::NamedVariable TrySizeofPostfixAsType(CFlatParser::UnaryExpressionContext* ctx,
                                                      CFlatParser::PostfixExpressionContext* postFixCtx);

    /*
     * The operand of sizeof / alignof / typeof is UNEVALUATED (C): lower it into a throwaway block only to
     * learn its type, then erase every instruction it emitted and drop the owned temps, pending
     * postfix ++/-- and detection ledgers it registered. Diagnostics still fire. Returned values
     * are types only - no llvm::Value from the region survives.
     */
    struct UnevaluatedOperandType
    {
        LLVMBackend::TypeAndValue Type;
        std::string SourceTypeName;
        llvm::Type* ValueType = nullptr;  // type of the operand's value (Primary), if any
        llvm::Type* BaseType = nullptr;
        bool HasValue = false;
        bool IsStringLiteral = false;  // the value is a string literal's global
    };
    UnevaluatedOperandType EvaluateOperandTypeOnly(const std::function<LLVMBackend::NamedVariable()>& emit);
    llvm::Type* ResolveUnevaluatedOperandType(const UnevaluatedOperandType& operand,
                                               LLVMBackend::TypeAndValue& typeValue);
    // sizeof (isSizeof) or alignof of a resolved type, honoring struct-level alignas; i64 result.
    LLVMBackend::NamedVariable SizeOrAlignOfAutoVariable(const std::string& name, bool isSizeof);
    LLVMBackend::NamedVariable SizeOrAlignOfType(const LLVMBackend::TypeAndValue& typeValue,
                                                 llvm::Type* llvmType, bool isSizeof);
    LLVMBackend::NamedVariable ParseOffsetofText(antlr4::ParserRuleContext* errCtx,
                                                  const std::string& typeText,
                                                  const std::vector<std::string>& path);

    std::string ParseTypeSpecifierName(CFlatParser::TypeSpecifierContext* ctx,
                                       CFlatParser::MultiWordTypeSuffixContext* suffix = nullptr);
    std::string ParseTupleTypeSpecifier(CFlatParser::TupleTypeSpecifierContext* ctx);
    TupleTypeElementResolveContext TupleTypeElementContext(antlr4::ParserRuleContext* site);
    std::string ResolveTupleTypeElementSpecifier(
        CFlatParser::TypeSpecifierContext* typeSpec,
        CFlatParser::MultiWordTypeSuffixContext* suffix,
        const std::unordered_map<std::string, std::string>* substitutions,
        const std::string& namespaceName);

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

    /*
     * Field-destination twin of LLVMBackend::RejectValueIntoArrayViewParam. A 'T[]' field indexes
     * whole elements, so a single object - a class value, a pointer to one, or even a fat 'IA' -
     * is not one element of it; brace-init stored it into the thin view slot and the first index
     * faulted. Proves the source implements (or IS) the field interface, or IS the field's own
     * class element, before rejecting; a view, a fixed array, a counted 'new T[n]', a primitive
     * source and a null/unnamed source all pass.
     */
    bool RejectValueIntoArrayViewField(
        const LLVMBackend::NamedVariable& rightNV,
        const LLVMBackend::TypeAndValue& fieldType,
        const std::string& displayTypeName,
        const std::string& fieldName,
        antlr4::ParserRuleContext* errCtx);

    bool EmitOneFieldInit(
        llvm::Value* structPtr,
        const LLVMBackend::StructData& sd,
        const std::string& typeName,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& rightNV,
        const LLVMBackend::OwnedTempMark& ownedTempMark,
        antlr4::ParserRuleContext* errCtx,
        bool destinationIsFresh = false,
        bool deferForeignReplacementDrop = false);

    // A C++ `operator=(U)` on `typeName` for a non-T source; null when none binds. `refusal` is
    // set when C++ would call one that CFlat's call rules refuse.
    llvm::Value* TryDirectCxxAssignOperator(antlr4::ParserRuleContext* ctx,
                                            const LLVMBackend::NamedVariable& destination,
                                            const std::string& typeName,
                                            const LLVMBackend::NamedVariable& rhsNV,
                                            std::string& refusal,
                                            bool probeOnly = false,
                                            LLVMBackend::TypeAndValue* selectedReturn = nullptr);

    enum class CxxAssignReturnKind { Void, ByValue, Reference, Unsupported };
    CxxAssignReturnKind GetCxxAssignReturnKind(
        antlr4::ParserRuleContext* ctx,
        const std::string& typeName,
        const LLVMBackend::TypeAndValue& selectedReturn);
    llvm::Value* CxxAssignOperatorValue(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& namedVar,
        const std::string& typeName,
        llvm::Value* direct,
        const LLVMBackend::TypeAndValue& selectedReturn,
        LLVMBackend::NamedVariable* assignmentResult,
        llvm::AllocaInst* slot = nullptr);

    // Construct a lowered struct value into raw storage from its source address (construct_at).
    bool EmitLoweredValueIntoSlot(LLVMBackend* compiler, const std::string& typeName,
        llvm::Value* slot, LLVMBackend::NamedVariable& sourceNV, llvm::Value* sourceValue,
        antlr4::ParserRuleContext* ctx, const char* context = "into construct_at storage");

    // Construct a foreign C++ value into a fresh or replacement slot when overload selection
    // matters. Shared by scalar fields and fixed-array elements so copy/move decisions agree.
    bool EmitForeignCxxValueIntoSlot(
        const LLVMBackend::TypeAndValue& destType,
        llvm::Value* destination,
        LLVMBackend::NamedVariable& sourceNV,
        llvm::Value* sourceValue,
        const LLVMBackend::OwnedTempMark& ownedTempMark,
        const char* context,
        antlr4::ParserRuleContext* errCtx,
        bool intoFreshSlot = false,
        bool replaceExistingAfterConversion = false);
    void DestroyForeignCxxRelocationSource(const LLVMBackend::NamedVariable& sourceNV);
    bool EmitForeignCxxFixedArrayIntoSlot(
        const LLVMBackend::DeclTypeAndValue& field,
        llvm::Value* destination,
        llvm::Value* sourceSlot,
        const char* context,
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
    std::string DescribePointerDeclType(const LLVMBackend::TypeAndValue& tv) const;

    /*
     * Spell a code-value store DESTINATION the way the source wrote it: 'Rec*', 'Rec**', 'int[]',
     * 'Rec*[2]', 'string'. DescribePointerDeclType renders an array VIEW as 'int*', which is not
     * what the declaration says, so views are spelled here instead.
     */
    std::string CodeValueDestSpelling(const LLVMBackend::TypeAndValue& dest) const;

    /*
     * The cast escape is advised only where one actually COMPILES - verified per spelling, not
     * assumed. A plain pointer takes '(Rec*)' / '(Rec**)'. An array VIEW takes neither: '(int*)w'
     * then fails the view's own "cannot bind a raw pointer 'T*' to an array-view" check and
     * '(int[])w' is refused outright, so advising either would send the user into a second error.
     * `string` is reached by the char* coercion and '(string)' of a raw value is itself rejected.
     */
    std::string CodeValueCastAdvice(const LLVMBackend::TypeAndValue& dest) const;

    /*
     * Parse a struct/class field DEFAULT initializer and gate it. `struct H { Rec* p = ro; };` is a
     * store path reached by neither the declarator nor the braces, and it has FIVE default-ctor
     * emitters - ParseStructDefinition, ParseClassDefinition, ParseConstructorDefinition, and BOTH
     * `program` emitters (ParseProgramDefinition and ParseImportedProgramDefinition). The guard has
     * to be in all five or the same spelling survives through another declaration kind.
     */
    void BindNamedFunctionToFuncPtrSlot(LLVMBackend* compiler,
        LLVMBackend::NamedVariable& value, const LLVMBackend::TypeAndValue& slot,
        antlr4::ParserRuleContext* site);
    // A scalar initializer of a C++ class field runs the converting constructor: built into
    // intoSlot when given (constructedInPlace set), else into a temporary whose value is returned.
    llvm::Value* ParseFieldDefaultInitializer(
        const std::string& structName,
        const LLVMBackend::TypeAndValue& field,
        CFlatParser::AssignmentExpressionContext* ae,
        bool* srcIsUnsigned = nullptr,
        llvm::Value* intoSlot = nullptr,
        bool* constructedInPlace = nullptr);

    /*
     * The brace-list spelling of a field default (`Inner i = { x = 1 };` and the bare
     * `Inner i { x = 1 };`). Returns null when the list is not one this path can apply, in
     * which case the caller keeps its pre-existing no-initializer handling.
     */
    CFlatParser::InitializerListContext* FieldDefaultBraceList(
        const LLVMBackend::DeclTypeAndValue& field);
    bool FieldHasEmptyCxxBraceInitializer(
        const LLVMBackend::DeclTypeAndValue& field);
    // Value-initializes a C++ class at slot from `{}` (default ctor, else empty initializer_list).
    bool EmitCxxEmptyBraceConstruct(llvm::Value* slot, const std::string& typeName,
        antlr4::ParserRuleContext* braceContext, bool copyListInit);
    // A C++-class fixed array (no inner dimensions) whose list is non-empty and holds only `{}`
    // elements: every element is value-initialized at startup, never folded to a constant.
    bool IsCxxEmptyBraceArrayInit(const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* list);

    // inPlaceDest: the field slot an armed C++ sret builds into; the seed consumes it and the list
    // edits it, so a null return means the value is already in place.
    llvm::Value* ParseFieldDefaultBraceInitializer(
        const std::string& structName,
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list,
        llvm::Value* inPlaceDest = nullptr);

    // Build field initialization for a native constructor without a user ctor body. Only the
    // first `fieldCount` fields are initialized (a program's synthetic fields follow its own).
    llvm::Value* EmitAggregateFieldInitialization(
        const std::string& structName,
        llvm::StructType*& structType,
        std::vector<LLVMBackend::DeclTypeAndValue>& fields,
        size_t fieldCount = SIZE_MAX,
        llvm::Value* destination = nullptr,
        CFlatParser::InitializerListContext* braceOverrides = nullptr,
        bool preservePartialScope = false);

    llvm::Value* ComputeAggregateFieldDefault(
        const std::string& structName,
        llvm::StructType*& structType,
        std::vector<LLVMBackend::DeclTypeAndValue>& fields,
        size_t fieldIndex,
        bool& fieldSrcUnsigned);
    llvm::Value* ResolveAggregateFieldValue(
        llvm::StructType* structType,
        std::vector<LLVMBackend::DeclTypeAndValue>& fields,
        size_t fieldIndex,
        llvm::Value* rvalue,
        bool fieldSrcUnsigned);

    // True when a CFlat struct type runs a user-declared no-arg constructor for itself or for a
    // struct element reachable through its by-value fields (arrays included).
    bool CflatStructNeedsCtorConstruction(const std::string& typeName, int depth = 0);

    bool CanFreshBraceInitializeAggregate(
        const std::string& typeName,
        CFlatParser::InitializerListContext* list,
        bool force = false);
    bool EmitFreshBraceInitializeAggregate(
        llvm::Value* destination,
        const std::string& typeName,
        CFlatParser::InitializerListContext* list,
        bool force = false);

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
        CFlatParser::InitializerListContext* list,
        llvm::Value** outSlot = nullptr,
        bool rejectFrameBoundReferences = false);

    // Per-slot value-init of an array whose element type owns a resource: construct each slot
    // independently and re-apply the named overrides there, instead of splatting one seed.
    // emptyBraceCtx: the source context for an empty `{}` element's list-constructor call, which
    // exists even when `list` is null (`T[N] a = {};`).
    void EmitArrayValueInitSlots(
        llvm::Value* arrAlloc,
        llvm::StructType* elemTy,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* list,
        bool forceRoot,
        antlr4::ParserRuleContext* emptyBraceCtx);

    // Seed-once value-init of an array field: default-construct one element, apply the
    // named overrides, memcpy it into every slot of `slot`. POD elements only - an owning
    // element type routes to EmitArrayValueInitSlots instead.
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
    antlr4::ParserRuleContext* FindExplicitUnionMemberDefault(
        const LLVMBackend::DeclTypeAndValue& field);
    void ValidateUnionMemberDefaultInitializers(
        const std::vector<LLVMBackend::DeclTypeAndValue>& fields);

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

    /*
     * A string LITERAL is a 'const char*'; it is NOT a pointer to a struct. Storing one into a
     * 'T*' / 'T?' slot leaves the slot pointing at character data, which the next member access
     * reads as a 'T' (silent SIGSEGV, no diagnostic). Proven from the literal constant and a
     * registered struct pointee alone, so every unproven source stays accepted.
     * `destDesc` names the slot ("variable 'x'", "field 'S.f'", ...). Returns true after erroring.
     */
    bool RejectStringLiteralIntoStructPointer(antlr4::ParserRuleContext* ctx,
                                              const LLVMBackend::TypeAndValue& destTV,
                                              llvm::Value* right,
                                              const std::string& destDesc);

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
        CFlatParser::InitializerListContext* ctx,
        bool destinationIsFresh = false,
        const std::string* onlyField = nullptr,
        bool deferForeignReplacementDrop = false,
        llvm::Value* completedObjectSlot = nullptr,
        const std::string* completedObjectType = nullptr);
    void EmitConstructedFieldOverrides(
        llvm::Value* structPtr,
        const std::string& typeName,
        CFlatParser::InitializerListContext* ctx,
        bool retainUnwind = false,
        std::vector<std::pair<llvm::Value*, std::string>>* retainedUnwindSlots = nullptr);

    bool RejectNestedNamedBraceInitializer(
        CFlatParser::InitializerListContext* list,
        const std::string& message);

    // A `fieldInit` written as a brace (`{...}` / `{}`) rather than an expression: the grammar
    // alternative that carries NO assignmentExpression, so every element path must screen for it
    // before reading assignmentExpression(0).
    static bool FieldInitIsBraceElement(CFlatParser::FieldInitContext* fi);

    // Construct one fixed-array slot from a braced element and route it through the same
    // field-initializer machinery the scalar `T x = {field = v};` spelling uses, so every
    // field-store rule (ownership, string copy, interface rebox, temp-field reject) applies.
    void EmitBraceElementIntoFixedSlot(
        llvm::Value* elemPtr,
        llvm::StructType* elemStructTy,
        const std::string& elemTypeName,
        CFlatParser::FieldInitContext* fi,
        bool rejectFrameBoundReferences = false);

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
        llvm::Value* arrAlloc,
        bool updateExistingElements = false,
        bool rejectFrameBoundReferences = false);

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
    std::string DescribeArrayShape(const LLVMBackend::TypeAndValue& tv) const;

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
                                 bool srcElemPointer, int srcPointerDepth,
                                 uint64_t srcConstArraySize,
                                 const std::vector<uint64_t>& srcInnerDims);

    /*
     * Default-initialize a fixed array of structs declared with no initializer (`T arr[N];`),
     * giving every element what the scalar path gives `T one;` - the synthesized default
     * constructor's value, field initializers included. Primitive elements are deliberately not
     * covered: plain `int x;` is uninitialized (C semantics), so `int[N] x;` stays that way too.
     * Multi-dimensional arrays are contiguous, so the flat element walk covers `T[N][M]` as well.
     */
    void EmitFixedArrayDefaultInit(llvm::Value* arrAlloc, const LLVMBackend::TypeAndValue& tv,
                                   antlr4::ParserRuleContext* where = nullptr,
                                   bool forceRoot = false);

    // Coerce folded brace elements to their storage type using the source CFlat signedness.
    // LLVM literal width can be smaller than the source type's width.
    static llvm::Constant* CoerceConstantToArrayElement(
        LLVMBackend* compiler, llvm::Constant* c, llvm::Type* elemTy,
        const LLVMBackend::TypeAndValue& sourceTV);
    static std::string UniqueHiddenGlobalName(LLVMBackend* compiler, const std::string& kind);

    // Global fixed-size array brace/positional initializer: T[N] g = {v0,...} / {}.
    // Lowers to an LLVM constant array on the GlobalVariable (no runtime stores, which
    // cannot run at global-init time). Positional elements must be compile-time constants.
    void EmitGlobalFixedArrayInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        bool positionalArray,
        CFlatParser::DirectDeclaratorContext* direct,
        antlr4::ParserRuleContext* errCtx, bool threadLocal = false,
        bool hiddenBacking = false, llvm::ArrayType* typeOverride = nullptr,
        llvm::GlobalVariable** backingResult = nullptr);

    // Global T[] brace initializer: fold elements into hidden mutable storage, then bind the
    // user-visible view global to its first element and retain its runtime range length.
    void EmitGlobalArrayViewInferredInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        CFlatParser::DirectDeclaratorContext* direct,
        antlr4::ParserRuleContext* errCtx, bool threadLocal = false);

    // Fold an evaluated size expression to a compile-time unsigned integer. Handles literals,
    // loads of a global int with a constant initializer (cflat `const int N = ...`), integer
    // arithmetic (W*H etc.), and integer width casts. Returns false if it is not foldable.
    // constGlobals (optional): when non-null, a load of a GLOBAL only folds if the global's name
    // is in the set. This restricts if-const folding to true compile-time constants (const scalars,
    // enum members) and excludes mutable globals. When null, any global with a const-int initializer
    // folds (the original behavior, used by EvalGlobalArrayDim).
    // Where a speculative constant evaluation starts emitting, so the IR it emitted can be audited.
    struct EmissionMark
    {
        llvm::BasicBlock* Block = nullptr;
        llvm::Instruction* Last = nullptr;       // last instruction of Block before the evaluation
        llvm::BasicBlock* LastBlock = nullptr;   // last block of the function before the evaluation
        llvm::Instruction* EntryFirst = nullptr; // entry-block front; AllocaAtEntry inserts above it
    };
    static EmissionMark MarkEmission(llvm::IRBuilderBase& builder);
    // True when the IR emitted since `mark` writes memory that outlives the evaluation (a global,
    // a pre-existing local) or calls a function: a constant expression has neither.
    static bool EmittedSideEffect(const EmissionMark& mark);

    // extraGlobals (optional): a second admitted set (imported C/C++ enumerator globals).
    static bool TryFoldConstInt(llvm::Value* v, uint64_t& out,
                                const std::unordered_set<std::string>* constGlobals = nullptr,
                                const std::unordered_set<std::string>* extraGlobals = nullptr);
    void AdmitConstFloatingGlobal(LLVMBackend* compiler, const std::string& sourceName,
                                  llvm::GlobalVariable* global);

    // Resolve a GLOBAL array dimension to a compile-time constant. The size IR is built inside
    // a throwaway function so its loads/arithmetic never leak into the program-init block, then
    // folded. On a non-constant size, logs an error and returns 1 as an error-recovery extent
    // (LogError does not abort, so the dimension must stay well-formed to avoid a later crash).
    llvm::ConstantInt* EvalGlobalArrayDim(CFlatParser::AssignmentExpressionContext* expr);
    // Try the shared compile-time folder for either a local or global dimension. Locals that do
    // not fold retain their run-time size; globals report the existing constant-size diagnostic.
    llvm::ConstantInt* TryFoldArrayDim(CFlatParser::AssignmentExpressionContext* expr,
                                      bool allowEmission = true);
    void ValidateArrayDimValue(antlr4::ParserRuleContext* expr, llvm::Value* value);
    // Rejects a fixed array whose total element count (outer x inner dims) passes INT32_MAX.
    void ValidateArrayTotalElements(antlr4::ParserRuleContext* expr, uint64_t outer,
                                    const std::vector<uint64_t>& inner);
    llvm::Value* FoldArrayDimValue(antlr4::ParserRuleContext* expr, llvm::Value* value);

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
    std::string NewExpressionElementTypeName(CFlatParser::NewExpressionContext* ctx, uint32_t* aliasConstMask = nullptr);

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
    static bool IsCallArgumentThroughParens(antlr4::ParserRuleContext* ctx);
    // True when `ctx` is an operand arm of a '?:' that is the whole of a call argument (type-blind).
    static bool IsTernaryArmOfCallArgument(antlr4::ParserRuleContext* ctx);
    bool IsWholeCxxCallArgumentShape(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& expected) const;

    /*
     * True when `name` is a by-value STRUCT parameter of the current function that was NOT
     * declared `move` - i.e. the callee only borrows the caller's struct. Moving a `unique`
     * field out of such a parameter nulls the callee's copy while the caller's original still
     * holds the pointer, so both free it. A `move` parameter (leg E) owns its copy and is safe.
     */
    bool IsBorrowedStructParameter(LLVMBackend* compiler, const std::string& name);

    // The same question asked of a RESOLVED binding rather than a name. Storage identity is what
    // separates the parameter from an inner local declared with the parameter's name.
    // Copy-on-entry for a COPYABLE owning by-value parameter the body writes as a whole: the
    // caller keeps its value (a copyable owner is never consumed), so the callee owns a copy.
    void CopyWrittenCopyableParams(CFlatParser::FunctionDefinitionContext* func,
                                   const std::vector<LLVMBackend::TypeAndValue>& params);
    bool IsBorrowedByValueParamBinding(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& nv);
    void PrepareBorrowedStringParamForDataMut(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& receiver);

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
     * element) whose source is a field path rooted at a borrowed by-value parameter or an
     * alias-borrow local. The consume zeroes only a shallow copy, so the real owner's field still
     * frees the same pointee. This is the ruling ParseMoveExpression applies to `move w.b`.
     * Returns true (and has already reported) when the store must not be lowered.
     */
    // Is this field an owning VALUE (not a pointer)? Only that family is covered by the q06
    // move-required/move-allowed ruling; a pointer field stays on the delete-borrow rules.
    static bool FieldIsOwningValue(LLVMBackend* compiler, const LLVMBackend::NamedVariable& nv);
    static bool BorrowOriginIsMethodReceiver(LLVMBackend* compiler, const std::string& origin);
    bool RejectConsumeOfBorrowedByValueParamField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        antlr4::ParserRuleContext* ctx, bool constantUniqueArrayFieldElement = false);

    /*
     * q11 ruling point 2: reject an IMPLICIT consuming store whose source is storage that
     * OUTLIVES the frame - a file-scope global or a `static` local. Such storage is not
     * re-initialized between entries, so the transfer silently empties it and the second
     * execution of the same statement reads nothing. Explicit `move` stays legal (it is the
     * sanctioned spelling and re-initializes the storage), and a copyable owner never reaches
     * here because it takes the copy arm. Returns true (and has already reported) when the
     * store must not be lowered.
     */
    bool RejectImplicitConsumeOfOutlivingOwner(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        bool srcIsMove, antlr4::ParserRuleContext* ctx);
    bool RejectImplicitThisFieldConsume(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        bool srcIsMove, antlr4::ParserRuleContext* ctx);
    // Slot key of the `=` destination being lowered; `item = item;` is no transfer out.
    std::string thisFieldAssignDestPath_;
    // Field path + constant index path; empty (never exempt) for a dynamic index or non-this slot.
    static std::string ThisFieldSlotKey(const LLVMBackend::NamedVariable& nv)
    {
        if (!nv.RootIsBorrowedThisField || nv.ThisFieldPath.empty() || nv.MovePathHasDynamicIndex)
            return {};
        return nv.ThisFieldPath + "|" + nv.MovePathKey;
    }
    // Each source arm of a '?:' / '??' join that feeds a unique field store gets the same refusal
    // its plain form gets (a field of a borrowed by-value parameter). True when refused.
    bool RejectJoinedSourceIntoUniqueField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& rightNV,
        antlr4::ParserRuleContext* ctx);

    // Reject an owning-field overwrite when the field path roots at a borrowed by-value
    // parameter or an alias-borrow local. The drop-old destructor would free the real owner's
    // pointee through a shallow copy. Returns true after reporting the diagnostic.
    bool RejectStoreIntoBorrowedField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& destNV,
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
    // [immovable] assignment source refusal; true after logging. Shared by the assignment sites.
    bool RefuseImmovableAssignSource(LLVMBackend* compiler, antlr4::ParserRuleContext* ctx,
                                     const std::string& operatorText, const LLVMBackend::NamedVariable& rightNV);

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

    // C11 transparent anonymous-member access: resolve `base.fieldName` where `fieldName` is not
    // a direct field of `base` but lives inside an anonymous synthetic struct/union member.
    // The C-interop extractor flattens a C11 anonymous member to a synthetic `__anonN` sub-record
    // (e.g. _LARGE_INTEGER's unnamed DUMMYSTRUCTNAME struct), so `li.LowPart` walks that chain.
    // Builds the chained GEP/load and returns an lvalue NamedVariable identical to the explicit
    // form. Only synthetic anonymous-member fields are searched (real named members like `u`
    // are NOT transparent). Returns false
    // (leaving `out` untouched) when no anonymous member exposes `fieldName`.
    bool ResolveTransparentAnonField(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& structVar,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& out);
    bool FindTransparentAnonFieldPath(const std::string& structName,
        const std::string& fieldName, std::vector<unsigned>& path);

    // Classify a postfix call result into the struct/interface receiver slots so the next
    // chained '.member'/'.method()' resolves against it. Mirrors the inline classifier used
    // after the normal (non-interface) call path; the caller clears both slots first.
    void ClassifyPostfixCallResult(
        antlr4::ParserRuleContext* ctx,
        LLVMBackend::NamedVariable& result,
        LLVMBackend::NamedVariable& structVar,
        LLVMBackend::NamedVariable& interfaceVar);

    // Turn the pointer returned by a non-pointer `alias T` call into an lvalue. Pointer aliases
    // keep their existing borrowed-pointer value semantics.
    void PrepareAliasCallResult(
        antlr4::ParserRuleContext* ctx,
        LLVMBackend::NamedVariable& result, bool markRvalue = true);
    void NormalizeCxxRvalueReferenceArgument(
        antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable& argument);
    void BindCxxCtorPointerSlotAddress(
        antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable& argument,
        const std::string& typeName, size_t index, size_t count);

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

    // Flexible / zero-length record member in a value context: decays to a pointer to its first
    // element (C semantics), so every downstream site sees a plain `T*`.
    void DecayFlexibleArrayMember(LLVMBackend::NamedVariable& namedVar);

    // The body of ParsePostfixExpression. The public entry is a thin wrapper that funnels
    // every exit through the void-result gate below.
    LLVMBackend::NamedVariable ParsePostfixExpressionInner(CFlatParser::PostfixExpressionContext* ctx, bool lValue,
                                                       size_t dropTrailingChildren, ResultUse use);
    LLVMBackend::NamedVariable ParseStringFunctionalConstruction(
        CFlatParser::PrimaryExpressionContext* ctx, ResultUse use);

    // A call whose return type is 'void' produces no consumable value: the direct path hands
    // back a void-typed CallInst that reaches the verifier as `void <badref>`. Reject at the
    // call, where the position is still known, rather than at each consumer.
    // Leaf arms (parentheses stripped) of a nested `?:` parsed for its value under a discarded
    // conditional: a void call there is still a discarded operand, not a consumed value.
    std::vector<antlr4::misc::Interval> discardedNestedArmLeaves_;
    // The nested `?:` arms themselves: two void branches there stay a discarded statement.
    std::vector<antlr4::ParserRuleContext*> discardedNestedArms_;
    void DiagnoseVoidResultConsumed(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& nv,
                                    ResultUse use, const std::string& subject);

    // Ruling 2026-09-27: an assignment anywhere inside a call argument is refused, any callee.
    void DiagnoseAssignmentArguments(CFlatParser::ArgumentExpressionListContext* list);

    // Ruling 2026-10-13: a variable modified twice inside one argument list or brace initializer
    // list (`what` names which) is refused; the walk covers the whole region, nested lists included.
    void DiagnoseSameVariableModified(antlr4::ParserRuleContext* region, const char* what);
    // > 0 while EvaluateOperandTypeOnly lowers an unevaluated operand: nothing there runs, so
    // the same-variable rule is off (a call inside `sizeof(...)` is not checked).
    int unevaluatedOperandDepth_ = 0;

    // Runs DiagnoseSameVariableModified on every brace list under ctx, not entering a nested
    // statement, compound statement or lambda body (those check their own). Called at codegen of
    // each statement and declaration, so an expect_error block sees the refusal in its scope.
    void DiagnoseBraceListsUnder(antlr4::ParserRuleContext* ctx);

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

    // True when the runtime format path is needed for a real segment or Verbatim region.
    bool HasInterpolation(const std::string& rawText);

    // Parses a format string literal with {expr} interpolation.
    // Splits the literal into alternating plain-text and expression segments,
    // coerces each expression segment to string via operator string,
    // stacks all (ptr, len) pairs on the stack and calls __strconcat.
    llvm::Value* ParseFormatString(CFlatParser::PrimaryExpressionContext* ctx, const std::string& rawText);
    llvm::Value* ParseFormatString(CFlatParser::PrimaryExpressionContext* ctx,
                                   const std::vector<std::string>& rawTexts);

    struct CaptureInfo
    {
        std::string Name;
        LLVMBackend::TypeAndValue TV;
        bool ByReference;       // true for non-pointer struct types (capture by reference)
        llvm::Value* OuterStorage;
        bool IsThis = false;    // the implicit method self pointer; capture by value, re-register as 'this'
        bool SourceIsStaticLocal = false;
        std::optional<int64_t> ConstFoldInt;  // a folded const local stays foldable through the capture
    };

    // Walk the lambda body AST and collect variables captured from the enclosing function scope.
    std::vector<CaptureInfo> CollectLambdaCaptures(
        antlr4::ParserRuleContext* bodyCtx,
        const std::set<std::string>& lambdaParamNames,
        LLVMBackend* compiler);

    LLVMBackend::NamedVariable ParseLambdaExpression(CFlatParser::LambdaExpressionContext* ctx);

    // Accept a bonded closure field only when the holder's stack lifetime is provable.
    bool AllowBondedClosureFieldStore(
        LLVMBackend* compiler,
        LLVMBackend::NamedVariable& destination,
        llvm::Value* fieldStorage,
        const std::vector<std::string>& sources,
        antlr4::ParserRuleContext* errCtx);

    bool IsBondedClosureContainer(
        LLVMBackend* compiler, const LLVMBackend::DeclTypeAndValue& type);

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

    uint32_t ParseCharLiteral(const std::string& text);

    // foldBraces folds doubled braces outside Verbatim regions; raw strings keep their braces.
    // Pass false for accumulated literal text inside ParseFormatString.
    std::string ProcessRawText(const std::string& rawText, bool foldBraces = false);
    std::optional<StringLiteralExpressionInfo> StringLiteralExpressionValue(
        antlr4::tree::ParseTree* node);

    llvm::Value* ParseExpression(CFlatParser::ExpressionContext* ctx);
    llvm::Value* ParseConditionExpression(CFlatParser::ExpressionContext* ctx);
    // The one bool conversion of every condition position (C++ class operator bool on the object).
    llvm::Value* ConvertConditionToBool(const LLVMBackend::TypedValue& tv);
    llvm::Value* ConvertConditionToBool(llvm::Value* value);

    // A '?:' arm parsed for its value, keeping the arm's signedness for the join (see the
    // definition; the fallback is plain ParseExpression).
    LLVMBackend::TypedValue ParseTernaryArmExpression(CFlatParser::ExpressionContext* ctx);
    LLVMBackend::TypedValue ParseTernaryMoveArm(CFlatParser::MoveExpressionContext* moveCtx,
                                                antlr4::ParserRuleContext* armCtx);

    bool IsDefaultOnlyExpression(antlr4::ParserRuleContext* ctx,
                                 bool allowParentheses = true) const;
    bool IsSameTypeCastOfDefault(antlr4::ParserRuleContext* ctx,
                                 const std::string& typeName);
    bool HasDefaultOnlyTernaryArm(antlr4::ParserRuleContext* ctx) const;
    // T418: the `?:` tree of a C++ call argument whose leaves are `default` or a live local of a
    // nontrivial C++ class; fills the place leaves' common type. False for any other shape.
    // T518: `allowCallArm` also admits a zero-argument CFlat call returning that class.
    bool IsCxxDeferrableDefaultTernary(antlr4::ParserRuleContext* ctx, std::string& placeType,
                                       bool allowCallArm = false);
    bool ResolveCxxDeferredCallArm(antlr4::tree::ParseTree* node, LLVMBackend::NamedVariable& callable,
                                   LLVMBackend::TypeAndValue& returnType);
    bool IsCxxInertCallArgument(antlr4::ParserRuleContext* ctx, bool condition = false);
    // T518: whether `ctx` names a local or parameter whose storage is one of `storage`'s roots.
    bool ArgumentNamesStorageRoot(antlr4::ParserRuleContext* ctx, llvm::Value* storage);
    // T518: the `?:` of a call argument whose every leaf is `default`; nullptr otherwise.
    antlr4::ParserRuleContext* CxxDefaultOnlyTernary(antlr4::ParserRuleContext* ctx);
    bool IsCxxTernaryArgument(antlr4::ParserRuleContext* ctx);
    // T459: `c ? default : place` into a `const T&` class parameter joins a temporary of that class.
    LLVMBackend::NamedVariable ParseCxxConstRefTernaryTemp(CFlatParser::AssignmentExpressionContext* ctx,
        const LLVMBackend::TypeAndValue& dest);
    // T418: builds the deferred argument (selector + place leaves) for the C++ wrapper to evaluate.
    // T518: `guardPlace` makes the wrapper prove (in clang) that the parameter is `defaultType`.
    bool BuildCxxDeferredDefaultTernary(antlr4::ParserRuleContext* ctx, const std::string& defaultType,
        LLVMBackend::NamedVariable& out, bool guardPlace = false);
    // T142: types and owns a `default` call argument for the parameter it binds.
    void MaterializeDefaultArgument(antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable& argNV,
        const LLVMBackend::TypeAndValue& dest, bool directLoweredSret = false);
    LLVMBackend::TypeAndValue InferTernaryArmType(llvm::Value* value);
    LLVMBackend::TypeAndValue InferTernaryArmType(const LLVMBackend::TypedValue& value);
    // Pointee name of a `??` / `?:` pointer join, read from its arms (empty when unproven).
    LLVMBackend::TypeAndValue JoinedPointerArmType(llvm::Value* joined);

    // A discarded statement result (`makePlain(2);`) that is an unclaimed owning-struct rvalue
    // temp is claimed by nothing, so without this it leaks. Spill it and register for destruction
    // at the end of the full expression (FlushOwnedTemps at the block-item boundary). Mirrors the
    // field-access registration gate (see the `makeToken().text` path). Storage==null is the
    // rvalue-temp signal: a named local or a deref (`(*p)`) carries Storage and is freed by its own
    // scope dtor, so we must not double-free it here. `alias` borrows and string/closure values
    // (own runtime owned-bit + temp lists) are excluded.
    void RegisterDiscardedOwningStructTemp(const LLVMBackend::NamedVariable& nv, bool bare = false);

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
    // topLevel pushes a scratch alias frame: body-scope `using` renames this pre-scan meets
    // bind in source order and are discarded before the real walk registers them again.
    void ScanAndQueueGenericTypeUses(antlr4::RuleContext* ctx, bool topLevel = true,
        const std::unordered_set<std::string>* localEnumNames = nullptr,
        bool insideScopedExpectation = false, bool afterBareExpectation = false);

    // Check if a declaration uses a generic type and queue it for instantiation if needed.
    // Instantiation is deferred to avoid interrupting code generation in the current context.
    void QueueInstantiateGenericType(CFlatParser::DeclarationSpecifiersContext* declSpec);

    // Compute the mangled name for a generic instantiation, e.g. Box<int, float> -> "Box$int$float".
    std::string MangledGenericName(const std::string& baseName, const std::vector<std::string>& typeArgs,
                                   antlr4::tree::ParseTree* useSite = nullptr);
    void ResolveEnumTypeArguments(std::vector<std::string>& typeArgs);
    bool QueueAndProcessLocalEnumInstantiation(
        CFlatParser::GenericTypeParametersContext* params,
        const std::string& baseName, const std::vector<std::string>& typeArgs,
        const std::string& mangledName, antlr4::ParserRuleContext* site);

    // Process all pending generic instantiations that were queued during parsing.
    // This is called when it's safe to emit code (e.g., after a declaration completes).
    void ProcessPendingInstantiations();
    // Scoped expect_error recovery: count the queued records before a block; on a swallowed error,
    // drop only the records above that count (and their instantiatedGenerics marks).
    std::unordered_map<std::string, size_t> MarkPendingInstantiations() const;
    void DropPendingInstantiationsSince(const std::unordered_map<std::string, size_t>& mark);

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
        const LLVMBackend::TypeAndValue& returnType,
        bool staticOnly = false,
        bool skipStatic = false);

    // The program-owned inbox/outbox channel is always arena_channel<IMessage>.
    static std::string ArenaChannelTypeName(const LLVMBackend& compiler)
    {
        return MangleGenericInstance(compiler, "arena_channel", { "IMessage" });
    }

    // Ensure arena_channel<IMessage> (and its transitive block_pool<IMessage> /
    // page_arena<IMessage>) is fully instantiated so the synthetic program
    // inbox/outbox fields and the `>>` wiring have a concrete type + methods to
    // reference. Returns true if the type is available (template was imported).
    bool EnsureArenaChannelInstantiated(LLVMBackend* compiler);

    void ParseStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& nameOverride = {}, const std::string& namespaceName = {});
    LLVMBackend::DeclTypeAndValue ParseInlineAggregateField(
        const std::string& ownerName, CFlatParser::AggregateMemberContext* member,
        unsigned& anonymousIndex);
    void ValidateAnonymousMemberNames(
        const std::vector<CFlatParser::AggregateMemberContext*>& members);
    void ParseInlineAggregateBody(const std::string& typeName, bool isUnion,
        const std::vector<CFlatParser::AggregateMemberContext*>& members,
        CFlatParser::AlignmentSpecifierContext* alignment, unsigned& anonymousIndex,
        bool anonymousMemberBody);

    void PrepareLaterCppStructDefinitions(CFlatParser::StructDefinitionContext* ctx,
                                          const std::string& namespaceName);

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

    // The held mode of the lock guarding `receiverPath`.`guard`, or null when it is not held.
    // A `this` (or absent) receiver is the enclosing struct: its guardian is seeded bare by a
    // positional group and as "this.<guard>" by an explicit lock clause, so both spellings match.
    // *matchedKey receives the key that matched, so the write check keys on the same lock.
    const LockMode* FindHeldGuard(const std::string& receiverPath, const std::string& guard,
                                  std::string* matchedKey = nullptr) const;

    // True when the lock set holds `key` under EITHER implicit-this spelling. Inside a method
    // `inner.mtx` and `this.inner.mtx` name one lock, and a `lock(...)` statement records only
    // the spelling the user wrote - so both are probed, exactly as FindHeldGuard does.
    // True when a bare `name` in the current method body resolves to a FIELD of the enclosing
    // struct - false when a local or a parameter of that name shadows it.
    bool LockRootIsImplicitThisField(const std::string& name) const;
    bool LockSetHoldsPath(const std::string& key) const;

    // Read-side guarded-field check for a receiver-qualified access ('o.inner.value'). Keys on
    // the canonical receiver PATH, the same spelling `lock(...)` records, so a held lock on a
    // deeper receiver matches. A receiver that is not a plain path keeps the legacy
    // immediate-parent key, leaving those verdicts unchanged. heldKey receives the matched key.
    void CheckGuardedFieldRead(antlr4::ParserRuleContext* ctx, const std::string& fieldName,
                               const std::string& guard, const std::string& receiverPath,
                               const std::string& receiverName, std::string& heldKey);

    // Write-side half of the guarded-access check. Reading a guarded field is legal in every
    // held mode; WRITING one is legal only under an exclusive lock - a shared reader may not
    // mutate what other readers are reading, and an optimistic reader holds nothing at all
    // (its body must be side-effect free or the version validation means nothing).
    // No-ops when the guard is not held: the read-side check has already reported that.
    void CheckGuardedWrite(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& target);
    // The write target is a C-declared global, or a pointer loaded from one (wording only).
    bool IsReachedFromCGlobal(llvm::Value* destination);
    // The C (cGlobalsOnly) or const C/C++ global behind a write target, or null.
    llvm::GlobalVariable* GlobalBehindWrite(llvm::Value* destination, bool cGlobalsOnly);
    // Refuses a store reached through a C++ const reference or const pointer result, or, for
    // `++` / `--`, is a const C/C++ namespace object (C/C++ wording; true when it refused).
    // fallback: only the last-resort refusal of a const element no other check proved.
    bool RefuseCxxConstReferentWrite(antlr4::ParserRuleContext* ctx, llvm::Value* destination,
                                     const std::string& operation, bool constElement = false,
                                     bool fallback = false);
    // Every write path: the C/C++ wording first, then the native const checks, then the fallback.
    // True when it refused.
    bool RefuseConstWrite(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& target,
                          llvm::Value* destination, const std::string& nativeOperation,
                          const std::string& cxxOperation);
    // Refuses a write (assign / compound / ++ / -- / explicit move) whose destination lands by
    // value in a native const object; the one predicate every write path calls. True if refused.
    // A native const global with a constant initializer and no startup construction -> rodata.
    void MarkConstGlobalReadOnly(llvm::GlobalVariable* global, const LLVMBackend::TypeAndValue& tv);
    bool RefuseConstObjectWrite(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& target,
                                llvm::Value* destination, const std::string& operation);
    // Refuses `obj.f()` when obj is read-only C++ storage (const namespace object, const
    // reference result) and every overload of f is non-const. True when it refused.
    bool RefuseCxxConstReceiverCall(antlr4::ParserRuleContext* ctx,
                                    const std::vector<LLVMBackend::NamedVariable>& arguments,
                                    const std::string& receiverType, const std::string& member,
                                    const std::string& receiverText);

    // Verify that the current lock-set satisfies the RequiredLocks of the function just called.
    // Called immediately after CreateOverloadedFunctionCall; reads lastCallRequiredLocks and
    // lastCallParameterNames from the side-channel populated by that call.
    // receiverText: the name of the receiver object ("acct"), "this" for bare method calls, or "".
    // receiverPath: the receiver's canonical source path ("o.inner" for o.inner.m()), or "".
    // arguments: the NamedVariable vector passed to the call (includes implicit this at index 0 when present).
    void CheckCallSiteLocks(antlr4::ParserRuleContext* ctx,
                            const std::string& receiverText,
                            const std::vector<LLVMBackend::NamedVariable>& arguments,
                            const std::string& receiverPath = {});

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

    std::vector<std::string> ParseGenericTypeParameters(CFlatParser::GenericTypeParametersContext* genericParams,
                                                        std::vector<std::string>* valueTypes = nullptr,
                                                        std::vector<std::string>* valueDefaults = nullptr);

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

    bool CheckValueConstraints(
        const std::string& templateName,
        CFlatParser::WhereClauseContext* whereClause,
        const std::vector<std::string>& typeParams,
        const std::vector<std::string>& valueParams,
        const std::vector<std::string>& typeArgs);

    void ParseConstructorDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName,
                                    bool suppliesNoArgCtor = false, size_t cppCtorIndex = SIZE_MAX,
                                    bool hasExplicitNoArgCtor = false);
    void EmitCppStructConstructorThunk(antlr4::ParserRuleContext* ctx,
                                       CFlatParser::BlockItemListContext* body,
                                       const std::string& structName,
                                       const std::vector<LLVMBackend::DeclTypeAndValue>& fields,
                                       const std::vector<LLVMBackend::DeclTypeAndValue>& params,
                                       size_t ctorIndex);
    void EmitCppStructDestructorThunk(antlr4::ParserRuleContext* ctx,
                                      const std::string& structName);
    void EmitCppStructMoveThunk(antlr4::ParserRuleContext* ctx, const std::string& structName);

    void ParseDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& structName);

    // Like ParseDestructorDefinition, but for a `program`: emit the user's ~Name() body,
    // then append the builtin field teardown so the program has ONE destructor with the
    // builtin fields cleaned up at the end. Requires programTable[name] field indices to
    // be set before this is called.
    void ParseProgramDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& name);

    std::vector<LLVMBackend::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList);

    LLVMBackend::ConstantVariant ParseNumberConstant(const std::string& rawNumber);
    // Set by ParseNumberConstant: the last unsuffixed literal folded to a signed value with its bit
    // pattern kept (hex in [2^31, 2^32), anything past INT64_MAX), and that pattern's bits.
    bool lastNumberLiteralIsBitPattern = false;
    uint64_t lastNumberLiteralBits = 0;
    unsigned lastNumberLiteralWidth = 0;

    LLVMBackend::TypeAndValue ParseLiteralTypeAndValue(const std::string& rawNumber);

    void LogErrorContext(antlr4::tree::TerminalNode* ctx, std::string errorMessage);

    void LogErrorContext(antlr4::ParserRuleContext* ctx, std::string errorMessage);
    static void LogErrorContext(LLVMBackend* compiler, antlr4::ParserRuleContext* ctx,
                                std::string errorMessage, const std::string& fileName = {});
    bool NativeTemplateNeedsTypeArgs(const std::string& spelledBase,
                                    std::vector<std::string>& parameterNames);
    void RejectMissingNativeTemplateTypeArgs(antlr4::ParserRuleContext* site,
        const std::string& spelledBase, const std::vector<std::string>& parameterNames);

    void LogCxxErrorContext(antlr4::ParserRuleContext* ctx, const std::string& errorMessage)
    {
        std::string spelling;
        std::string typeName;
        bool refusedEarlier = false;
        const bool incomplete = Compiler(ctx)->DecodeCxxIncompleteTemplateError(
            errorMessage, spelling, typeName, &refusedEarlier);
        if (incomplete && refusedEarlier)
            Compiler(ctx)->LogErrorMessage(
                "'{}' was refused earlier in this compile because '{}' was incomplete at that "
                "point; declare '{}' before its first use in a C++ template argument",
                { spelling, typeName, typeName });
        else if (incomplete)
            Compiler(ctx)->LogErrorMessage(
                "'{}' needs the complete definition of [cpp] struct '{}'; define the struct before this use",
                { spelling, typeName });
        else
            LogErrorContext(ctx, errorMessage);
    }

    static void ReportCxxTypeUseError(
        void* listener, antlr4::ParserRuleContext* ctx, const std::string& errorMessage)
    {
        static_cast<MainListener*>(listener)->LogCxxErrorContext(ctx, errorMessage);
    }

    void LogWarningContext(antlr4::ParserRuleContext* ctx, std::string warningMessage);

    void PrintContext(antlr4::ParserRuleContext* ctx, std::string suffix = "");
};
