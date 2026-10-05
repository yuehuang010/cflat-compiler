// clang C++ API implementation of the C-interop extractor. All clang/LLVM includes are
// confined to this translation unit (see CClangExtract.h for the rationale and contract).
//
// Two-stage, single-full-parse design:
//   1. A cheap preprocess-only prepass over the header collects object-like macro names (via
//      PPCallbacks) and reconstructs function-like macros. No AST, no Sema.
//   2. One full parse of the header stub with `static const __auto_type __cflat_macro_<i> =
//      (MACRO);` probe lines appended. The frontend deduces each macro's natural type and folds
//      its value; the decls/enums/records/typedefs and the probe VarDecls are all read out of
//      that single AST. This replaces the old two-full-parse libclang flow (decl+name parse,
//      then value-fold parse) with prepass + one parse.
#include "CClangExtract.h"
#include "LlvmHelpers.h"

#define CFLAT_LLVM_COMPAT_CLANG
#undef CFLAT_LLVM_COMPAT_CLANG

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/AST/BaseSubobject.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Sema/Scope.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Token.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/IRBuilder.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ADT/SmallString.h"

#include "llvm/Support/TimeProfiler.h"

#include <set>
#include <array>
#include <algorithm>
#include <mutex>
#include <cctype>
#include <chrono>
#include <string_view>
#include <unordered_set>
#include <format>
#include <limits>

namespace cflat_cinterop
{
    using namespace clang;

    std::string AliasCxxNestedSpecializationNames(const std::string& source)
    {
        auto identChar = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : source) hash = (hash ^ c) * 1099511628211ULL;
        const std::string tag = std::format("{:016x}", hash);
        const std::string key = "__cflat_ax_key_" + tag;
        const std::string set = "__cflat_ax_set_" + tag;
        const std::string get = "__cflat_ax_get_" + tag;
        std::string out = source;
        std::map<std::string, std::string> aliases;
        std::string decls;
        size_t firstUse = std::string::npos;
        size_t from = 0;
        for (size_t pos; (pos = out.find(">::", from)) != std::string::npos; )
        {
            // The specialization's '<' (an arrow is not a closing angle).
            int depth = 0;
            size_t open = std::string::npos;
            for (size_t i = pos + 1; i-- > 0; )
            {
                if (out[i] == '>' && !(i > 0 && out[i - 1] == '-')) ++depth;
                else if (out[i] == '<' && --depth == 0) { open = i; break; }
            }
            size_t start = open;
            while (start != std::string::npos && start > 0
                   && (identChar(out[start - 1]) || out[start - 1] == ':'))
                --start;
            const size_t nameBegin = pos + 3;
            size_t end = nameBegin;
            while (end < out.size() && identChar(out[end])) ++end;
            const std::string name = out.substr(nameBegin, end - nameBegin);
            if (open == std::string::npos || start == open || name.empty() || name == "operator"
                || name == "template" || std::isdigit((unsigned char)name[0]))
            {
                from = pos + 3;
                continue;
            }
            if (end < out.size() && out[end] == '<')
            {
                int args = 0;
                size_t close = end;
                for (; close < out.size(); ++close)
                {
                    if (out[close] == '<') ++args;
                    else if (out[close] == '>' && out[close - 1] != '-' && --args == 0) break;
                }
                if (close == out.size()) { from = pos + 3; continue; }
                end = close + 1;
            }
            const std::string spelled = out.substr(start, end - start);
            auto [it, inserted] = aliases.emplace(spelled, std::string());
            if (inserted)
            {
                const std::string index = std::to_string(aliases.size() - 1);
                it->second = "__cflat_ax_" + tag + "_" + index;
                const std::string tagName = "__cflat_ax_tag_" + tag + "_" + index;
                decls += "struct " + tagName + ";\n";
                decls += "template struct " + set + "<" + tagName + ", " + spelled + ">;\n";
                decls += "typedef __remove_pointer(decltype(" + get + "(" + key + "<" + tagName
                    + ">{}))) " + it->second + ";\n";
            }
            out.replace(start, end - start, it->second);
            firstUse = std::min(firstUse, start);
            from = start;
        }
        if (decls.empty()) return source;
        size_t lineStart = out.rfind('\n', firstUse == 0 ? 0 : firstUse - 1);
        lineStart = (lineStart == std::string::npos || firstUse == 0) ? 0 : lineStart + 1;
        const std::string prologue =
            "template <class> struct " + key + " { friend auto " + get + "(" + key + "); };\n"
            "template <class Tag, class T> struct " + set + " { friend auto " + get + "(" + key
            + "<Tag>) { return (T *)nullptr; } };\n";
        out.insert(lineStart, prologue + decls);
        return out;
    }

    std::string CxxForeignIdentity(const std::string& spelling)
    {
        // Keep multi-word primitive template arguments aligned with the CFlat spellings emitted
        // by CxxSpellingForCflatType (for example vector<unsigned char> -> vector$u8).
        std::string normalized = spelling;
        for (const char* tag : { "class ", "struct ", "union ", "enum " })
            if (normalized.starts_with(tag))
            {
                normalized.erase(0, std::strlen(tag));
                break;
            }
        // Longer spellings first: this is a substring replace, so a prefix must not win.
        static constexpr std::pair<std::string_view, std::string_view> spellings[] = {
            { "unsigned long long int", "u64" }, { "signed long long int", "i64" },
            { "unsigned long long", "u64" }, { "signed long long", "i64" },
            { "long long int", "i64" }, { "long long", "i64" },
            { "unsigned long int", "ulong" }, { "signed long int", "long" },
            { "unsigned long", "ulong" }, { "signed long", "long" },
            { "long int", "long" }, { "long double", "longdouble" },
            { "unsigned short int", "u16" }, { "signed short int", "short" },
            { "unsigned short", "u16" }, { "signed short", "short" },
            { "short int", "short" },
            { "unsigned int", "u32" }, { "signed int", "int" },
            { "unsigned char", "u8" }, { "signed char", "i8" },
            { "unsigned", "u32" }, { "signed", "int" },
            { "char8_t", "c8" }, { "char16_t", "c16" },
            { "char32_t", "c32" }, { "wchar_t", "wchar" } };
        for (const auto& [from, to] : spellings)
        {
            for (size_t pos = 0; (pos = normalized.find(from, pos)) != std::string::npos; )
            {
                const bool leftOk = pos == 0
                    || (!std::isalnum((unsigned char)normalized[pos - 1])
                        && normalized[pos - 1] != '_');
                const size_t end = pos + from.size();
                const bool rightOk = end == normalized.size()
                    || (!std::isalnum((unsigned char)normalized[end]) && normalized[end] != '_');
                if (leftOk && rightOk)
                {
                    normalized.replace(pos, from.size(), to);
                    pos += to.size();
                }
                else
                    pos = end;
            }
        }
        std::string out;
        for (size_t i = 0; i < normalized.size(); ++i)
        {
            const bool negativeValue = normalized[i] == '-' && i + 1 < normalized.size()
                                    && std::isdigit((unsigned char)normalized[i + 1]);
            const bool positiveValue = std::isdigit((unsigned char)normalized[i]);
            if (negativeValue || positiveValue)
            {
                const size_t valueStart = negativeValue ? i + 1 : i;
                size_t valueEnd = valueStart;
                while (valueEnd < normalized.size()
                       && std::isdigit((unsigned char)normalized[valueEnd]))
                    ++valueEnd;
                size_t before = negativeValue ? i : valueStart;
                while (before > 0 && std::isspace((unsigned char)normalized[before - 1])) --before;
                size_t after = valueEnd;
                while (after < normalized.size() && std::isspace((unsigned char)normalized[after])) ++after;
                const bool isValueArgument = before > 0
                    && (normalized[before - 1] == '<' || normalized[before - 1] == ',')
                    && (after == normalized.size() || normalized[after] == ',' || normalized[after] == '>');
                if (isValueArgument)
                {
                    out += negativeValue ? ".n" : ".";
                    out.append(normalized, valueStart, valueEnd - valueStart);
                    i = valueEnd - 1;
                    continue;
                }
            }
            if (normalized[i] == ':' && i + 1 < normalized.size() && normalized[i + 1] == ':')
            { out += '.'; ++i; continue; }
            if (normalized[i] == '<' || normalized[i] == ',') { out += '$'; continue; }
            if (normalized[i] == '>') continue;
            if (std::isspace((unsigned char)normalized[i])) continue;
            if (normalized[i] == '*') { out += "ptr"; continue; }
            if (normalized[i] == '&') { out += "ref"; continue; }
            if (std::isalnum((unsigned char)normalized[i]) || normalized[i] == '_'
                || normalized[i] == '.' || normalized[i] == '$')
                out += normalized[i];
            else
                out += '_';
        }
        return out;
    }

    struct CxxExtractionStageTimer
    {
        bool enabled;
        std::string name;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        CxxExtractionStageTimer(bool isEnabled, std::string stage)
            : enabled(isEnabled), name(std::move(stage)) {}

        ~CxxExtractionStageTimer()
        {
            if (!enabled) return;
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << std::format("[verbose]   extraction stage {}: {:.3f} ms\n", name, ms);
        }
    };

    /*
     * A virtual member cflat cannot reach any other way, and must therefore route through a
     * clang-emitted thunk. Deliberately NARROW: a plain virtual method whose slot is unnameable
     * is still reachable through the record of the base that DECLARES it, with `this` adjusted -
     * that path works and costs nothing, so a thunk must not displace it. What has no such
     * fallback is:
     *   - a DESTRUCTOR, which is only ever looked up on the class itself. Under the MS ABI its
     *     slot is unnameable whenever the vfptr sits inside a virtual base (every iostream).
     *   - a COVARIANT return whose base conversion is not at offset zero. The derived member is
     *     refused and the base member hands back an UNADJUSTED pointer, so the fallback is not
     *     merely slower, it is wrong.
     */
    bool CxxMemberNeedsVirtualThunk(const RawCxxMember& m)
    {
        if (!m.isVirtual) return false;
        if (m.covariantReturnNeedsAdjust) return true;
        return m.kind == RawCxxMember::Destructor && m.vtableIndex < 0;
    }

    /*
     * A CONSTRUCTOR of a class with virtual bases takes an implicit extra argument in every ABI
     * cflat targets (the MS is-most-derived flag, the Itanium VTT) that arrangeCXXMethodType does
     * not surface, so a direct call would seed the vbtable from a garbage register. Route it
     * through a placement-new thunk instead and let Clang supply the argument. Deliberately
     * narrow: only a constructor cflat would otherwise bind directly is redirected. One that
     * already goes through the on-demand wrapper path (a constructor template, an inherited
     * variadic) is left alone - that path is a placement new too and is already correct.
     */
    bool CxxCtorNeedsVbaseThunk(const RawRecord& rec, const RawCxxMember& m)
    {
        if (m.kind != RawCxxMember::Constructor || !rec.hasVirtualBases) return false;
        if (!rec.layoutRefusal.empty() || rec.canonicalCtype.empty()) return false;
        if (m.access != AccessPublic || m.isDeleted || m.variadic) return false;
        if (m.requiresConstructorWrapper || !m.bindRefusal.empty()) return false;
        return !m.paramTypes.empty();   // no `this` slot: not a real constructor arrangement
    }

    bool SplitStdFunctionSpelling(const std::string& spelling, std::string& ret,
                                  std::string& params)
    {
        ret.clear();
        params.clear();
        constexpr std::string_view prefix = "std::function<";
        const size_t prefixPos = spelling.find(prefix);
        if (prefixPos == std::string::npos) return false;
        const size_t bodyStart = prefixPos + prefix.size();

        size_t outerClose = std::string::npos;
        int outerAngleDepth = 1;
        for (size_t i = bodyStart; i < spelling.size(); ++i)
        {
            if (spelling[i] == '<')
                ++outerAngleDepth;
            else if (spelling[i] == '>')
            {
                if (--outerAngleDepth == 0)
                {
                    outerClose = i;
                    break;
                }
                if (outerAngleDepth < 0) return false;
            }
        }
        if (outerClose == std::string::npos) return false;

        size_t openParen = std::string::npos;
        size_t closeParen = std::string::npos;
        int angleDepth = 0;
        int parenDepth = 0;
        for (size_t i = bodyStart; i < outerClose; ++i)
        {
            const char c = spelling[i];
            if (c == '<')
            {
                ++angleDepth;
                continue;
            }
            if (c == '>')
            {
                if (angleDepth == 0) return false;
                --angleDepth;
                continue;
            }
            if (c == '(')
            {
                if (angleDepth == 0 && parenDepth == 0 && openParen == std::string::npos)
                    openParen = i;
                ++parenDepth;
                continue;
            }
            if (c == ')')
            {
                if (parenDepth == 0) return false;
                --parenDepth;
                if (parenDepth == 0) closeParen = i;
            }
        }
        if (angleDepth != 0 || openParen == std::string::npos
            || closeParen == std::string::npos || parenDepth != 0)
            return false;
        for (size_t i = closeParen + 1; i < outerClose; ++i)
            if (!std::isspace((unsigned char)spelling[i])) return false;

        const auto trim = [](std::string value) {
            size_t first = 0;
            while (first < value.size() && std::isspace((unsigned char)value[first])) ++first;
            size_t last = value.size();
            while (last > first && std::isspace((unsigned char)value[last - 1])) --last;
            return value.substr(first, last - first);
        };
        ret = trim(spelling.substr(bodyStart, openParen - bodyStart));
        params = trim(spelling.substr(openParen + 1, closeParen - openParen - 1));
        return !ret.empty();
    }

    namespace
    {
        /*
         * Program-facing key of a C++ static inline: its ODR hash folded with internal functions
         * and variables its body reaches. The ODR hash records names, not which static each refers
         * to, so their source locations and import group must also contribute.
         */
        uint64_t StaticInlineBodyKey(const clang::FunctionDecl* root,
                                     const std::string& importGroupKey)
        {
            auto mixText = [](uint64_t& key, const std::string& text) {
                for (unsigned char c : text)
                    key = (key ^ c) * 1099511628211ULL;
                key = (key ^ 0xffu) * 1099511628211ULL;
            };
            struct Visitor : clang::RecursiveASTVisitor<Visitor>
            {
                std::vector<const clang::FunctionDecl*>& queue;
                uint64_t& key;
                decltype(mixText)& mix;
                Visitor(std::vector<const clang::FunctionDecl*>& q, uint64_t& k,
                        decltype(mixText)& m) : queue(q), key(k), mix(m) {}
                bool VisitDeclRefExpr(clang::DeclRefExpr* e)
                {
                    if (const auto* f = llvm::dyn_cast<clang::FunctionDecl>(e->getDecl()))
                        if (!f->hasExternalFormalLinkage()) queue.push_back(f);
                    if (const auto* v = llvm::dyn_cast<clang::VarDecl>(e->getDecl()))
                        if (v->isFileVarDecl() && !v->hasExternalFormalLinkage())
                        {
                            clang::SourceManager& sm = v->getASTContext().getSourceManager();
                            const clang::PresumedLoc loc = sm.getPresumedLoc(
                                sm.getExpansionLoc(v->getLocation()));
                            if (loc.isValid())
                            {
                                mix(key, loc.getFilename());
                                key = (key ^ loc.getLine()) * 1099511628211ULL;
                                key = (key ^ loc.getColumn()) * 1099511628211ULL;
                            }
                            mix(key, v->getNameAsString());
                        }
                    return true;
                }
            };
            uint64_t key = 14695981039346656037ULL;
            mixText(key, importGroupKey);
            std::set<const clang::FunctionDecl*> seen;
            std::vector<const clang::FunctionDecl*> queue{root};
            while (!queue.empty())
            {
                const clang::FunctionDecl* fd = queue.back();
                queue.pop_back();
                if (fd->getDefinition() != nullptr) fd = fd->getDefinition();
                if (!seen.insert(fd).second) continue;
                const unsigned odr = const_cast<clang::FunctionDecl*>(fd)->getODRHash();
                for (int shift = 0; shift < 32; shift += 8)
                    key = (key ^ ((odr >> shift) & 0xffu)) * 1099511628211ULL;
                if (fd->hasBody())
                {
                    Visitor visitor(queue, key, mixText);
                    visitor.TraverseStmt(fd->getBody());
                }
            }
            return key;
        }

        // True when a constant value is or contains an address (pointer, reference, member
        // pointer, label difference) - something no two import groups can share by value.
        bool ApValueHoldsAddress(const clang::APValue& v)
        {
            switch (v.getKind())
            {
            case clang::APValue::LValue:
            case clang::APValue::MemberPointer:
            case clang::APValue::AddrLabelDiff:
                return true;
            case clang::APValue::Struct:
                for (unsigned i = 0; i < v.getStructNumBases(); ++i)
                    if (ApValueHoldsAddress(v.getStructBase(i))) return true;
                for (unsigned i = 0; i < v.getStructNumFields(); ++i)
                    if (ApValueHoldsAddress(v.getStructField(i))) return true;
                return false;
            case clang::APValue::Union:
                return v.getUnionField() != nullptr && ApValueHoldsAddress(v.getUnionValue());
            case clang::APValue::Array:
                for (unsigned i = 0; i < v.getArrayInitializedElts(); ++i)
                    if (ApValueHoldsAddress(v.getArrayInitializedElt(i))) return true;
                return v.hasArrayFiller() && ApValueHoldsAddress(v.getArrayFiller());
            default:
                return false;
            }
        }

        std::string StaticCxxGlobalAlias(const std::string& importGroupKey,
                                         const std::string& linkageName)
        {
            uint64_t key = 14695981039346656037ULL;
            for (unsigned char c : importGroupKey)
                key = (key ^ c) * 1099511628211ULL;
            return std::format("__cflat_sv_{:016x}_{}", key, linkageName);
        }

        // Rename a per-group static; a COFF comdat keyed by the old name moves with it (with its
        // associative members), since a comdat needs a leader symbol of its own name.
        void RenamePerGroupGlobal(llvm::GlobalVariable* gv, const std::string& newName)
        {
            llvm::Comdat* old = gv->getComdat();
            const std::string oldName = gv->getName().str();
            gv->setName(newName);
            if (old == nullptr || old->getName() != oldName) return;
            llvm::Module& mod = *gv->getParent();
            llvm::Comdat* renamed = mod.getOrInsertComdat(gv->getName());
            renamed->setSelectionKind(old->getSelectionKind());
            for (llvm::GlobalObject& go : mod.global_objects())
                if (go.getComdat() == old) go.setComdat(renamed);
        }

        // A bindable entry for an internal function: the body and its in-module callers keep the
        // internal copy, so this companion's static is never merged with another module's.
        void AddStaticInlineProgramThunk(llvm::Module& mod, llvm::Function* fn,
                                         const std::string& programName)
        {
            if (fn == nullptr || fn->isDeclaration() || mod.getNamedValue(programName) != nullptr)
                return;
            auto* thunk = llvm::Function::Create(fn->getFunctionType(),
                                                 llvm::GlobalValue::WeakODRLinkage,
                                                 programName, &mod);
            thunk->setCallingConv(fn->getCallingConv());
            thunk->setAttributes(fn->getAttributes());
            thunk->setVisibility(llvm::GlobalValue::DefaultVisibility);
            // COFF: a weak_odr definition in two objects is a duplicate unless it is in a comdat
            // (clang always pairs them there). Mach-O has no comdats and merges weak_odr itself.
            if (mod.getTargetTriple().supportsCOMDAT())
                thunk->setComdat(mod.getOrInsertComdat(programName));
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(mod.getContext(), "entry", thunk));
            std::vector<llvm::Value*> callArgs;
            for (llvm::Argument& arg : thunk->args()) callArgs.push_back(&arg);
            llvm::CallInst* call = builder.CreateCall(fn, callArgs);
            call->setCallingConv(fn->getCallingConv());
            call->setAttributes(fn->getAttributes());
            if (thunk->getReturnType()->isVoidTy()) builder.CreateRetVoid();
            else builder.CreateRet(call);
        }

        const char kProbePrefix[] = "__cflat_macro_";

        std::string CanonicalSpelling(const ASTContext& ctx, QualType qt)
        {
            QualType canonical = qt.getCanonicalType();
            PrintingPolicy policy(ctx.getLangOpts());
            policy.FullyQualifiedName = true;
            policy.SuppressScope = false;
            policy.PrintAsCanonical = true;
            std::string s = canonical.getAsString(policy);
            // C++ prints an enum type as a bare name ("cppi::Mode"), where C prints "enum X".
            // The type mapper keys the int decay on the tag, so restore it.
            if (canonical->getAs<EnumType>() != nullptr && s.rfind("enum ", 0) != 0
                && s.rfind("const enum ", 0) != 0)
                s.insert(s.rfind("const ", 0) == 0 ? 6 : 0, "enum ");
            return s;
        }

        /*
         * A generated wrapper's arithmetic parameter that clang CONVERTS into a temporary bound
         * to a `const S&` parameter of the callee it selected: that temporary lives in the
         * wrapper frame, so a result keeping the reference dangles once the wrapper returns.
         * Returns S per wrapper parameter ("" where none); empty when no parameter is converted.
         */
        std::vector<std::string> WrapperConvertedTemporaryTypes(const ASTContext& ctx,
                                                                const FunctionDecl* fd)
        {
            std::vector<std::string> out;
            const Stmt* body = fd != nullptr ? fd->getBody() : nullptr;
            if (body == nullptr) return out;
            auto visit = [&](auto&& self, const Stmt* s) -> void {
                if (s == nullptr) return;
                if (const auto* temp = llvm::dyn_cast<MaterializeTemporaryExpr>(s);
                    temp != nullptr && temp->isBoundToLvalueReference()
                    && temp->getType()->isArithmeticType() && !temp->getType()->isEnumeralType())
                {
                    const Expr* e = temp->getSubExpr();
                    bool converted = false;
                    while (e != nullptr)
                    {
                        e = e->IgnoreParens();
                        const auto* cast = llvm::dyn_cast<CastExpr>(e);
                        if (cast == nullptr) break;
                        const CastKind kind = cast->getCastKind();
                        if (kind == CK_IntegralCast || kind == CK_IntegralToFloating
                            || kind == CK_FloatingToIntegral || kind == CK_FloatingCast
                            || kind == CK_IntegralToBoolean || kind == CK_FloatingToBoolean)
                            converted = true;
                        else if (kind != CK_LValueToRValue && kind != CK_NoOp)
                            break;
                        e = cast->getSubExpr();
                    }
                    const auto* ref = llvm::dyn_cast_or_null<DeclRefExpr>(e);
                    const auto* parm = ref != nullptr
                        ? llvm::dyn_cast<ParmVarDecl>(ref->getDecl()) : nullptr;
                    if (converted && parm != nullptr && parm->getDeclContext() == fd
                        && parm->getType().getNonReferenceType()->isArithmeticType())
                    {
                        const unsigned index = parm->getFunctionScopeIndex();
                        if (out.size() < fd->getNumParams()) out.resize(fd->getNumParams());
                        if (index < out.size())
                            out[index] = CanonicalSpelling(ctx,
                                temp->getType().getUnqualifiedType());
                    }
                }
                for (const Stmt* child : s->children()) self(self, child);
            };
            visit(visit, body);
            return out;
        }

        // The selected callee, exactly: qualified name, specialized type, and primary pattern.
        std::string CalleeIdentity(const ASTContext& ctx, const FunctionDecl* callee)
        {
            if (callee == nullptr) return {};
            std::string identity = callee->getQualifiedNameAsString() + "|"
                + CanonicalSpelling(ctx, callee->getType());
            if (const FunctionTemplateDecl* primary = callee->getPrimaryTemplate())
                identity += "|" + CanonicalSpelling(ctx, primary->getTemplatedDecl()->getType());
            return identity;
        }

        /*
         * A generated wrapper passing a string literal as its own array lvalue
         * (`*reinterpret_cast<const char (*)[N]>(pK)`) to a callee whose `X *const &` (or `X *&&`)
         * binds the DECAYED pointer: that temporary lives in the wrapper frame, where C++ keeps it
         * to the end of the caller's full-expression. Reports the temporary's type in
         * `temporaries[K]` (beside WrapperConvertedTemporaryTypes) and the selected callee's
         * identity. A wrapper passing a reference-to-pointer parameter straight to a call reports
         * that callee's identity too, so a re-spelled wrapper can prove it selects the same one.
         */
        void WrapperLiteralPointerTemporaries(const ASTContext& ctx, const FunctionDecl* fd,
                                              std::vector<std::string>& temporaries,
                                              std::string& identity)
        {
            const Stmt* body = fd != nullptr ? fd->getBody() : nullptr;
            if (body == nullptr) return;
            auto wrapperParameter = [&](const Expr* e) -> const ParmVarDecl* {
                const auto* ref = llvm::dyn_cast_or_null<DeclRefExpr>(e);
                const auto* parm = ref != nullptr ? llvm::dyn_cast<ParmVarDecl>(ref->getDecl()) : nullptr;
                return parm != nullptr && parm->getDeclContext() == fd ? parm : nullptr;
            };
            // The wrapper parameter K a decayed literal temporary was made from, or null.
            auto decayedLiteral = [&](const MaterializeTemporaryExpr* temp) -> const ParmVarDecl* {
                if (!temp->getType()->isPointerType()) return nullptr;
                const Expr* e = temp->getSubExpr();
                bool decayed = false;
                while (e != nullptr)
                {
                    e = e->IgnoreParens();
                    const auto* cast = llvm::dyn_cast<ImplicitCastExpr>(e);
                    if (cast == nullptr) break;
                    if (cast->getCastKind() == CK_ArrayToPointerDecay) decayed = true;
                    else if (cast->getCastKind() != CK_NoOp && cast->getCastKind() != CK_BitCast)
                        return nullptr;
                    e = cast->getSubExpr();
                }
                const auto* deref = llvm::dyn_cast_or_null<UnaryOperator>(e);
                if (!decayed || deref == nullptr || deref->getOpcode() != UO_Deref) return nullptr;
                e = deref->getSubExpr()->IgnoreParens();
                const auto* reinterpret = llvm::dyn_cast<CXXReinterpretCastExpr>(e);
                if (reinterpret == nullptr) return nullptr;
                e = reinterpret->getSubExpr()->IgnoreParenImpCasts();
                const ParmVarDecl* parm = wrapperParameter(e);
                return parm != nullptr && parm->getType()->isPointerType() ? parm : nullptr;
            };
            auto inspect = [&](const FunctionDecl* callee, const Expr* const* args, unsigned count) {
                for (unsigned i = 0; i < count; ++i)
                {
                    const Expr* arg = args[i];
                    if (arg == nullptr) continue;
                    // IgnoreImplicit would strip the MaterializeTemporaryExpr itself.
                    const Expr* inner = arg->IgnoreParens();
                    if (const auto* temp = llvm::dyn_cast<MaterializeTemporaryExpr>(inner))
                        if (const ParmVarDecl* parm = decayedLiteral(temp))
                        {
                            const unsigned index = parm->getFunctionScopeIndex();
                            if (temporaries.size() < fd->getNumParams())
                                temporaries.resize(fd->getNumParams());
                            if (index < temporaries.size())
                                temporaries[index] = CanonicalSpelling(ctx,
                                    temp->getType().getUnqualifiedType());
                            identity = CalleeIdentity(ctx, callee);
                            continue;
                        }
                    if (const ParmVarDecl* parm = wrapperParameter(arg->IgnoreParenImpCasts());
                        parm != nullptr && parm->getType()->isReferenceType()
                        && parm->getType().getNonReferenceType()->isPointerType()
                        && identity.empty())
                        identity = CalleeIdentity(ctx, callee);
                }
            };
            auto visit = [&](auto&& self, const Stmt* s) -> void {
                if (s == nullptr) return;
                if (const auto* call = llvm::dyn_cast<CallExpr>(s))
                    inspect(call->getDirectCallee(), call->getArgs(), call->getNumArgs());
                else if (const auto* construct = llvm::dyn_cast<CXXConstructExpr>(s))
                    inspect(construct->getConstructor(), construct->getArgs(),
                            construct->getNumArgs());
                for (const Stmt* child : s->children()) self(self, child);
            };
            visit(visit, body);
        }

        /*
         * A generated wrapper that materializes a brace list (`std::max({p0, p1})`) owns what
         * the list became - an initializer_list's backing array, an aggregate or array temporary,
         * a converted container - and all of it dies when the wrapper returns, where C++ keeps it
         * to the end of the caller's full-expression. Whether a result refers into it is not
         * decidable from its type (a pointer to an element's member, a c_str() of a string
         * element, a view with a user-written destructor), so the result is flagged when it can
         * carry any address: a pointer, reference, member pointer, or a class with such a field
         * or base at any depth, a vptr, or an incomplete / too-deep layout. Arithmetic, enum,
         * classes proven pointer-free (pair<int, int>), and std::string (a deep owner) are not.
         * The caller consults the flag for brace-list wrappers only. Any brace wrapper reports,
         * per argument of its returned call, the element type of an initializer_list passed
         * there DIRECTLY ("" otherwise), so the caller can back those lists in its own frame
         * (always when flagged; for a non-trivial element type otherwise).
         */
        /*
         * A brace-list argument that became an array, aggregate or converted container temporary
         * (not a direct initializer_list) whose elements are non-trivially destructible or
         * copyable: it would die in the wrapper, observably early. Reported as this marker.
         */
        constexpr const char* kBraceTemporaryNontrivialMarker = "#nontrivial-brace-temporary";
        bool BraceTemporaryHasNontrivialElements(const ASTContext& ctx, const Expr* argument)
        {
            auto nontrivial = [&](QualType type) {
                if (type.isNull() || type->isDependentType()) return false;
                type = ctx.getBaseElementType(type.getNonReferenceType());
                return type.isDestructedType() != QualType::DK_none
                    || !type.isTriviallyCopyableType(ctx);
            };
            const Expr* inner = argument->IgnoreImplicit();
            if (llvm::isa<InitListExpr>(inner)) return nontrivial(inner->getType());
            const auto* construct = llvm::dyn_cast<CXXConstructExpr>(inner);
            if (construct == nullptr || !construct->isListInitialization()) return false;
            if (construct->getNumArgs() == 0 || nontrivial(construct->getType()) == false)
                return false;
            // A container built over an initializer_list: its elements decide.
            for (const Expr* arg : construct->arguments())
                if (const auto* il = llvm::dyn_cast<CXXStdInitializerListExpr>(arg->IgnoreImplicit()))
                    if (const auto* spec = llvm::dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                            il->getType()->getAsCXXRecordDecl());
                        spec != nullptr && spec->getTemplateArgs().size() == 1
                        && spec->getTemplateArgs()[0].getKind() == TemplateArgument::Type)
                        return nontrivial(spec->getTemplateArgs()[0].getAsType());
            // A class list-initialized from its members' values directly.
            return nontrivial(construct->getType());
        }

        bool WrapperResultBorrowsBraceList(const ASTContext& ctx, const FunctionDecl* fd,
                                           std::vector<std::string>& elementSpellings)
        {
            elementSpellings.clear();
            const auto* body = llvm::dyn_cast_or_null<CompoundStmt>(fd != nullptr ? fd->getBody()
                                                                                   : nullptr);
            if (body == nullptr) return false;
            bool braced = false;
            auto findBraces = [&](auto&& self, const Stmt* st) -> void {
                if (st == nullptr || braced) return;
                const auto* construct = llvm::dyn_cast<CXXConstructExpr>(st);
                if (llvm::isa<InitListExpr>(st) || llvm::isa<CXXStdInitializerListExpr>(st)
                    || (construct != nullptr && construct->isListInitialization()))
                {
                    braced = true;
                    return;
                }
                for (const Stmt* child : st->children()) self(self, child);
            };
            findBraces(findBraces, body);
            if (!braced) return false;
            std::unordered_set<const Type*> proven;
            auto mayCarryAddress = [&](auto&& self, QualType type, int depth) -> bool {
                if (type.isNull()) return false;
                type = ctx.getCanonicalType(type);
                if (depth > 8) return true;
                if (type->isVoidType() || type->isArithmeticType() || type->isEnumeralType()
                    || type->isNullPtrType())
                    return false;
                if (const auto* array = ctx.getAsArrayType(type))
                    return self(self, array->getElementType(), depth + 1);
                const auto* record = type->getAsCXXRecordDecl();
                if (record == nullptr) return true;
                // A std::basic_string of an integer character type with the standard allocator owns
                // its storage: its pointers never refer outside the object, however it was built.
                if (const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(record);
                    spec != nullptr && spec->isInStdNamespace() && spec->getName() == "basic_string")
                {
                    const TemplateArgumentList& args = spec->getTemplateArgs();
                    const auto* alloc = args.size() == 3 && args[2].getKind() == TemplateArgument::Type
                        ? llvm::dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                              args[2].getAsType()->getAsCXXRecordDecl())
                        : nullptr;
                    if (alloc != nullptr && alloc->isInStdNamespace()
                        && alloc->getName() == "allocator"
                        && args[0].getKind() == TemplateArgument::Type
                        && args[0].getAsType()->isIntegerType())
                        return false;
                }
                if (!record->hasDefinition() || record->isDependentType()) return true;
                record = record->getDefinition();
                if (record->isDynamicClass() || record->getNumVBases() != 0) return true;
                if (proven.count(type.getTypePtr()) != 0) return false;
                for (const CXXBaseSpecifier& base : record->bases())
                    if (self(self, base.getType(), depth + 1)) return true;
                for (const FieldDecl* field : record->fields())
                    if (self(self, field->getType(), depth + 1)) return true;
                proven.insert(type.getTypePtr());
                return false;
            };
            for (const Stmt* statement : body->body())
            {
                const auto* ret = llvm::dyn_cast<ReturnStmt>(statement);
                const Expr* value = ret != nullptr ? ret->getRetValue() : nullptr;
                const auto* call = value != nullptr
                    ? llvm::dyn_cast<CallExpr>(value->IgnoreImplicit()) : nullptr;
                if (call == nullptr) continue;
                for (const Expr* argument : call->arguments())
                {
                    const auto* list = llvm::dyn_cast<CXXStdInitializerListExpr>(
                        argument->IgnoreImplicit());
                    if (list == nullptr && BraceTemporaryHasNontrivialElements(ctx, argument))
                    {
                        elementSpellings.push_back(kBraceTemporaryNontrivialMarker);
                        continue;
                    }
                    const auto* spec = list != nullptr
                        ? llvm::dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                              list->getType()->getAsCXXRecordDecl())
                        : nullptr;
                    elementSpellings.push_back(
                        spec != nullptr && spec->getTemplateArgs().size() == 1
                                && spec->getTemplateArgs()[0].getKind() == TemplateArgument::Type
                            ? CanonicalSpelling(ctx, spec->getTemplateArgs()[0].getAsType()
                                                         .getUnqualifiedType())
                            : std::string());
                }
                break;
            }
            if (std::all_of(elementSpellings.begin(), elementSpellings.end(),
                            [](const std::string& e) { return e.empty(); }))
                elementSpellings.clear();
            return mayCarryAddress(mayCarryAddress, fd->getReturnType(), 0);
        }

        std::string CxxQualifiedName(const NamedDecl* d)
        {
            std::string n = d->getQualifiedNameAsString();
            std::replace(n.begin(), n.end(), ':', '.');
            while (n.find("..") != std::string::npos) n.erase(n.find(".."), 1);
            return n;
        }

        /*
         * Enclosing-NAMESPACE-qualified name. A free operator is looked up by the namespace of
         * its class operand ("std" for std.string), while libc++ declares some of its operators
         * in the versioned inline namespace std::__1, some directly in std, and some as hidden
         * friends inside the class. All three must answer to the same CFlat name, so inline
         * namespaces and record scopes are both skipped and only named namespaces are kept.
         */
        std::string CxxEnclosingNamespaceName(const NamedDecl* d)
        {
            std::vector<std::string> parts;
            for (const DeclContext* dc = d->getDeclContext();
                 dc != nullptr && !dc->isTranslationUnit(); dc = dc->getParent())
            {
                const auto* ns = llvm::dyn_cast<NamespaceDecl>(dc);
                if (ns == nullptr || ns->isInline()) continue;
                if (ns->isAnonymousNamespace()) return std::string();
                parts.push_back(ns->getNameAsString());
            }
            std::string out;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += *it + ".";
            return out + d->getNameAsString();
        }

        // Clang spells an unnamed enclosing scope as "(anonymous namespace)", "(unnamed struct
        // ...)" or "f()::Local". Those collapse to a dotted name CFlat can neither parse nor
        // look up, and none of them is externally linkable, so the decl is dropped instead.
        bool IsValidDottedName(const std::string& n)
        {
            if (n.empty()) return false;
            bool startOfComponent = true;
            for (char c : n)
            {
                if (c == '.')
                {
                    if (startOfComponent) return false;
                    startOfComponent = true;
                    continue;
                }
                bool ok = (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (!startOfComponent && c >= '0' && c <= '9');
                if (!ok) return false;
                startOfComponent = false;
            }
            return !startOfComponent;
        }

        /*
         * The infix and compound-assignment operators CFlat has a spelling for, as a free
         * function. Unary, subscript, call and conversion forms stay out.
         */
        bool IsBindableFreeBinaryOperator(OverloadedOperatorKind kind)
        {
            switch (kind)
            {
                case OO_Plus: case OO_Minus: case OO_Star: case OO_Slash: case OO_Percent:
                case OO_EqualEqual: case OO_ExclaimEqual:
                case OO_Less: case OO_Greater: case OO_LessEqual: case OO_GreaterEqual:
                case OO_Spaceship:
                case OO_LessLess: case OO_GreaterGreater:
                case OO_Amp: case OO_Pipe: case OO_Caret:
                case OO_AmpAmp: case OO_PipePipe:
                case OO_PlusEqual: case OO_MinusEqual: case OO_StarEqual:
                case OO_SlashEqual: case OO_PercentEqual:
                case OO_LessLessEqual: case OO_GreaterGreaterEqual:
                case OO_AmpEqual: case OO_PipeEqual: case OO_CaretEqual:
                    return true;
                default: return false;
            }
        }

        // The unary operators CFlat spells `- + ! ~`, as a free function of ONE parameter.
        bool IsBindableFreeUnaryOperator(OverloadedOperatorKind kind)
        {
            return kind == OO_Plus || kind == OO_Minus || kind == OO_Exclaim || kind == OO_Tilde;
        }

        // A free operator function TEMPLATE the operator lookup can bind: binary or unary.
        bool IsBindableFreeOperatorTemplate(const FunctionDecl* fd)
        {
            if (fd == nullptr || llvm::isa<CXXMethodDecl>(fd)) return false;
            return (fd->getNumParams() == 2 && IsBindableFreeBinaryOperator(fd->getOverloadedOperator()))
                || (fd->getNumParams() == 1 && IsBindableFreeUnaryOperator(fd->getOverloadedOperator()));
        }

        /*
         * Whether a type names a class member that is not public (a protected / private nested
         * class, or a specialization over one). Code at namespace scope cannot spell it, so a
         * generated default-argument wrapper over it does not compile.
         */
        bool NamesNonPublicMember(QualType type, unsigned depth = 0)
        {
            if (type.isNull() || depth > 16) return false;
            type = type.getCanonicalType();
            if (const auto* ref = type->getAs<ReferenceType>())
                return NamesNonPublicMember(ref->getPointeeType(), depth + 1);
            if (!type->getPointeeType().isNull())
                return NamesNonPublicMember(type->getPointeeType(), depth + 1);
            if (const auto* array = type->getAsArrayTypeUnsafe())
                return NamesNonPublicMember(array->getElementType(), depth + 1);
            if (const auto* proto = type->getAs<FunctionProtoType>())
            {
                if (NamesNonPublicMember(proto->getReturnType(), depth + 1)) return true;
                for (QualType param : proto->getParamTypes())
                    if (NamesNonPublicMember(param, depth + 1)) return true;
                return false;
            }
            const TagDecl* tag = type->getAsTagDecl();
            for (const Decl* decl = tag; decl != nullptr;
                 decl = llvm::dyn_cast<TagDecl>(decl->getDeclContext()))
            {
                if (llvm::isa<RecordDecl>(decl->getDeclContext())
                    && (decl->getAccess() == AS_private || decl->getAccess() == AS_protected))
                    return true;
                const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(decl);
                if (spec == nullptr) continue;
                for (const TemplateArgument& arg : spec->getTemplateArgs().asArray())
                {
                    if (arg.getKind() == TemplateArgument::Type
                        && NamesNonPublicMember(arg.getAsType(), depth + 1))
                        return true;
                    if (arg.getKind() == TemplateArgument::Pack)
                        for (const TemplateArgument& inner : arg.pack_elements())
                            if (inner.getKind() == TemplateArgument::Type
                                && NamesNonPublicMember(inner.getAsType(), depth + 1))
                                return true;
                }
            }
            return false;
        }

        RawDefaultArg DefaultArgumentOf(const ParmVarDecl* p, ASTContext& ctx)
        {
            RawDefaultArg result;
            if (p == nullptr || !p->hasDefaultArg()) return result;
            // A template member default stays uninstantiated until some call uses it. Read the
            // pattern so the answer does not depend on whether this TU happened to use it.
            const Expr* init = p->hasUninstantiatedDefaultArg()
                ? p->getUninstantiatedDefaultArg() : p->getDefaultArg();
            if (init == nullptr || init->containsErrors() || init->isInstantiationDependent())
            {
                result.kind = "nonconst";
                return result;
            }
            init = init->IgnoreParenImpCasts();
            // `= nullptr`, `= NULL` (clang's `__null`), `= 0` on a pointer parameter all mean the
            // null pointer; a pointer default that is anything else is not a constant here.
            if (llvm::isa<CXXNullPtrLiteralExpr>(init)
                || (p->getType()->isAnyPointerType()
                    && init->isNullPointerConstant(ctx, Expr::NPC_ValueDependentIsNotNull)
                           != Expr::NPCK_NotNull))
            {
                result.kind = "nullptr";
                result.value = "nullptr";
                return result;
            }
            if (p->getType()->isAnyPointerType() || p->getType()->isMemberPointerType())
            {
                result.kind = "nonconst";
                return result;
            }
            Expr::EvalResult ev;
            if (!init->EvaluateAsRValue(ev, ctx))
            {
                result.kind = "nonconst";
                return result;
            }
            if (ev.Val.isInt())
            {
                const bool isSigned = p->getType()->isSignedIntegerOrEnumerationType();
                llvm::SmallString<64> text;
                ev.Val.getInt().toString(text, 10, isSigned);
                result.value = text.str().str();
                result.kind = p->getType()->isBooleanType() ? "bool"
                    : p->getType()->isEnumeralType() ? "enum" : "int";
                return result;
            }
            if (ev.Val.isFloat())
            {
                llvm::APFloat f = ev.Val.getFloat();
                bool losesInfo = false;
                f.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven,
                          &losesInfo);
                result.kind = p->getType()->isSpecificBuiltinType(BuiltinType::Float)
                    ? "float" : "double";
                result.value = std::format("{:.17g}", f.convertToDouble());
                return result;
            }
            result.kind = "nonconst";
            return result;
        }

        std::string CxxLinkageName(ASTContext& ctx, const FunctionDecl* fd)
        {
            auto mangle = std::unique_ptr<MangleContext>(ctx.createMangleContext());
            llvm::SmallString<128> storage;
            llvm::raw_svector_ostream os(storage);
            mangle->mangleName(GlobalDecl(fd), os);
            return os.str().str();
        }

        // Structor linkage names must name the COMPLETE-object variant; a bare FunctionDecl
        // GlobalDecl has no variant and MangleContext asserts on it.
        std::string CxxLinkageName(ASTContext& ctx, GlobalDecl gd)
        {
            auto mangle = std::unique_ptr<MangleContext>(ctx.createMangleContext());
            llvm::SmallString<128> storage;
            llvm::raw_svector_ostream os(storage);
            mangle->mangleName(gd, os);
            return os.str().str();
        }

        // The symbol CodeGen gives GD: its mangled name, or the plain name for C linkage.
        std::string DemandSymbolName(MangleContext& mangle, GlobalDecl gd)
        {
            const auto* named = llvm::cast<NamedDecl>(gd.getDecl());
            if (!mangle.shouldMangleDeclName(named)) return named->getNameAsString();
            llvm::SmallString<128> storage;
            llvm::raw_svector_ostream os(storage);
            mangle.mangleName(gd, os);
            return os.str().str();
        }

        // A global-scope C variable keeps its plain name; mangleName would spell it `_Z...`
        // anyway, so the "should this be mangled at all" question has to be asked first.
        std::string CxxLinkageName(ASTContext& ctx, const VarDecl* vd)
        {
            auto mangle = std::unique_ptr<MangleContext>(ctx.createMangleContext());
            if (!mangle->shouldMangleDeclName(vd)) return vd->getNameAsString();
            llvm::SmallString<128> storage;
            llvm::raw_svector_ostream os(storage);
            mangle->mangleName(GlobalDecl(vd), os);
            return os.str().str();
        }

        int MapAccess(AccessSpecifier a)
        {
            if (a == AS_private)   return AccessPrivate;
            if (a == AS_protected) return AccessProtected;
            return AccessPublic;
        }

        bool DeclIsNoexcept(const FunctionDecl* fd)
        {
            ExceptionSpecificationType est = fd->getExceptionSpecType();
            return est == EST_BasicNoexcept || est == EST_NoexceptTrue || est == EST_NoThrow;
        }

        /*
         * The complete-object GlobalDecl for a member: structors need their variant, everything
         * else is the plain decl. Under the Microsoft ABI Dtor_Complete mangles to the "vbase
         * destructor" (`??_D`), which clang emits ONLY for a class with virtual bases; every
         * other class has just the base destructor (`??1`), and that is the symbol a complete
         * destruction calls. Naming Dtor_Complete there yields a linkage name no module ever
         * defines, so stage 2 finds no body and the whole class is refused as a local.
         */
        GlobalDecl MemberGlobalDecl(const CXXMethodDecl* md)
        {
            if (const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md))
                return GlobalDecl(ctor, Ctor_Complete);
            if (const auto* dtor = llvm::dyn_cast<CXXDestructorDecl>(md))
            {
                const bool microsoft = md->getASTContext().getTargetInfo().getCXXABI().isMicrosoft();
                const bool baseIsComplete = microsoft && md->getParent()->getNumVBases() == 0;
                return GlobalDecl(dtor, baseIsComplete ? Dtor_Base : Dtor_Complete);
            }
            return GlobalDecl(md);
        }

        // Treat same-class `T&&` overloads as move members when Clang does not flag templates.
        bool IsSameClassRvalueParameter(const CXXMethodDecl* md)
        {
            if (md == nullptr || md->getNumParams() != 1) return false;
            QualType param = md->getParamDecl(0)->getType();
            if (!param->isRValueReferenceType()) return false;
            const auto* rhs = param->getPointeeType()->getAsCXXRecordDecl();
            return rhs != nullptr
                && rhs->getCanonicalDecl() == md->getParent()->getCanonicalDecl();
        }

        // False only when Sema PROVES the trailing requires-clause unsatisfied.
        static bool ConstrainedMemberSatisfied(Sema& sema, const CXXMethodDecl* md)
        {
            if (md == nullptr || !md->getTrailingRequiresClause() || md->isDependentContext())
                return true;
            clang::ConstraintSatisfaction satisfaction;
            if (sema.CheckFunctionConstraints(md, satisfaction))
                return true;
            return satisfaction.IsSatisfied;
        }

        /*
         * Two satisfied non-template members with the same parameter list and object qualifiers
         * differ only by constraints: overload resolution always picks the more constrained one
         * ([over.match.best]), so the other is never callable. iota_view's constrained end()
         * returning the iterator hides the one returning its sentinel. MSVC does not mangle the
         * constraint, so there the two also share one symbol name (elements_view's operator*).
         * Only twins with no default argument and the same ellipsis are pruned: a default the
         * winner lacks leaves the loser callable when it is omitted, and an ellipsis difference
         * has no constraint tiebreak (both stay, as before this rule).
         */
        static bool LosesToMoreConstrainedTwin(Sema& sema, const CXXMethodDecl* md,
                                               const std::vector<const CXXMethodDecl*>& methods)
        {
            if (md == nullptr || md->isDependentContext() || md->getDescribedFunctionTemplate() != nullptr
                || md->getPrimaryTemplate() != nullptr)
                return false;
            auto sameShape = [](const CXXMethodDecl* a, const CXXMethodDecl* b) {
                if (a->getDeclName() != b->getDeclName() || a->isStatic() != b->isStatic()
                    || a->getNumParams() != b->getNumParams()
                    || a->getMethodQualifiers() != b->getMethodQualifiers()
                    || a->getRefQualifier() != b->getRefQualifier() || a->isVariadic() != b->isVariadic()
                    || a->getMinRequiredArguments() != a->getNumParams()
                    || b->getMinRequiredArguments() != b->getNumParams())
                    return false;
                const ASTContext& astCtx = a->getASTContext();
                for (unsigned i = 0; i < a->getNumParams(); ++i)
                    if (!astCtx.hasSameType(a->getParamDecl(i)->getType(),
                                            b->getParamDecl(i)->getType()))
                        return false;
                return true;
            };
            for (const CXXMethodDecl* other : methods)
            {
                if (other == md || other == nullptr || other->getDescribedFunctionTemplate() != nullptr
                    || other->getPrimaryTemplate() != nullptr
                    || (!md->getTrailingRequiresClause() && !other->getTrailingRequiresClause())
                    || !sameShape(md, other) || !ConstrainedMemberSatisfied(sema, other))
                    continue;
                const FunctionDecl* winner = sema.getMoreConstrainedFunction(
                    const_cast<CXXMethodDecl*>(md), const_cast<CXXMethodDecl*>(other));
                if (winner == other) return true;
            }
            return false;
        }

        /*
         * A member overload resolution can never select (constraint unsatisfied, or the less
         * constrained of two same-signature twins). Deducing its `decltype(auto)` return still
         * instantiates its body, and handing that body to the demand pass beside the selected
         * twin defines one symbol twice.
         */
        static bool IsNonViableConstrainedMember(Sema& sema, const Decl* d)
        {
            const auto* md = llvm::dyn_cast_or_null<CXXMethodDecl>(d);
            if (md == nullptr || md->isDependentContext()) return false;
            if (!ConstrainedMemberSatisfied(sema, md)) return true;
            std::vector<const CXXMethodDecl*> siblings;
            for (const CXXMethodDecl* sibling : md->getParent()->methods()) siblings.push_back(sibling);
            return LosesToMoreConstrainedTwin(sema, md, siblings);
        }

        /*
         * The bound library owns a member's symbol in two cases, and Clang then deliberately
         * emits a reference rather than a body. An EXPLICIT INSTANTIATION DECLARATION
         * (`extern template class basic_string<char>;`) says so for a template member - but
         * only when the library really exports it: isExternallyVisible() is about LINKAGE, so a
         * _LIBCPP_HIDE_FROM_ABI member (hidden, excluded from the instantiation, absent from
         * libc++.dylib) satisfies it while having no symbol anywhere, which would turn a
         * compile-time refusal into a link-time "undefined symbol". A DLLIMPORT member of an
         * exported MSVC class (`ios_base::good`) is the Microsoft counterpart: MSVC exports every
         * member of a dllexport class, inline ones included, and Clang never emits a dllimport
         * body, so the import library's thunk is the only definition.
         */
        static bool LibraryOwnsMemberSymbol(const CXXMethodDecl* md)
        {
            if (md->hasAttr<clang::DLLImportAttr>()
                && md->getASTContext().getTargetInfo().getCXXABI().isMicrosoft())
                return true;
            return md->getTemplateSpecializationKind() == clang::TSK_ExplicitInstantiationDeclaration
                && md->isExternallyVisible()
                && md->getVisibility() == clang::DefaultVisibility
                && !md->hasAttr<clang::ExcludeFromExplicitInstantiationAttr>();
        }

        // APSInt -> long long. Signed values sign-extend (they always fit in int64); unsigned
        // values may exceed INT64_MAX (e.g. ~0ULL), so zero-extend and bit-reinterpret rather
        // than call getSExtValue, which asserts isRepresentableByInt64 for those.
        long long ApsIntToLongLong(const llvm::APSInt& v)
        {
            return v.isSigned() ? v.getSExtValue() : static_cast<long long>(v.getZExtValue());
        }

        double ApFloatToDouble(const llvm::APFloat& value, bool* losesInfo = nullptr)
        {
            // convertToDouble() requires IEEEdouble semantics. This also rounds x87 or other
            // long-double formats instead of asserting when the target uses wider semantics.
            llvm::APFloat converted = value;
            bool ignored = false;
            const llvm::APFloat::opStatus status = converted.convert(
                llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &ignored);
            if (losesInfo != nullptr)
                *losesInfo = (status & llvm::APFloat::opInexact) != 0;
            return converted.convertToDouble();
        }

        std::string NormPath(std::string p)
        {
            std::replace(p.begin(), p.end(), '\\', '/');
            std::transform(p.begin(), p.end(), p.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            // Collapse repeated separators. Clang's MSVC toolchain detection emits paths with
            // doubled separators (e.g. "Windows Kits/10//include/.../um/foo.h"); without this the
            // prefix compare in PathInScope fails to match the single-separator in-scope dir.
            std::string out;
            out.reserve(p.size());
            for (char c : p)
            {
                if (c == '/' && !out.empty() && out.back() == '/') continue;
                out.push_back(c);
            }
            return out;
        }

        // dirs must already be normalized (via NormPath) and have trailing '/' stripped.
        bool PathInScope(const std::string& path, const std::vector<std::string>& normDirs)
        {
            if (normDirs.empty()) return true;
            std::string np = NormPath(path);
            for (const auto& nd : normDirs)
            {
                if (nd.empty() || np.size() < nd.size()) continue;
                if (!np.starts_with(nd)) continue;
                // Require a separator boundary so ".../um" does not match ".../umbra/...".
                if (np.size() == nd.size() || np[nd.size()] == '/') return true;
            }
            return false;
        }

        class PrereqDiagConsumer : public DiagnosticConsumer
        {
        public:
            unsigned prereqErrors = 0;
            std::string firstPrereqError;
            std::string firstError;
            // Every error with its presumed location, so a record whose definition failed to
            // compile can name the clang diagnostic that broke it (capped: a broken TU cascades).
            struct ErrorNote
            {
                std::string message;
                std::string file;
                unsigned line = 0;
                bool inMainFile = false;   // raised in the in-memory stub, not a header
            };
            std::vector<ErrorNote> errors;

            void HandleDiagnostic(DiagnosticsEngine::Level level, const Diagnostic& info) override
            {
                // Deliberately do NOT chain to DiagnosticConsumer::HandleDiagnostic: the base
                // increments NumErrors, and CompilerInstance::ExecuteAction returns false when
                // the client reports errors - which would turn the intentional macro-probe
                // errors into a whole-extraction failure. Like IgnoringDiagConsumer, we swallow
                // the diagnostic and only keep our own prerequisite tally.
                if (level < DiagnosticsEngine::Error) return;

                llvm::SmallString<256> msg;
                info.FormatDiagnostic(msg);
                if (firstError.empty()) firstError = msg.str().str();
                if (errors.size() < 64)
                {
                    ErrorNote note{ msg.str().str(), std::string(), 0, false };
                    if (info.hasSourceManager() && info.getLocation().isValid())
                    {
                        const SourceManager& sm = info.getSourceManager();
                        PresumedLoc pl = sm.getPresumedLoc(info.getLocation());
                        if (pl.isValid()) { note.file = pl.getFilename(); note.line = pl.getLine(); }
                        note.inMainFile = sm.isInMainFile(info.getLocation());
                    }
                    errors.push_back(std::move(note));
                }
                if (msg.str().find("unknown type name") == llvm::StringRef::npos) return;

                // Errors in the in-memory stub (the macro probes) are not header prerequisites.
                if (info.hasSourceManager())
                {
                    const SourceManager& sm = info.getSourceManager();
                    if (info.getLocation().isValid() && sm.isInMainFile(info.getLocation()))
                        return;
                }

                ++prereqErrors;
                if (firstPrereqError.empty())
                {
                    firstPrereqError = msg.str().str();
                    // Name the header line so a failure deep in a library is locatable.
                    if (info.hasSourceManager() && info.getLocation().isValid())
                    {
                        PresumedLoc pl = info.getSourceManager().getPresumedLoc(info.getLocation());
                        if (pl.isValid())
                            firstPrereqError += std::format(" at {}:{}", pl.getFilename(), pl.getLine());
                    }
                }
            }
        };

        struct ExtractState
        {
            const ExtractRequest& req;
            ExtractResult& out;
            std::vector<CxxMacroProbe> probes;   // index == probe slot
            std::unordered_set<unsigned> emittedProbes;  // probe slots that produced a RawMacro
            std::unordered_set<std::string> emittedGlobals;  // dedup global var redeclarations by name
            std::unordered_set<const BaseUsingDecl*> emittedUsingDecls;
            std::unordered_set<std::string> emittedOpaqueForward;  // dedup opaque forward-decl records by tag
            std::unordered_set<const RecordDecl*> emittedDefinedRecords;
            std::unordered_set<std::string> emittedRequestedRecords;
            std::vector<std::string> normDirs; // req.inScopeDirs normalized once (NormPath + trailing-/ stripped)
            // PathInScope is a pure function of the path (normDirs is fixed once the state is built),
            // and a header walk asks about the same few files once per declaration.
            struct PathHash
            {
                using is_transparent = void;
                size_t operator()(std::string_view v) const { return std::hash<std::string_view>{}(v); }
            };
            std::unordered_map<std::string, bool, PathHash, std::equal_to<>> scopeMemo;
            bool InScope(std::string_view path)
            {
                if (normDirs.empty()) return true;
                auto it = scopeMemo.find(path);
                if (it != scopeMemo.end()) return it->second;
                const std::string owned(path);
                const bool in = PathInScope(owned, normDirs);
                scopeMemo.emplace(owned, in);
                return in;
            }
            bool InScope(const char* path) { return InScope(std::string_view(path != nullptr ? path : "")); }
            const NamedDecl* scopeSentinel = nullptr;
            // Set in BeginSourceFileAction so the ABI pass can build a CodeGenerator against the
            // very invocation that produced the AST (same triple, same target features).
            CompilerInstance* ci = nullptr;
            TranslationUnitDecl* contextRoot = nullptr;
            // cxxMode only: (index into out.sigs, the decl it came from). Resolved after the
            // traversal so a single CodeGenerator serves every declaration.
            std::vector<std::pair<size_t, const FunctionDecl*>> abiWork;
            std::vector<QualType> functionPointerAbiWork;
            std::unordered_set<std::string> functionPointerAbiSeen;
            std::unordered_set<const FunctionTemplateDecl*> emittedFunctionTemplates;
            std::unordered_set<std::string> emittedClassTemplateNames;
            // cxxMode only: (index into out.records, index into that record's members, decl).
            struct MemberAbiWork { size_t recordIdx; size_t memberIdx; const CXXMethodDecl* md; };
            std::vector<MemberAbiWork> memberAbiWork;
            // R1 completion: methods already defined before it ran (their facts stay the harvest's).
            std::unordered_set<const FunctionDecl*> completionPredefined;
            // Plain C++ header records whose implicit special members must be materialized before
            // CollectCxxMembers walks the record's methods.
            std::vector<const CXXRecordDecl*> headerSpecialMemberWork;
            // Hidden friend operators of a requested record, published after the walk.
            std::vector<FunctionDecl*> pendingFriendOps;
            // Bound static data members of an implicit class-template specialization whose
            // definition Sema has not instantiated yet; instantiated and emitted after the walk.
            std::vector<VarDecl*> pendingStaticVarDefs;
            std::unordered_set<const CXXRecordDecl*> headerSpecialMemberSeen;
            /*
             * Every decl Sema ANNOUNCED to the consumer, in order. This is the set a real compile's
             * CodeGen sees, and it is strictly larger than the translation unit's own decls():
             * an implicit function-template instantiation (`std::__to_address<int>`, `std::max<T>`)
             * is announced when Sema instantiates it but is not a child of its namespace. Without
             * replaying these, a member body emitted into the companion module calls helpers whose
             * definitions were never emitted and the link fails on them.
             */
            std::vector<Decl*> announcedDecls;
            /*
             * The subset of announcedDecls this request itself produced: its own chunk and what
             * Sema instantiated while parsing it. In a live Interpreter announcedDecls also
             * replays the shared header and prelude roots, and every specialization any EARLIER
             * request instantiated hangs off those templates with its used-bit set. Promotion
             * of used helpers walks only this list, or each request's module would carry every
             * body the group ever emitted - and a cache entry keyed on one request would then
             * link another program's thunks.
             */
            std::vector<Decl*> requestDecls;
            // The rest of announcedDecls: the shared header and prelude roots of a live
            // Interpreter. Their used helpers are registered lazily, never forced.
            std::vector<Decl*> sharedDecls;
            // emitDefinitions only: polymorphic classes whose vtable/RTTI Clang must emit, and
            // inline / constexpr static data members whose storage lives in the companion module.
            std::vector<const CXXRecordDecl*> vtableWork;
            std::vector<const VarDecl*> varEmitWork;
            struct IncompleteCxxType
            {
                QualType type;
                std::string spelling;
            };
            std::vector<IncompleteCxxType> incompleteCxxTypes;
            // Spellings still incomplete after the harvest; the QualTypes above die with the
            // CompilerInstance, so anything read after RunAction must use this instead.
            std::vector<std::string> stillIncompleteSpellings;
            ExtractState(const ExtractRequest& r, ExtractResult& o) : req(r), out(o)
            {
                probes = r.cxxMacroProbes;
                for (const auto& d : r.inScopeDirs)
                {
                    std::string nd = NormPath(d);
                    while (!nd.empty() && nd.back() == '/') nd.pop_back();
                    normDirs.push_back(std::move(nd));
                }
            }
        };

        struct HeaderScopeSentinelVisitor
            : RecursiveASTVisitor<HeaderScopeSentinelVisitor>
        {
            ExtractState& st;
            explicit HeaderScopeSentinelVisitor(ExtractState& s) : st(s) {}

            bool VisitNamedDecl(NamedDecl* decl)
            {
                if (!st.req.checkHeaderScope || st.scopeSentinel != nullptr
                    || decl->getName() != kHeaderScopeSentinel || st.ci == nullptr)
                    return true;
                if (st.ci->getSourceManager().isInMainFile(decl->getLocation()))
                    st.scopeSentinel = decl;
                return true;
            }
        };

        void QueueIncompleteCxxType(ExtractState& st, ASTContext& ctx, QualType qt)
        {
            if (!st.req.cxxMode) return;
            // Complete by-value signature types without instantiating their member bodies.
            qt = qt.getCanonicalType();
            if (!qt->isRecordType() || !qt->isIncompleteType()) return;
            const auto* cxx = qt->getAsCXXRecordDecl();
            if (cxx == nullptr || !llvm::isa<ClassTemplateSpecializationDecl>(cxx)) return;
            const auto* specialization = llvm::cast<ClassTemplateSpecializationDecl>(cxx);
            // Nested helper specializations can contain state-machine members whose explicit
            // instantiation is not required for the enclosing type's ABI and may be ill-formed.
            if (specialization->getSpecializedTemplate()->getDeclContext()->isRecord()) return;
            std::string spelling = CanonicalSpelling(ctx, qt);
            if (std::find_if(st.incompleteCxxTypes.begin(), st.incompleteCxxTypes.end(),
                             [&](const ExtractState::IncompleteCxxType& queued) {
                                 return queued.spelling == spelling;
                             }) == st.incompleteCxxTypes.end())
                st.incompleteCxxTypes.push_back({ qt, std::move(spelling) });
        }

        void QueueFunctionPointerAbi(ExtractState& st, ASTContext& ctx, QualType qt)
        {
            if (!st.req.cxxMode) return;
            QualType t = qt.getCanonicalType();
            if (t->isPointerType()) t = t->getPointeeType().getCanonicalType();
            if (t->getAs<FunctionProtoType>() == nullptr) return;
            if (t->isDependentType())
            {
                if (st.req.verbose)
                    std::cout << "[verbose]   skipped dependent function-pointer ABI type '"
                              << t.getAsString(ctx.getPrintingPolicy()) << "'\n";
                return;
            }
            std::string key = CanonicalSpelling(ctx, t);
            if (st.functionPointerAbiSeen.insert(key).second)
                st.functionPointerAbiWork.push_back(t);
        }

        // Prepass PPCallbacks: collect object-like macro names (-> probe list) and reconstruct
        // function-like macros directly. Empty object-like macros (include guards) are skipped.
        struct MacroCollector : public PPCallbacks
        {
            Preprocessor& pp;
            std::unique_ptr<ExtractRequest> ownedReq;
            std::unique_ptr<ExtractState> ownedState;
            ExtractState& st;
            std::shared_ptr<bool> active;
            MacroCollector(Preprocessor& p, ExtractState& s,
                           std::shared_ptr<bool> enabled = {})
                : pp(p), st(s), active(std::move(enabled)) {}
            MacroCollector(Preprocessor& p, const ExtractRequest& req, ExtractResult& out,
                           std::shared_ptr<bool> enabled)
                : pp(p), ownedReq(std::make_unique<ExtractRequest>(req)),
                  ownedState(std::make_unique<ExtractState>(*ownedReq, out)),
                  st(*ownedState), active(std::move(enabled)) {}

            void MacroDefined(const Token& nameTok, const MacroDirective* md) override
            {
                if (active && !*active) return;
                if (!md) return;
                const MacroInfo* mi = md->getMacroInfo();
                if (!mi || mi->isBuiltinMacro()) return;
                const IdentifierInfo* ii = nameTok.getIdentifierInfo();
                if (!ii) return;
                std::string name = ii->getName().str();
                if (name.starts_with("__")) return;
                if (name.starts_with(kProbePrefix)) return;

                SourceManager& sm = pp.getSourceManager();
                PresumedLoc pl = sm.getPresumedLoc(mi->getDefinitionLoc());
                std::string file = (pl.isValid() && pl.getFilename()) ? pl.getFilename() : "";
                int line = pl.isValid() ? (int)pl.getLine() : 1;
                int col = pl.isValid() ? (int)pl.getColumn() : 0;
                if (st.req.requireInScope && !st.InScope(file)) return;

                if (mi->isFunctionLike())
                {
                    RawFuncMacro fm;
                    fm.name = name;
                    for (const IdentifierInfo* p : mi->params())
                        fm.params.push_back(p->getName().str());
                    for (const Token& tok : mi->tokens())
                    {
                        std::string spell = pp.getSpelling(tok);
                        if (!fm.body.empty()) fm.body += ' ';
                        fm.body += spell;
                    }
                    fm.file = file; fm.line = line; fm.col = col;
                    st.out.funcMacros.push_back(std::move(fm));
                    return;
                }

                if (mi->getNumTokens() == 0) return;   // include guard / empty define: nothing to fold

                // A one-token identifier body is an alias spelling; keep the spelling so a
                // macro whose probe cannot fold (a function, extern data, a type) still binds.
                std::string aliasTarget;
                bool targetIsFuncMacro = false;
                if (mi->getNumTokens() == 1 && mi->tokens().front().isAnyIdentifier())
                {
                    const Token& body = mi->tokens().front();
                    aliasTarget = pp.getSpelling(body);
                    if (const IdentifierInfo* bii = body.getIdentifierInfo())
                        if (const MacroInfo* bmi = pp.getMacroInfo(bii))
                            targetIsFuncMacro = bmi->isFunctionLike();
                }

                // Aliasing a function-like macro: the probe would read the bare target name,
                // which clang error-recovers into a bogus constant. Report it, inject no probe.
                if (targetIsFuncMacro)
                {
                    RawMacro m;
                    m.name = name; m.file = file; m.line = line; m.col = col;
                    m.aliasTarget = aliasTarget;
                    m.kind = RawMacro::Skip;
                    st.out.macros.push_back(std::move(m));
                    return;
                }

                CxxMacroProbe mp;
                mp.name = name; mp.file = file; mp.line = line; mp.col = col;
                mp.aliasTarget = aliasTarget;
                st.probes.push_back(std::move(mp));
                if (ownedState) st.out.macroProbes.push_back(st.probes.back());
            }
        };

        struct IncrementalMacroPrepassAction : public PPCallbacks
        {
            MacroCollector collector;
            IncrementalMacroPrepassAction(Preprocessor& pp, const ExtractRequest& req,
                                          ExtractResult& out,
                                          std::shared_ptr<bool> active)
                : collector(pp, req, out, std::move(active)) {}
            void MacroDefined(const Token& nameTok, const MacroDirective* md) override
            {
                collector.MacroDefined(nameTok, md);
            }
        };

        struct PrepassAction : public PreprocessOnlyAction
        {
            ExtractState& st;
            explicit PrepassAction(ExtractState& s) : st(s) {}
            bool BeginSourceFileAction(CompilerInstance& ci) override
            {
                ci.getPreprocessor().addPPCallbacks(
                    std::make_unique<MacroCollector>(ci.getPreprocessor(), st));
                // Brace depth over the expanded tokens: a balanced header leaves the stub's scope
                // sentinel at depth 0. An incremental parse must never see an unbalanced header.
                if (st.req.cxxMode)
                    ci.getPreprocessor().setTokenWatcher([out = &st.out, depth = 0](const Token& tok) mutable {
                        if (tok.is(tok::l_brace)) ++depth;
                        else if (tok.is(tok::r_brace)) --depth;
                        else if (depth != 0 && tok.is(tok::identifier)
                                 && tok.getIdentifierInfo()->getName() == kHeaderScopeSentinel)
                            out->headerScopeOpen = true;
                    });
                return true;
            }
        };

        // Deep header-cache PPCallbacks: record every file the preprocessor enters. The raw
        // list includes the virtual main stub and clang pseudo-files (<built-in>, ...); those
        // are filtered out by the caller when it builds the on-disk dependency list (only
        // paths that resolve to a real file on disk are kept).
        struct IncludeCollector : public PPCallbacks
        {
            Preprocessor& pp;
            ExtractState& st;
            IncludeCollector(Preprocessor& p, ExtractState& s) : pp(p), st(s) {}

            void FileChanged(SourceLocation loc, FileChangeReason reason,
                             SrcMgr::CharacteristicKind, FileID) override
            {
                if (reason != EnterFile) return;
                StringRef fn = pp.getSourceManager().getFilename(loc);
                if (!fn.empty()) st.out.includedFiles.push_back(fn.str());
            }
        };

        /*
         * A by-value class type must be COMPLETE for Clang to arrange it: the target's ABI
         * classifier reads its record layout. An incomplete one has none, and asking for it is a
         * null dereference in an assertions-off build. References and pointers are always fine -
         * they are arranged as pointers. Returns the offending spelling, or empty.
         */
        std::string IncompleteByValueRecord(const ASTContext& ctx, QualType qt)
        {
            QualType t = qt.getCanonicalType();
            if (t->isReferenceType() || t->isPointerType() || t->isVoidType()) return {};
            while (const ConstantArrayType* cat = ctx.getAsConstantArrayType(t))
                t = cat->getElementType().getCanonicalType();
            const auto* rt = t->getAs<RecordType>();
            if (rt == nullptr) return {};
            if (rt->getDecl()->getDefinition() != nullptr) return {};
            return CanonicalSpelling(ctx, t);
        }

        /*
         * True when the incomplete record `qt` names is a member class (or a member class template
         * specialization) whose pattern HAS a definition, so clang instantiates it on demand. A
         * merely declared nested class cannot be completed by any request.
         */
        bool LazyCompletableMemberClass(QualType qt)
        {
            QualType t = qt.getCanonicalType();
            const auto* rd = t->getAsCXXRecordDecl();
            if (rd == nullptr || !llvm::isa<CXXRecordDecl>(rd->getDeclContext())) return false;
            if (const CXXRecordDecl* pattern = rd->getInstantiatedFromMemberClass())
                return pattern->getDefinition() != nullptr;
            if (const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(rd))
            {
                const ClassTemplateDecl* tpl = spec->getSpecializedTemplate();
                while (const ClassTemplateDecl* from = tpl->getInstantiatedFromMemberTemplate())
                    tpl = from;
                return tpl->getTemplatedDecl()->getDefinition() != nullptr;
            }
            return false;
        }

        const CXXRecordDecl* CompleteNonDependentCxxRecord(const CXXRecordDecl* rd)
        {
            if (rd == nullptr || rd->isInvalidDecl() || rd->isDependentType()) return nullptr;
            const CXXRecordDecl* def = rd->getDefinition();
            if (def == nullptr || def->isInvalidDecl() || def->isDependentType()
                || !def->isCompleteDefinition())
                return nullptr;
            return def;
        }

        bool DeferHeaderSpecialMembers(const ExtractState& st);
        bool HasDeferredSpecialMemberWork(ASTContext& ctx, const CXXRecordDecl* cxx);
        void CompleteRequestedSpecialMembers(ExtractState& st, ASTContext& ctx,
                                             const CXXRecordDecl* record);
        void IndexSpecialMemberRecords(ASTContext& ctx, const DeclContext* dc,
            std::unordered_map<std::string, const CXXRecordDecl*>& index);

        struct DeclVisitor : public RecursiveASTVisitor<DeclVisitor>
        {
            ASTContext& ctx;
            SourceManager& sm;
            ExtractState& st;
            const Stmt* skippedBody = nullptr;

            DeclVisitor(ASTContext& c, ExtractState& s) : ctx(c), sm(c.getSourceManager()), st(s) {}

            // Harvested API data comes from declarations, not function statements.
            bool shouldWalkTypesOfTypeLocs() const { return false; }

            bool TraverseFunctionDecl(FunctionDecl* fd)
            {
                if (fd == nullptr || fd->getBody() == nullptr)
                    return RecursiveASTVisitor<DeclVisitor>::TraverseFunctionDecl(fd);
                const Stmt* previous = skippedBody;
                skippedBody = fd->getBody();
                const bool result = RecursiveASTVisitor<DeclVisitor>::TraverseFunctionDecl(fd);
                skippedBody = previous;
                return result;
            }

            bool TraverseStmt(
                Stmt* stmt, RecursiveASTVisitor<DeclVisitor>::DataRecursionQueue* queue = nullptr)
            {
                if (stmt == skippedBody) return true;
                return RecursiveASTVisitor<DeclVisitor>::TraverseStmt(stmt, queue);
            }

            void PrepareHeaderSpecialMembers(const CXXRecordDecl* cxx, bool completing = false);
            std::string InvalidDefinitionRefusal(const CXXRecordDecl* def) const;
            std::string InvalidTypeRequestRefusal(const CXXRecordDecl* def,
                                                  const std::string& spelling) const;

            // Resolve a decl's presumed location WITHOUT applying the in-scope filter. Returns
            // false only on an invalid/unknown location.
            bool LocOfRaw(const Decl* d, std::string& file, int& line, int& col) const
            {
                SourceLocation loc = DeclFileLoc(d);
                if (loc.isInvalid()) return false;
                PresumedLoc pl = sm.getPresumedLoc(loc);
                if (pl.isInvalid()) return false;
                file = pl.getFilename() ? pl.getFilename() : "";
                line = (int)pl.getLine();
                col = (int)pl.getColumn();
                return true;
            }

            // The real file whose text produced `d` - a macro's expansion site, not its definition -
            // when that is not `presumed` (a `#line` name, or the macro's defining header).
            std::string PhysicalFileOf(const Decl* d, const std::string& presumed) const
            {
                SourceLocation loc = d->getLocation();
                if (loc.isInvalid()) return {};
                std::string physical = sm.getFilename(sm.getExpansionLoc(loc)).str();
                return physical == presumed ? std::string() : physical;
            }

            SourceLocation DeclFileLoc(const Decl* d) const
            {
                SourceLocation loc = d->getLocation();
                if (loc.isInvalid()) return loc;
                // Macro-generated declarations (for example ATen's Tensor operators) normally
                // have a spelling location in the defining header. Macro arguments can instead
                // resolve to a pseudo-file such as `<scratch space>`; use the expansion header
                // in that case so the in-scope filter does not drop a real declaration.
                if (loc.isMacroID())
                {
                    SourceLocation spelling = sm.getSpellingLoc(loc);
                    PresumedLoc spellingPl = sm.getPresumedLoc(spelling);
                    const char* spellingFile = spellingPl.isValid() ? spellingPl.getFilename() : nullptr;
                    loc = spellingFile != nullptr && spellingFile[0] != '<'
                        ? spelling : sm.getExpansionLoc(loc);
                }
                return loc;
            }

            // Location plus the in-scope gate: used by functions/enums/globals where an
            // out-of-scope decl must be dropped outright (not part of the bound API surface).
            bool LocOf(const Decl* d, std::string& file, int& line, int& col) const
            {
                if (!LocOfRaw(d, file, line, col)) return false;
                if (st.req.requireInScope && !st.InScope(file)) return false;
                return true;
            }

            void RecordRawFieldLayout(QualType type, RawField& field) const
            {
                // Use Clang's complete-type query directly. This preserves the size and
                // alignment of an unregistered class-template specialization for opaque blobs.
                if (type.isNull() || type->isIncompleteType() || type->isDependentType()
                    || type->isUndeducedType() || type->isSizelessType())
                    return;
                const TypeInfo info = ctx.getTypeInfo(type);
                const uint64_t charWidth = ctx.getCharWidth();
                field.sizeBytes = (info.Width + charWidth - 1) / charWidth;
                field.alignBytes = (info.Align + charWidth - 1) / charWidth;
                if (st.req.verbose && type->isRecordType()
                    && type.getAsString().find('<') != std::string::npos)
                    std::cout << std::format(
                        "[verbose]   raw C++ field layout '{}' size={} align={}\n",
                        type.getAsString(), field.sizeBytes, field.alignBytes);
            }

            bool VisitFunctionTemplateDecl(FunctionTemplateDecl* ftd)
            {
                if (!st.req.cxxMode || ftd == nullptr
                    || !st.emittedFunctionTemplates.insert(ftd).second)
                    return true;
                const auto* fd = ftd->getTemplatedDecl();
                if (fd == nullptr || fd->isVariadic() || !fd->hasExternalFormalLinkage())
                    return true;
                // A FREE binary operator template has no identifier, so the ordinary gate below
                // drops it; libc++ declares every basic_string operator that way.
                const bool isFreeBinaryOperatorTemplate = IsBindableFreeOperatorTemplate(fd);
                const auto* md = llvm::dyn_cast<CXXMethodDecl>(fd);
                if (md != nullptr && md->getAccess() != AS_public) return true;
                if (md == nullptr && fd->getStorageClass() == SC_Static) return true;
                const bool isMemberAssignmentOperatorTemplate = md != nullptr
                    && md->getOverloadedOperator() == OO_Equal
                    && !md->isCopyAssignmentOperator() && !md->isMoveAssignmentOperator();
                // A member operator() template (MSVC STL _Not_fn) is CFlat's `obj(args)`.
                const bool isMemberCallOperatorTemplate = md != nullptr
                    && md->getOverloadedOperator() == OO_Call;
                if (!fd->getIdentifier() && !isFreeBinaryOperatorTemplate
                    && !isMemberCallOperatorTemplate
                    && !isMemberAssignmentOperatorTemplate) return true;

                unsigned typeParameterCount = 0;
                std::string templateParameterKinds;
                for (const NamedDecl* tp : *ftd->getTemplateParameters())
                {
                    const auto* typeParam = llvm::dyn_cast<TemplateTypeParmDecl>(tp);
                    if (typeParam != nullptr)
                    {
                        ++typeParameterCount;
                        templateParameterKinds += typeParam->isParameterPack() ? 'P' : 'T';
                        continue;
                    }
                    // SFINAE helpers such as fmt::to_string's enable_if are non-type
                    // parameters with a default value. They need no explicit CFlat argument.
                    const auto* nonTypeParam = llvm::dyn_cast<NonTypeTemplateParmDecl>(tp);
                    if (nonTypeParam == nullptr || nonTypeParam->isParameterPack()) return true;
                    if (nonTypeParam->hasDefaultArgument())
                    {
                        templateParameterKinds += 'd';
                        continue;
                    }
                    // A non-defaulted non-type parameter must be spellable from CFlat, which
                    // only writes integer literals (std::get<0>, a fixed slot index, ...).
                    if (!nonTypeParam->getType()->isIntegralOrEnumerationType()) return true;
                    templateParameterKinds += 'N';
                }

                std::string file;
                int line = 1, col = 0;
                if (!LocOf(fd, file, line, col))
                {
                    // A NAMESPACE-SCOPE function template declared outside the bound header dirs
                    // is still callable by its qualified CFlat spelling (`std.get<0>(t)` from
                    // libc++'s <tuple>). Publish it so namespace member lookup can reach it.
                    // MEMBER templates stay in-scope-only: they arrive with their owning record,
                    // and an out-of-scope owner is not bound in the first place.
                    if (md != nullptr) return true;
                    if (!LocOfRaw(fd, file, line, col)) return true;
                }
                RawFunctionTemplate result;
                result.kind = md == nullptr ? RawFunctionTemplate::Free
                    : md->isStatic() ? RawFunctionTemplate::StaticMember
                                     : RawFunctionTemplate::InstanceMember;
                // A member of a PARTIAL specialization's pattern (`unique_ptr<_Tp[], _Dp>`) is
                // named through its primary template: clang prints the partial's arguments.
                const auto* partialOwner = md != nullptr
                    ? llvm::dyn_cast<ClassTemplatePartialSpecializationDecl>(md->getParent()) : nullptr;
                // A member template of an instantiation of a PARTIAL specialization (the MSVC
                // STL's tuple<_This, _Rest...>; only its instantiations reach this visitor)
                // answers to its class template's name, as the primary's members do.
                const auto* specOwner = md == nullptr || partialOwner != nullptr ? nullptr
                    : llvm::dyn_cast<ClassTemplateSpecializationDecl>(md->getParent());
                if (specOwner != nullptr
                    && (specOwner->getSpecializationKind() != TSK_ImplicitInstantiation
                        || !llvm::isa<ClassTemplatePartialSpecializationDecl*>(
                               specOwner->getSpecializedTemplateOrPartial())))
                    specOwner = nullptr;
                const std::string ownerName = partialOwner != nullptr
                    ? CxxQualifiedName(partialOwner->getSpecializedTemplate()->getTemplatedDecl())
                    : specOwner != nullptr ? CxxQualifiedName(specOwner->getSpecializedTemplate())
                    : md != nullptr ? CxxQualifiedName(md->getParent()) : std::string();
                result.name = isFreeBinaryOperatorTemplate
                    ? CxxEnclosingNamespaceName(fd)
                    : partialOwner != nullptr || specOwner != nullptr
                        ? ownerName + "." + fd->getNameAsString()
                        : CxxQualifiedName(fd);
                // An operator's name is never a dotted identifier ("std.operator+"); the
                // namespace prefix it carries is validated instead.
                if (isFreeBinaryOperatorTemplate)
                {
                    const size_t dot = result.name.rfind('.');
                    if (result.name.empty()) return true;
                    if (dot != std::string::npos
                        && !IsValidDottedName(result.name.substr(0, dot)))
                        return true;
                }
                else if (isMemberAssignmentOperatorTemplate || isMemberCallOperatorTemplate
                    ? !IsValidDottedName(ownerName)
                    : !IsValidDottedName(result.name)) return true;
                if (md != nullptr)
                {
                    result.owner = ownerName;
                    result.memberName = fd->getNameAsString();
                }
                result.cxxSpelling = "::" + result.name;
                for (size_t pos = 0; (pos = result.cxxSpelling.find('.', pos)) != std::string::npos; )
                {
                    result.cxxSpelling.replace(pos, 1, "::");
                    pos += 2;
                }
                const unsigned declaredArity = (unsigned)fd->getNumParams();
                result.hasParameterPack = declaredArity > 0
                    && llvm::isa<PackExpansionType>(fd->getParamDecl(declaredArity - 1)->getType());
                result.minArity = declaredArity - (result.hasParameterPack ? 1u : 0u);
                result.maxArity = result.hasParameterPack
                    ? std::numeric_limits<unsigned>::max() : result.minArity;
                for (unsigned i = 0; i < declaredArity; ++i)
                {
                    result.parameterNames.push_back(fd->getParamDecl(i)->getNameAsString());
                    result.parameterTypes.push_back(
                        fd->getParamDecl(i)->getType().getAsString());
                    bool forwardingReference = false;
                    unsigned forwardingTemplateParameterIndex = (unsigned)-1;
                    QualType parameterType = fd->getParamDecl(i)->getType();
                    if (const auto* packExpansion = parameterType->getAs<PackExpansionType>())
                        parameterType = packExpansion->getPattern();
                    if (const auto* rvalueReference = parameterType->getAs<RValueReferenceType>())
                        if (const auto* templateParameter = rvalueReference->getPointeeType()
                                ->getAs<TemplateTypeParmType>())
                            for (const NamedDecl* functionParameter : *ftd->getTemplateParameters())
                                if (functionParameter == templateParameter->getDecl())
                                {
                                    forwardingReference = true;
                                    if (const auto* typeParameter =
                                            llvm::dyn_cast<TemplateTypeParmDecl>(functionParameter))
                                        forwardingTemplateParameterIndex = typeParameter->getIndex();
                                    break;
                                }
                    result.forwardingReferenceParameters.push_back(forwardingReference ? 1 : 0);
                    result.forwardingReferenceTemplateParameterIndices.push_back(
                        forwardingTemplateParameterIndex);
                }
                while (result.minArity > 0
                       && fd->getParamDecl(result.minArity - 1)->hasDefaultArg())
                    --result.minArity;
                result.typeParameterCount = typeParameterCount;
                result.templateParameterKinds = std::move(templateParameterKinds);
                result.isConst = md != nullptr && !md->isStatic() && md->isConst();
                result.isNoexcept = DeclIsNoexcept(fd);
                result.access = md == nullptr ? AccessPublic : MapAccess(md->getAccess());
                result.file = file;
                result.physicalFile = PhysicalFileOf(fd, file);
                result.line = line;
                result.col = col;
                st.out.functionTemplates.push_back(std::move(result));
                return true;
            }

            bool VisitClassTemplateDecl(ClassTemplateDecl* ctd)
            {
                // Namespace-scope templates too: a dotted name only feeds the backend's
                // cross-import conflict check, a TU-scope one is also a published group name.
                if (!st.req.cxxMode || ctd == nullptr || ctd->isInvalidDecl()
                    || !ctd->getDeclContext()->isFileContext())
                    return true;
                std::string file;
                int line = 1, col = 0;
                LocOfRaw(ctd, file, line, col);
                if (st.req.requireInScope && !st.InScope(file)) return true;
                const std::string name = CxxQualifiedName(ctd->getTemplatedDecl());
                if (!name.empty() && st.emittedClassTemplateNames.insert(name).second)
                    st.out.classTemplateNames.push_back(name);
                return true;
            }

            bool VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* spec)
            {
                if (!st.req.cxxMode || spec == nullptr || spec->isInvalidDecl()
                    || (spec->getSpecializationKind() != TSK_ExplicitSpecialization
                        && !llvm::isa<ClassTemplatePartialSpecializationDecl>(spec)))
                    return true;
                std::string file;
                int line = 1, col = 0;
                LocOfRaw(spec, file, line, col);
                if (st.req.requireInScope && !st.InScope(file)) return true;
                const std::string name = CxxQualifiedName(spec->getSpecializedTemplate());
                if (!name.empty()
                    && std::find(st.out.classTemplateSpecializations.begin(),
                                 st.out.classTemplateSpecializations.end(), name)
                           == st.out.classTemplateSpecializations.end())
                    st.out.classTemplateSpecializations.push_back(name);
                return true;
            }

            /*
             * A hidden friend operator is reachable only through its own class, and its body is
             * instantiated only on odr-use: an incremental request chunk never writes such a use,
             * so the ADL operator libc++ containers rely on would be lost. Instantiate and publish
             * the queued ones here - after the traversal, since Sema appends decls to the TU.
             */
            void PublishPendingFriendOperators()
            {
                if (st.pendingFriendOps.empty()) return;
                std::vector<FunctionDecl*> pending;
                pending.swap(st.pendingFriendOps);
                if (st.ci != nullptr && st.ci->hasSema())
                {
                    Sema& sema = st.ci->getSema();
                    for (FunctionDecl* fn : pending)
                        if (!fn->hasBody() && !fn->isInvalidDecl())
                            sema.MarkFunctionReferenced(fn->getLocation(), fn,
                                                        /*MightBeOdrUse*/ true);
                    sema.PerformPendingInstantiations();
                }
                for (FunctionDecl* fn : pending)
                    if (fn->hasBody() && !fn->isInvalidDecl()) VisitFunctionDecl(fn);
            }

            /*
             * `template <class T> int TB<T>::K = 9;` is instantiated only on odr-use, which the
             * request chunk never writes, so a bound `TB<int>::K` had no definition to emit and
             * failed at link. Instantiate the queued ones after the walk (Sema appends decls).
             */
            void PublishPendingStaticVarDefs()
            {
                if (st.pendingStaticVarDefs.empty()) return;
                std::vector<VarDecl*> pending;
                pending.swap(st.pendingStaticVarDefs);
                if (st.ci == nullptr || !st.ci->hasSema()) return;
                Sema& sema = st.ci->getSema();
                for (VarDecl* vd : pending)
                    if (vd->getDefinition() == nullptr && !vd->isInvalidDecl())
                        sema.MarkVariableReferenced(vd->getLocation(), vd);
                sema.PerformPendingInstantiations();
                for (VarDecl* vd : pending)
                    if (const VarDecl* def = vd->getDefinition(); def != nullptr && !def->isInvalidDecl())
                        st.varEmitWork.push_back(def);
            }

            bool VisitFunctionDecl(FunctionDecl* fd)
            {
                // A generated `auto` wrapper whose body failed template deduction keeps an
                // undeduced return type in the recovered AST. It is useful for the diagnostic,
                // but CodeGen's ABI arranger cannot inspect it safely.
                if (fd == nullptr || fd->isInvalidDecl() || fd->getReturnType()->isUndeducedType())
                    return true;
                // The PATTERN of a function template is a FunctionDecl too. Its signature can be
                // entirely non-dependent (`template <int N> int nth_of(const vector<int>&)`), so
                // the dependent-type gate below misses it. Publishing it as a plain function
                // produces a call to a symbol that is never instantiated - link error at best.
                if (fd->getDescribedFunctionTemplate() != nullptr) return true;
                if (!st.req.cxxFunctionWrapperNames.empty()
                    && (!fd->getIdentifier()
                        || std::find(st.req.cxxFunctionWrapperNames.begin(),
                                     st.req.cxxFunctionWrapperNames.end(),
                                     fd->getNameAsString())
                               == st.req.cxxFunctionWrapperNames.end()))
                    return true;
                // Non-member overloaded operators participate in ADL and must be published even
                // during the ordinary header walk; unlike a member they have no identifier.
                if (!fd->getIdentifier()
                    && !(st.req.cxxMode && fd->getOverloadedOperator() != OO_None
                         && !llvm::isa<CXXMethodDecl>(fd)))
                    return true;
                if (fd->getDeclContext()->isRecord()
                    && !(st.req.cxxMode
                         && fd->getOverloadedOperator() != OO_None
                         && !llvm::isa<CXXMethodDecl>(fd)))
                    return true;
                // M0-M3 expose free functions. Methods, constructors and operators need the
                // class ABI/lifetime machinery from M4; friend operators are the exception because
                // they are free functions despite being declared inside the class.
                // C/C++ static inline helpers are callable through a per-module definition.
                // Keep ordinary internal functions hidden; the demand path handles these bodies.
                const bool inlineFunction = fd->isInlined() || fd->isInlineSpecified();
                const bool internalInline = inlineFunction
                    && (fd->getStorageClass() == SC_Static || !fd->hasExternalFormalLinkage());
                if ((fd->getStorageClass() == SC_Static || !fd->hasExternalFormalLinkage())
                    && !internalInline) return true;
                if (st.req.definitionsOnly && !fd->isThisDeclarationADefinition()) return true;
                if (st.req.cxxMode && fd->getType()->isDependentType()) return true;
                std::string file; int line = 1, col = 0;
                if (!LocOf(fd, file, line, col)) return true;

                RawSig sig;
                sig.name = st.req.cxxMode ? CxxQualifiedName(fd) : fd->getNameAsString();
                if (st.req.cxxMode && (fd->isInAnonymousNamespace()
                    || (fd->getOverloadedOperator() == OO_None && !IsValidDottedName(sig.name))))
                    return true;
                sig.qualifiedName = sig.name;
                // C declarations link by their own name: leaving this empty keeps
                // CreateFunctionDeclaration free to rename (import program renames 'main').
                sig.linkageName = st.req.cxxMode
                    ? (fd->isExternC() ? fd->getNameAsString() : CxxLinkageName(ctx, fd))
                    : std::string();
                sig.retType = CanonicalSpelling(ctx, fd->getReturnType());
                sig.variadic = fd->isVariadic();
                sig.isCxx = st.req.cxxMode;
                sig.isInline = inlineFunction;
                sig.isStaticInline = internalInline
                    && fd->getStorageClass() == SC_Static;
                sig.isNoexcept = !st.req.cxxMode
                    || fd->getExceptionSpecType() == EST_BasicNoexcept
                    || fd->getExceptionSpecType() == EST_NoexceptTrue
                    || fd->getExceptionSpecType() == EST_NoThrow;
                sig.file = file; sig.line = line; sig.col = col;
                sig.physicalFile = PhysicalFileOf(fd, file);
                /*
                 * A C-like record returned by value (std::div's div_t: anonymous on libc,
                 * `struct _div_t` on MSVC) may live outside the import scope: bind it so the
                 * signature maps - an anonymous one under its typedef spelling.
                 */
                if (st.req.cxxMode)
                    if (const RecordType* rt =
                            fd->getReturnType().getCanonicalType()->getAs<RecordType>())
                    {
                        RecordDecl* rd = rt->getDecl()->getDefinition();
                        const auto* cxx = llvm::dyn_cast_or_null<CXXRecordDecl>(rd);
                        std::string recordName;
                        if (rd != nullptr && (cxx == nullptr || cxx->isCLike()))
                        {
                            if (const TypedefNameDecl* td = rd->getTypedefNameForAnonDecl())
                                recordName = td->getNameAsString();
                            else if (rd->getIdentifier() != nullptr)
                                recordName = CxxQualifiedName(rd);
                        }
                        if (!recordName.empty()) EmitDefinedRecord(rd, recordName);
                    }
                QueueIncompleteCxxType(st, ctx, fd->getReturnType());
                for (const ParmVarDecl* p : fd->parameters())
                {
                    sig.paramTypes.push_back(CanonicalSpelling(ctx, p->getType()));
                    sig.paramNames.push_back(p->getNameAsString());
                    sig.defaultArgs.push_back(DefaultArgumentOf(p, ctx));
                    QueueIncompleteCxxType(st, ctx, p->getType());
                    QueueFunctionPointerAbi(st, ctx, p->getType());
                }
                QueueFunctionPointerAbi(st, ctx, fd->getReturnType());
                if (st.req.cxxMode && !st.req.cxxFunctionWrapperNames.empty())
                {
                    sig.paramTemporaryTypes = WrapperConvertedTemporaryTypes(ctx, fd);
                    WrapperLiteralPointerTemporaries(ctx, fd, sig.paramTemporaryTypes,
                                                     sig.calleeIdentity);
                    sig.resultBorrowsBraceList = WrapperResultBorrowsBraceList(
                        ctx, fd, sig.braceListElementTypes);
                }
                // An immediate ('consteval') function is evaluated by the C++ front end and gets
                // no runtime symbol, so binding it would only fail at link time with a mangled name.
                if (st.req.cxxMode && fd->isConsteval())
                    sig.bindRefusal = std::string("C++ function '") + sig.name
                        + "' is declared 'consteval'; an immediate function has no runtime symbol, "
                          "so cflat cannot call it";
                if (st.req.cxxMode && !fd->isVariadic() && sig.bindRefusal.empty())
                    st.abiWork.emplace_back(st.out.sigs.size(), fd);
                // A C++ static inline is internal to each companion module, like clang's per-TU
                // copy. The program binds a thunk under a body-keyed name (StaticInlineBodyKey).
                if (st.req.cxxMode && internalInline && !sig.linkageName.empty())
                {
                    std::string programName = std::format("__cflat_sl_{:016x}_{}",
                        StaticInlineBodyKey(fd, st.req.cxxImportGroupKey), sig.linkageName);
                    st.out.localInlineAliases.emplace_back(sig.linkageName, programName);
                    sig.linkageName = std::move(programName);
                }
                st.out.sigs.push_back(std::move(sig));
                return true;
            }

            /*
             * A namespace-scope using-declaration (`namespace torch { using at::manual_seed; }`)
             * re-exports a function under a SECOND qualified name. Clang keeps one FunctionDecl,
             * owned by the original namespace, so the ordinary walk only ever publishes
             * `at.manual_seed`; the alias name the header advertises would not resolve. Publish a
             * second signature that differs only in `name` - the linkage name stays the target's,
             * because the alias adds no symbol of its own. Member using-declarations are NOT
             * handled here: those re-expose a base member and CollectCxxMembers already reads the
             * shadow's access.
             */
            bool VisitUsingShadowDecl(UsingShadowDecl* usd)
            {
                if (!st.req.cxxMode || usd == nullptr || usd->isInvalidDecl()) return true;
                if (usd->getDeclContext()->isRecord()) return true;
                // A function-request walk harvests only functions, so the type and value
                // publishers below would add entries that walk never otherwise produces.
                if (st.req.cxxFunctionWrapperNames.empty() && PublishUsingShadowAlias(usd))
                    return true;
                auto* target = llvm::dyn_cast<FunctionDecl>(usd->getTargetDecl());
                if (target == nullptr || llvm::isa<CXXMethodDecl>(target)) return true;
                const std::string aliasName = CxxQualifiedName(usd);
                if (!IsValidDottedName(aliasName)) return true;
                auto filteredWhy = [&]() {
                    if (target->isInvalidDecl()) return std::string("declaration is invalid");
                    if (target->getReturnType()->isUndeducedType())
                        return std::string("return type is undeduced");
                    if (target->getDescribedFunctionTemplate() != nullptr)
                        return std::string("target is a function-template pattern");
                    if (!st.req.cxxFunctionWrapperNames.empty()
                        && (!target->getIdentifier()
                            || std::find(st.req.cxxFunctionWrapperNames.begin(),
                                         st.req.cxxFunctionWrapperNames.end(),
                                         target->getNameAsString())
                                   == st.req.cxxFunctionWrapperNames.end()))
                        return std::string("target was not requested by the wrapper filter");
                    if (!target->getIdentifier())
                        return std::string("target has no bindable name");
                    if (target->getDeclContext()->isRecord())
                        return std::string("target is a member function");
                    if (target->getStorageClass() == SC_Static
                        || !target->hasExternalFormalLinkage())
                        return std::string("target is not externally linkable");
                    if (st.req.definitionsOnly && !target->isThisDeclarationADefinition())
                        return std::string("target is not defined in this translation unit");
                    if (st.req.cxxMode && target->getType()->isDependentType())
                        return std::string("target has a dependent type");
                    std::string file;
                    int line = 1;
                    int col = 0;
                    if (!LocOfRaw(target, file, line, col))
                        return std::string("target has no valid source location");
                    if (st.req.requireInScope && !st.InScope(file))
                        return std::string("target is outside the requested scope");
                    const std::string targetName = CxxQualifiedName(target);
                    if (target->isInAnonymousNamespace() || !IsValidDottedName(targetName))
                        return std::string("target name is not a valid CFlat qualified name");
                    return std::string("target was filtered by C++ extraction rules");
                };
                auto reportFiltered = [&](const std::string& why) {
                    if (st.req.verbose)
                        std::cout << "[verbose]   C++ using-declaration " << aliasName
                                  << " not bound: " << why << "\n";
                };
                const size_t before = st.out.sigs.size();
                VisitFunctionDecl(target);
                if (st.out.sigs.size() != before + 1)
                {
                    if (st.req.verbose) reportFiltered(filteredWhy());
                    return true;
                }
                if (st.out.sigs.back().name == aliasName) return true;
                // VisitFunctionDecl already queued this slot's ABI work against `target`; renaming
                // in place publishes the alias without a duplicate of the target's own signature.
                st.out.sigs.back().name = aliasName;
                st.out.sigs.back().qualifiedName = aliasName;
                return true;
            }

            /*
             * `namespace a { using b::E; }` makes `a::E` a second spelling of `b::E`, never a new
             * entity. Publish it the way the equivalent alias declaration would be: an enum or a
             * typedef-name as a typedef `a.E` of the original type (so `a.E.Member` and `b.E`
             * stay one type), an enumerator (`using b::Red;`, `using enum b::E;`) as a second
             * constant spelling, a variable as a second name for the same linkage symbol.
             * Records are not bound: `a.S` already resolves to the original record through the
             * type-request path; the pair is only recorded in classUsings for the backend's
             * cross-import conflict check. Returns true when the shadow was a kind handled here.
             */
            bool PublishUsingShadowAlias(UsingShadowDecl* usd)
            {
                NamedDecl* target = usd->getTargetDecl();
                const bool isType = llvm::isa<EnumDecl>(target)
                    || (llvm::isa<TypedefNameDecl>(target)
                        && !(llvm::isa<TypeAliasDecl>(target)
                             && llvm::cast<TypeAliasDecl>(target)->getDescribedAliasTemplate()));
                const bool isValue = llvm::isa<EnumConstantDecl>(target) || llvm::isa<VarDecl>(target);
                const bool isClass = llvm::isa<RecordDecl>(target) || llvm::isa<ClassTemplateDecl>(target);
                if (!isType && !isValue && !isClass) return false;
                const std::string aliasName = CxxQualifiedName(usd);
                if (!IsValidDottedName(aliasName) || target->isInvalidDecl()) return true;
                if (aliasName == CxxQualifiedName(target)) return true;
                for (const DeclContext* dc = usd->getDeclContext(); dc != nullptr; dc = dc->getParent())
                {
                    if (dc->isFunctionOrMethod()) return true;
                    if (const auto* ns = llvm::dyn_cast<NamespaceDecl>(dc);
                        ns != nullptr && ns->isAnonymousNamespace())
                        return true;
                }
                if (isClass)
                {
                    // `a.S` already resolves to the original record; record the pair so the
                    // backend can diagnose a different `a.S` bound by another import.
                    const NamedDecl* t = target;
                    if (const auto* ctd = llvm::dyn_cast<ClassTemplateDecl>(target))
                        t = ctd->getTemplatedDecl();
                    const std::string targetName = CxxQualifiedName(t);
                    if (IsValidDottedName(targetName))
                        st.out.classUsings.emplace_back(aliasName, targetName);
                    return true;
                }
                if (isType)
                {
                    QualType u = ctx.getTypeDeclType(llvm::cast<TypeDecl>(target));
                    if (u.isNull() || u->isDependentType()) return true;
                    RawTypedef t;
                    t.name = usd->getNameAsString();
                    t.qualifiedName = aliasName;
                    if (!LocOf(usd, t.file, t.line, t.col)) return true;
                    FillTypedefUnderlying(u, t);
                    st.out.typedefs.push_back(std::move(t));
                    return true;
                }
                if (auto* ec = llvm::dyn_cast<EnumConstantDecl>(target))
                {
                    std::string file; int line = 1, col = 0;
                    if (!LocOf(usd, file, line, col)) return true;
                    RawEnum e;
                    if (!FillRawEnum(ec, e)) return true;
                    e.name = aliasName;
                    st.out.enums.push_back(std::move(e));
                    return true;
                }
                auto* vd = llvm::cast<VarDecl>(target);
                if (!vd->isFileVarDecl() || vd->isStaticDataMember()) return true;
                const std::string targetName = CxxQualifiedName(vd);
                auto findTarget = [&]() -> const RawGlobalVar* {
                    for (const RawGlobalVar& g : st.out.globals)
                        if (g.qualifiedName == targetName) return &g;
                    return nullptr;
                };
                if (findTarget() == nullptr) HarvestCxxNamespaceVar(vd);
                const RawGlobalVar* original = findTarget();
                if (original == nullptr || !st.emittedGlobals.insert(aliasName).second) return true;
                RawGlobalVar g = *original;
                g.qualifiedName = aliasName;
                st.out.globals.push_back(std::move(g));
                return true;
            }

            // The shadows a using-declaration (or C++20 `using enum`) introduces are IMPLICIT
            // decls, which the recursive visitor skips; reach them from the explicit decl instead.
            bool VisitUsingDecl(UsingDecl* ud)
            {
                if (ud == nullptr || !st.emittedUsingDecls.insert(ud).second) return true;
                for (UsingShadowDecl* shadow : ud->shadows()) VisitUsingShadowDecl(shadow);
                return true;
            }

            bool VisitUsingEnumDecl(UsingEnumDecl* ud)
            {
                if (ud == nullptr || !st.emittedUsingDecls.insert(ud).second) return true;
                for (UsingShadowDecl* shadow : ud->shadows()) VisitUsingShadowDecl(shadow);
                return true;
            }

            // A namespace-scope using-directive makes the nominated namespace visible without
            // creating aliases for its declarations. Preserve that relationship for lookup-time
            // resolution; function-local and anonymous-namespace directives are not exportable.
            bool VisitUsingDirectiveDecl(UsingDirectiveDecl* ud)
            {
                if (!st.req.cxxMode || ud == nullptr || ud->isInvalidDecl()) return true;
                const auto* owner = llvm::dyn_cast<NamespaceDecl>(ud->getDeclContext());
                const NamespaceDecl* nominated = ud->getNominatedNamespace();
                if (owner == nullptr || nominated == nullptr || owner->isAnonymousNamespace()
                    || nominated->isAnonymousNamespace())
                    return true;
                for (const DeclContext* dc = owner; dc != nullptr; dc = dc->getParent())
                    if (const auto* ns = llvm::dyn_cast<NamespaceDecl>(dc);
                        ns != nullptr && ns->isAnonymousNamespace())
                        return true;

                SourceLocation loc = ud->getLocation();
                if (loc.isInvalid()) return true;
                if (loc.isMacroID()) loc = sm.getSpellingLoc(loc);
                if (sm.isInMainFile(loc)) return true;
                std::string file; int line = 1, col = 0;
                if (!LocOf(ud, file, line, col)) return true;

                const std::string from = CxxQualifiedName(owner);
                const std::string to = CxxQualifiedName(nominated);
                if (IsValidDottedName(from) && IsValidDottedName(to))
                    st.out.usingDirectives.emplace_back(from, to);
                return true;
            }

            // `namespace a = b::c;` names an existing namespace under a second spelling. Harvest
            // the pair so lookup can hop the alias; the alias itself declares nothing.
            bool VisitNamespaceAliasDecl(NamespaceAliasDecl* na)
            {
                if (!st.req.cxxMode || na == nullptr || na->isInvalidDecl()) return true;
                const NamespaceDecl* target = na->getNamespace();
                if (target == nullptr || target->isAnonymousNamespace()) return true;
                if (!na->getIdentifier()) return true;
                // A function-local or anonymous-namespace alias is not exportable.
                for (const DeclContext* dc = na->getDeclContext(); dc != nullptr; dc = dc->getParent())
                {
                    if (dc->isFunctionOrMethod()) return true;
                    if (const auto* ns = llvm::dyn_cast<NamespaceDecl>(dc);
                        ns != nullptr && ns->isAnonymousNamespace())
                        return true;
                }
                std::string file; int line = 1, col = 0;
                if (!LocOf(na, file, line, col)) return true;

                std::string from = CxxQualifiedName(na);
                const std::string to = CxxQualifiedName(target);
                if (IsValidDottedName(from) && IsValidDottedName(to) && from != to)
                    st.out.namespaceAliases.emplace_back(from, to);
                return true;
            }

            // The enumerator's own record (type, value, source position) under its bare name.
            bool FillRawEnum(EnumConstantDecl* ec, RawEnum& e)
            {
                if (!ec->getIdentifier()) return false;
                std::string file; int line = 1, col = 0;
                if (!LocOf(ec, file, line, col)) return false;
                e.name = ec->getNameAsString();
                const auto* ed = llvm::dyn_cast<EnumDecl>(ec->getDeclContext());
                if (st.req.cxxMode)
                {
                    // An unnamed enum ("enum { value = 1 };" inside a class) has no spellable
                    // type; its enumerators publish as plain integer constants instead.
                    if (ed != nullptr && IsValidDottedName(CxxQualifiedName(ed)))
                    {
                        e.enumType = CxxQualifiedName(ed);
                        e.underlyingType = CanonicalSpelling(ctx, ed->getIntegerType());
                        if (!ed->isScoped() && !ed->getPromotionType().isNull())
                            e.promotedType = CanonicalSpelling(ctx, ed->getPromotionType());
                        e.isScoped = ed->isScoped();
                    }
                }
                else if (ed != nullptr && ed->getIdentifier())
                {
                    e.enumType = ed->getNameAsString();
                    e.underlyingType = CanonicalSpelling(ctx, ed->getIntegerType());
                }
                // `typedef enum { ... } Name;` - clang spells the anonymous enum by its typedef name.
                else if (const auto* td = ed != nullptr ? ed->getTypedefNameForAnonDecl() : nullptr)
                {
                    e.enumType = td->getNameAsString();
                    e.underlyingType = CanonicalSpelling(ctx, ed->getIntegerType());
                }
                e.value = ApsIntToLongLong(ec->getInitVal());
                e.file = file; e.line = line; e.col = col;
                e.physicalFile = PhysicalFileOf(ec, file);
                return true;
            }

            bool VisitEnumConstantDecl(EnumConstantDecl* ec)
            {
                RawEnum e;
                if (!FillRawEnum(ec, e)) return true;
                const auto* ed = llvm::dyn_cast<EnumDecl>(ec->getDeclContext());
                // C++ mode also publishes the QUALIFIED spelling, so a scoped or class-nested
                // enumerator is reachable as it is written in C++ ("ns.Cls.Kind.One") rather than
                // only under a bare name that could collide across namespaces. The unqualified
                // form stays registered as well - first writer wins downstream.
                if (st.req.cxxMode)
                {
                    std::string qualified = CxxQualifiedName(ec);
                    if (qualified != e.name && IsValidDottedName(qualified)
                        && !ec->getDeclContext()->isTranslationUnit())
                    {
                        RawEnum q = e;
                        q.name = qualified;
                        st.out.enums.push_back(std::move(q));
                    }
                    if (ed != nullptr && !ed->isScoped())
                    {
                        const std::string enumQualified = CxxQualifiedName(ed);
                        const size_t dot = enumQualified.rfind('.');
                        const std::string parent = dot == std::string::npos
                            ? std::string{} : enumQualified.substr(0, dot);
                        const std::string injected = parent.empty()
                            ? e.name : parent + "." + e.name;
                        if (injected != e.name && injected != qualified && IsValidDottedName(injected))
                        {
                            RawEnum q = e;
                            q.name = injected;
                            st.out.enums.push_back(std::move(q));
                        }
                    }
                    if (ed != nullptr)
                    {
                        const std::string enumMember = CxxQualifiedName(ed) + "." + e.name;
                        if (enumMember != e.name && enumMember != qualified
                            && IsValidDottedName(enumMember))
                        {
                            RawEnum q = e;
                            q.name = enumMember;
                            st.out.enums.push_back(std::move(q));
                        }
                    }
                }
                st.out.enums.push_back(std::move(e));
                return true;
            }

            // Fill rec.fields from a record. C11 transparent anonymous members (unnamed fields
            // whose type is an anonymous struct/union) get a synthetic `<tag>__anon<N>` record
            // plus a synthetic `__anon<N>` field, matching the old libclang path so CFlat reaches
            // the inner members via `o.__anon0.field`. Named nested records are emitted on their
            // own by the visitor. Anonymous bitfields are width markers (storage-unit boundaries).
            void CollectFields(const RecordDecl* rd, const std::string& tag, RawRecord& rec)
            {
                int anonIdx = 0;
                const ASTRecordLayout& layout = ctx.getASTRecordLayout(rd);
                for (const FieldDecl* f : rd->fields())
                {
                    if (f->getNameAsString() == kHeaderScopeSentinel) continue;
                    if (f->getDeclName().isEmpty())
                    {
                        if (f->isBitField())
                        {
                            RawField rf;
                            rf.isBitfield = true;
                            rf.bitWidth = f->getBitWidthValue();
                            rf.ctype = CanonicalSpelling(ctx, f->getType());
                            rf.offsetBytes = layout.getFieldOffset(f->getFieldIndex()) / 8;
                            rf.bitOffset = layout.getFieldOffset(f->getFieldIndex());
                            rec.fields.push_back(std::move(rf));
                            continue;
                        }
                        const RecordType* rt = f->getType()->getAs<RecordType>();
                        const RecordDecl* anon = rt ? rt->getDecl()->getDefinition() : nullptr;
                        if (anon && anon->isAnonymousStructOrUnion())
                        {
                            const int idx = anonIdx++;
                            const std::string synTag = tag + "__anon" + std::to_string(idx);
                            const bool isUnion = anon->isUnion();

                            RawRecord nested;
                            nested.name = synTag;
                            nested.isUnion = isUnion;
                            // Inside a C++ class it is C++ too: an unmappable member (libc++'s
                            // compressed-pair padding) still embeds as opaque bytes.
                            nested.isCxx = st.req.cxxMode;
                            // A C++ record also carries clang's triviality verdict, or a trivially
                            // copyable member reads as nontrivial with no copy constructor.
                            if (const auto* anonCxx = llvm::dyn_cast<CXXRecordDecl>(anon);
                                anonCxx != nullptr && st.req.cxxMode)
                            {
                                nested.isTrivial = anonCxx->isTrivial();
                                nested.isTriviallyCopyable = anonCxx->isTriviallyCopyable()
                                    && !anonCxx->hasNonTrivialDestructor()
                                    && !anonCxx->isPolymorphic();
                                nested.isTriviallyRelocatable = st.ci != nullptr
                                    && st.ci->hasSema()
                                    && st.ci->getSema().IsCXXTriviallyRelocatableType(*anonCxx);
                                nested.hasTrivialDefaultCtor = anonCxx->hasTrivialDefaultConstructor();
                                nested.hasTrivialCopyCtor = anonCxx->hasTrivialCopyConstructor();
                                nested.hasTrivialCopyAssign = anonCxx->hasSimpleCopyAssignment()
                                    && anonCxx->hasTrivialCopyAssignment();
                                nested.hasTrivialMoveAssign = anonCxx->hasSimpleMoveAssignment()
                                    && anonCxx->hasTrivialMoveAssignment();
                                nested.hasTrivialDtor = !anonCxx->hasNonTrivialDestructor();
                            }
                            // A synthetic anonymous member is never independently in-scope; it is
                            // pulled into the kept set only when its enclosing record is (via the
                            // "struct <tag>__anon<N>" field reference the closure walk follows).
                            nested.inScope = false;
                            std::string nf; int nl = 1, nc = 0;
                            if (LocOfRaw(anon, nf, nl, nc))
                            {
                                nested.file = nf; nested.line = nl; nested.col = nc;
                                nested.physicalFile = PhysicalFileOf(anon, nf);
                            }
                            CollectFields(anon, synTag, nested);
                            const ASTRecordLayout& nestedLayout = ctx.getASTRecordLayout(anon);
                            nested.sizeBytes = nestedLayout.getSize().getQuantity();
                            nested.alignBytes = nestedLayout.getAlignment().getQuantity();
                            RawField fe;
                            fe.name = "__anon" + std::to_string(idx);
                            fe.ctype = (isUnion ? "union " : "struct ") + synTag;
                            fe.access = MapAccess(f->getAccess());
                            fe.offsetBytes = layout.getFieldOffset(f->getFieldIndex()) / 8;
                            RecordRawFieldLayout(f->getType(), fe);
                            rec.fields.push_back(std::move(fe));
                            const uint64_t anonOffset = layout.getFieldOffset(f->getFieldIndex()) / 8;
                            for (const RawField& child : nested.fields)
                            {
                                if (child.name.empty() || child.name.starts_with("__anon")
                                    || child.isBitfield) continue;
                                const bool promoteAll = st.req.cxxMode
                                    && MapAccess(f->getAccess()) == AccessPublic;
                                if (!promoteAll && (!child.isZeroSize
                                    || (child.ctype.find("[]") == std::string::npos
                                        && child.ctype.find("[0]") == std::string::npos)))
                                    continue;
                                RawField promoted = child;
                                promoted.isPromoted = promoteAll;
                                promoted.offsetBytes += anonOffset;
                                rec.fields.push_back(std::move(promoted));
                            }
                            st.out.records.push_back(std::move(nested));
                        }
                        continue;  // unnamed non-bitfield non-anon: nothing to record
                    }

                    RawField rf;
                    rf.name = f->getNameAsString();
                    rf.access = MapAccess(f->getAccess());
                    rf.isConst = f->getType().isConstQualified();
                    rf.isMutable = f->isMutable();
                    // A zero-length or flexible array occupies no bytes; cflat has no 0-extent
                    // array, so it must ride the zero-size layout path.
                    const auto* constArr = ctx.getAsConstantArrayType(f->getType());
                    rf.isZeroSize = f->isZeroSize(ctx)
                        || (constArr != nullptr && constArr->isZeroSize())
                        || ctx.getAsIncompleteArrayType(f->getType()) != nullptr;
                    rf.offsetBytes = layout.getFieldOffset(f->getFieldIndex()) / 8;
                    RecordRawFieldLayout(f->getType(), rf);
                    // A reference member is a pointer-sized slot in every ABI cflat targets, and
                    // the type mapper already peels the '&' - so it binds as `T*`, exactly as the
                    // flattened (base/vptr) field walk in FlattenCxxLayout already does.

                    // Named field whose type is a *truly unnamed* (no tag, no typedef-for-linkage
                    // name) record - the `_LARGE_INTEGER::u` shape: `struct { DWORD LowPart;
                    // LONG HighPart; } u;`. Its canonical spelling is the unusable
                    // "struct X::(unnamed at ...)" which MapCTypeToTypeAndValue rejects, abandoning
                    // the whole record. Synthesize a tag and register the inner record (exactly like
                    // the anonymous-member path), but keep the field's real name so the member is
                    // reached via `o.u.LowPart`. This is distinct from a C11 anonymous member
                    // (handled above): that injects its members transparently; this one does not.
                    //
                    // The same shape also occurs *inside an array*: RETRIEVAL_POINTERS_BUFFER has
                    // `struct { LARGE_INTEGER NextVcn; LARGE_INTEGER Lcn; } Extents[1];`. Peel the
                    // constant-array extents off first so the unnamed element record gets the same
                    // synthetic tag, then re-append the `[N]...` suffix so RegisterCRecords lays it
                    // out as an inline array of the synthetic record (exact C ABI size). The element
                    // is still a named field, so access is `o.Extents[i].NextVcn` - non-transparent.
                    std::string arrSuffix;
                    QualType elemTy = f->getType();
                    while (const ConstantArrayType* cat = ctx.getAsConstantArrayType(elemTy))
                    {
                        arrSuffix += "[" + std::to_string(cat->getSize().getZExtValue()) + "]";
                        elemTy = cat->getElementType();
                    }
                    const RecordType* nrt = elemTy->getAs<RecordType>();
                    const RecordDecl* nrd = nrt ? nrt->getDecl()->getDefinition() : nullptr;
                    if (nrd && !nrd->getIdentifier() && !nrd->getTypedefNameForAnonDecl()
                        && (st.req.cxxMode || !nrd->isAnonymousStructOrUnion()))
                    {
                        const int idx = anonIdx++;
                        const std::string synTag = tag + "__anon" + std::to_string(idx);
                        const bool isUnion = nrd->isUnion();

                        RawRecord nested;
                        nested.name = synTag;
                        nested.isUnion = isUnion;
                        nested.inScope = false;
                        std::string nf; int nl = 1, nc = 0;
                        if (LocOfRaw(nrd, nf, nl, nc))
                        {
                            nested.file = nf; nested.line = nl; nested.col = nc;
                            nested.physicalFile = PhysicalFileOf(nrd, nf);
                        }
                        CollectFields(nrd, synTag, nested);
                        const ASTRecordLayout& nestedLayout = ctx.getASTRecordLayout(nrd);
                        nested.sizeBytes = nestedLayout.getSize().getQuantity();
                        nested.alignBytes = nestedLayout.getAlignment().getQuantity();
                        st.out.records.push_back(std::move(nested));

                        rf.ctype = (isUnion ? "union " : "struct ") + synTag
                                 + (arrSuffix.empty() ? std::string{} : " " + arrSuffix);
                    }
                    else
                    {
                        rf.ctype = CanonicalSpelling(ctx, f->getType());
                    }

                    if (f->isBitField())
                    {
                        rf.isBitfield = true;
                        rf.bitWidth = f->getBitWidthValue();
                        rf.bitOffset = layout.getFieldOffset(f->getFieldIndex());
                    }
                    rec.fields.push_back(std::move(rf));
                }
            }

            /*
             * Export a C++ class's callable surface: constructors, the destructor, non-static
             * methods (const and non-const), static methods and out-of-line static data members,
             * plus the triviality/access bits the backend needs to decide what it may bind. The
             * decision to REFUSE a member (virtual, private, deleted, template) is left to the
             * backend so the diagnostic lands at the CFlat use site; everything is exported with
             * enough truth attached to say why.
             * `outDecls` is filled in lockstep with rec.members so the ABI pass can revisit each
             * declaration once a single CodeGenerator exists.
             */
            bool MemberConstraintsSatisfied(const CXXMethodDecl* md)
            {
                return st.ci == nullptr || !st.ci->hasSema()
                    || ConstrainedMemberSatisfied(st.ci->getSema(), md);
            }

            bool IsLessConstrainedTwin(const CXXMethodDecl* md,
                                       const std::vector<const CXXMethodDecl*>& methods)
            {
                return st.ci != nullptr && st.ci->hasSema()
                    && LosesToMoreConstrainedTwin(st.ci->getSema(), md, methods);
            }

            void CollectCxxMembers(const CXXRecordDecl* cxx, RawRecord& rec,
                                   std::vector<const CXXMethodDecl*>& outDecls)
            {
                rec.isPolymorphic = cxx->isPolymorphic() || cxx->getNumVBases() > 0;
                // A polymorphic class needs a vtable. Clang decides whether this translation unit
                // owns it (all-inline: linkonce_odr here) or whether a key function anchors it in
                // the bound library (external declaration); asking is always safe.
                if (st.req.RecordsDefinitionDemand() && cxx->isPolymorphic()
                    && cxx->getNumVBases() == 0)
                    st.vtableWork.push_back(cxx);
                rec.hasBases = cxx->getNumBases() > 0 || cxx->getNumVBases() > 0;
                rec.hasVirtualBases = cxx->getNumVBases() > 0;
                rec.isFinal = cxx->hasAttr<FinalAttr>();
                rec.isAbstract = cxx->isAbstract();
                for (const FriendDecl* fr : cxx->friends())
                    if (const auto* named = fr->getFriendDecl())
                        if (const auto* fn = named->getAsFunction();
                            fn != nullptr && fn->getOverloadedOperator() != OO_None)
                        {
                            rec.hasFriendOperators = true;
                            break;
                        }
                rec.hasTrivialDefaultCtor = cxx->hasTrivialDefaultConstructor();
                rec.hasTrivialCopyCtor = cxx->hasTrivialCopyConstructor();
                rec.hasTrivialCopyAssign = cxx->hasSimpleCopyAssignment()
                                        && cxx->hasTrivialCopyAssignment();
                rec.hasTrivialMoveAssign = cxx->hasSimpleMoveAssignment()
                                        && cxx->hasTrivialMoveAssignment();
                rec.hasTrivialDtor = !cxx->hasNonTrivialDestructor();
                rec.paramDestroyedInCallee = cxx->isParamDestroyedInCallee();
                rec.hasDefaultCtor = cxx->hasDefaultConstructor();
                rec.hasCopyCtor = cxx->hasCopyConstructorWithConstParam()
                               || cxx->needsImplicitCopyConstructor()
                               || cxx->hasUserDeclaredCopyConstructor();
                for (const Decl* d : cxx->decls())
                {
                    const auto* ftd = llvm::dyn_cast<FunctionTemplateDecl>(d);
                    if (const auto* assign = ftd != nullptr
                            ? llvm::dyn_cast<CXXMethodDecl>(ftd->getTemplatedDecl()) : nullptr;
                        assign != nullptr && assign->getOverloadedOperator() == OO_Equal
                        && !assign->isDeleted())
                        rec.hasAssignTemplate = true;
                    const auto* ctor = ftd != nullptr
                        ? llvm::dyn_cast<CXXConstructorDecl>(ftd->getTemplatedDecl()) : nullptr;
                    if (ctor == nullptr) continue;
                    if (ftd->getAccess() == AS_public && !ctor->isDeleted())
                        rec.hasCtorTemplate = true;
                    // Every constructor template, as written, for the overload mirror.
                    RawCxxCtorTemplate ct;
                    PrintingPolicy pp = ctx.getPrintingPolicy();
                    pp.FullyQualifiedName = true;
                    pp.SuppressScope = false;
                    llvm::raw_string_ostream head(ct.head);
                    ftd->getTemplateParameters()->print(head, ctx, pp);
                    head.flush();
                    for (const ParmVarDecl* p : ctor->parameters())
                    {
                        ct.paramTypes.push_back(p->getType().getAsString(pp));
                        ct.defaulted.push_back(p->hasDefaultArg() ? 1 : 0);
                    }
                    ct.variadic = ctor->isVariadic();
                    ct.isDeleted = ctor->isDeleted();
                    ct.isExplicit = ctor->isExplicit();
                    ct.access = MapAccess(ftd->getAccess());
                    rec.ctorTemplates.push_back(std::move(ct));
                }
                rec.isAggregate = cxx->isAggregate();

                /*
                 * M5b - MEMBER TEMPLATES whose own template parameters are ALL DEFAULTED and whose
                 * signature does not depend on them. libc++ writes several ordinary-looking members
                 * that way for SFINAE - `basic_string(const char*)` is
                 * `template <__enable_if_t<...> = 0> basic_string(const _CharT*)` - so refusing every
                 * template member would refuse `std.string("hello")` itself. The concrete signature
                 * is already in the pattern, which is what the type request's stage-1 pass reports so
                 * its stage-2 stub can ODR-USE it; stage 2 (emitDefinitions) then binds the real
                 * SPECIALIZATION, the only decl that has a linkage name and a body.
                 * Nothing here helps a template whose parameters must be DEDUCED from the arguments -
                 * that is still out of scope.
                 */
                std::vector<const CXXMethodDecl*> methodList;
                std::set<const CXXMethodDecl*> templateExtras;
                for (const CXXMethodDecl* md : cxx->methods()) methodList.push_back(md);
                std::set<const CXXMethodDecl*> listedMethods(methodList.begin(), methodList.end());
                if (const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(cxx);
                    spec != nullptr && spec->getSpecializedTemplate() != nullptr)
                {
                    // Instantiated from a partial specialization (unique_ptr<T[]>): its members.
                    const auto from = spec->getSpecializedTemplateOrPartial();
                    const CXXRecordDecl* pattern =
                        from.is<ClassTemplatePartialSpecializationDecl*>()
                            ? from.get<ClassTemplatePartialSpecializationDecl*>()
                            : spec->getSpecializedTemplate()->getTemplatedDecl();
                    for (const Decl* d : pattern->decls())
                        if (const auto* ftd = llvm::dyn_cast<FunctionTemplateDecl>(d))
                            VisitFunctionTemplateDecl(const_cast<FunctionTemplateDecl*>(ftd));
                }
                // A using-declaration re-exposes a base member under ITS OWN access: the MSVC
                // STL keeps _Ptr_base::get protected and publishes it with `using _Mybase::get;`
                // in shared_ptr, so the shadow's access is the one this class grants.
                std::map<const CXXMethodDecl*, AccessSpecifier> usingAccess;
                for (const Decl* d : cxx->decls())
                {
                    const auto* shadow = llvm::dyn_cast<UsingShadowDecl>(d);
                    const auto* target = shadow != nullptr
                        ? llvm::dyn_cast<CXXMethodDecl>(shadow->getTargetDecl()) : nullptr;
                    if (target == nullptr) continue;
                    usingAccess.emplace(target, shadow->getAccess());
                    if (listedMethods.insert(target).second) methodList.push_back(target);
                }
                if (!st.req.cxxTypeRequests.empty())
                    for (const Decl* d : cxx->decls())
                    {
                        const auto* ftd = llvm::dyn_cast<FunctionTemplateDecl>(d);
                        if (ftd == nullptr) continue;
                        const auto* pattern = llvm::dyn_cast<CXXMethodDecl>(ftd->getTemplatedDecl());
                        if (pattern == nullptr) continue;
                        VisitFunctionTemplateDecl(const_cast<FunctionTemplateDecl*>(ftd));
                        bool allDefaulted = true;
                        for (const NamedDecl* tp : *ftd->getTemplateParameters())
                        {
                            if (const auto* tt = llvm::dyn_cast<TemplateTypeParmDecl>(tp))
                                allDefaulted = allDefaulted && tt->hasDefaultArgument();
                            else if (const auto* nt = llvm::dyn_cast<NonTypeTemplateParmDecl>(tp))
                                allDefaulted = allDefaulted && nt->hasDefaultArgument();
                            else
                                allDefaulted = false;
                        }
                        // Keep requested constructor templates; general function-template
                        // deduction remains unsupported.
                        const bool constructorTemplate = llvm::isa<CXXConstructorDecl>(pattern);
                        bool dependent = pattern->getReturnType()->isDependentType();
                        for (const ParmVarDecl* p : pattern->parameters())
                            dependent = dependent || p->getType()->isDependentType();
                        if (st.req.RecordsDefinitionDemand())
                        {
                            for (const FunctionDecl* spec : ftd->specializations())
                            {
                                const auto* smd = llvm::dyn_cast<CXXMethodDecl>(spec);
                                if (smd == nullptr || smd->isInvalidDecl()) continue;
                                templateExtras.insert(smd);
                                if (listedMethods.insert(smd).second) methodList.push_back(smd);
                            }
                            // Keep a dependent constructor pattern visible to the backend. A
                            // variadic constructor may be served by its generated wrapper.
                            if (constructorTemplate && dependent)
                            {
                                templateExtras.insert(pattern);
                                if (listedMethods.insert(pattern).second) methodList.push_back(pattern);
                            }
                        }
                        if (!st.req.RecordsDefinitionDemand()
                            && (!allDefaulted && !constructorTemplate))
                            continue;
                        if (dependent)
                        {
                            if (!allDefaulted || st.ci == nullptr || !st.ci->hasSema()) continue;
                            Sema& sema = st.ci->getSema();
                            clang::Scope tuScope(nullptr, clang::Scope::DeclScope,
                                                 st.ci->getDiagnostics());
                            const bool lendScope = sema.TUScope == nullptr;
                            if (lendScope) sema.TUScope = &tuScope;
                            FunctionDecl* specialization = nullptr;
                            sema::TemplateDeductionInfo info(pattern->getLocation());
                            const TemplateDeductionResult result = sema.DeduceTemplateArguments(
                                const_cast<FunctionTemplateDecl*>(ftd), nullptr, specialization,
                                info, /*IsAddressOfFunction*/ true);
                            if (lendScope) sema.TUScope = nullptr;
                            const auto* smd = result == TemplateDeductionResult::Success
                                ? llvm::dyn_cast_or_null<CXXMethodDecl>(specialization) : nullptr;
                            if (smd == nullptr)
                            {
                                // Do not silently lose a dependent constructor: the backend can
                                // report its refusal or defer a variadic wrapper until use.
                                if (constructorTemplate)
                                {
                                    templateExtras.insert(pattern);
                                    if (listedMethods.insert(pattern).second) methodList.push_back(pattern);
                                }
                                continue;
                            }
                            templateExtras.insert(smd);
                            if (listedMethods.insert(smd).second) methodList.push_back(smd);
                            continue;
                        }
                        if (!st.req.RecordsDefinitionDemand())
                        {
                            templateExtras.insert(pattern);
                            methodList.push_back(pattern);
                        }
                    }

                for (const CXXMethodDecl* md : methodList)
                {
                    const bool templateExtra = templateExtras.count(md) != 0;
                    // Templates and their specializations need Sema instantiation (M5), except the
                    // all-defaulted member templates selected above.
                    if (md->getDescribedFunctionTemplate() != nullptr && !templateExtra) continue;
                    if (md->getPrimaryTemplate() != nullptr && !templateExtra) continue;
                    // A member whose requires-clause this class does not satisfy is never viable
                    // ([over.match.viable]); of two same-signature twins only the more constrained is.
                    if (!MemberConstraintsSatisfied(md)) continue;
                    if (IsLessConstrainedTwin(md, methodList)) continue;
                    const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md);
                    const auto* dtor = llvm::dyn_cast<CXXDestructorDecl>(md);
                    // A trivial destructor does nothing and clang never emits one (CodeGen asserts
                    // on a non-exported trivial dtor); the record's hasTrivialDtor already says so.
                    if (dtor != nullptr && dtor->isTrivial()) continue;
                    // Operators and conversion functions are M5; they have no plain identifier.
                    // Copy and move ASSIGNMENT are the exception: they are special members that
                    // nontrivial-class lifetime needs (M4b), so they are exported under the name
                    // "operator=" and consumed by the class's assignment table, never as a
                    // callable member.
                    const bool isAssignSpecial = md->isCopyAssignmentOperator()
                                              || md->isMoveAssignmentOperator();
                    /*
                     * M5b - the operators CFlat has a spelling for are exported under their C++
                     * source name ("operator[]", "operator==", ...). The subscript path and the
                     * binary-operator overload table both look a member up by exactly that name,
                     * so no new registration surface is needed. Anything else without an
                     * identifier (conversion functions, operator new, ...) stays out.
                     */
                    bool isBindableOperator = false;
                    switch (md->getOverloadedOperator())
                    {
                        // Non-special operator= overloads (for example operator=(int)) are
                        // ordinary callable members. Copy/move assignment stays on the special-
                        // member path above and below, never in the member operator table.
                        case OO_Equal:
                            isBindableOperator = !isAssignSpecial; break;
                        case OO_Subscript: case OO_EqualEqual: case OO_ExclaimEqual:
                        case OO_Plus: case OO_Minus: case OO_Star: case OO_Slash:
                        case OO_PlusEqual: case OO_MinusEqual:
                        case OO_Less: case OO_Greater: case OO_LessEqual: case OO_GreaterEqual:
                        case OO_Percent: case OO_PercentEqual: case OO_StarEqual: case OO_SlashEqual:
                        case OO_LessLess: case OO_GreaterGreater:
                        case OO_LessLessEqual: case OO_GreaterGreaterEqual:
                        case OO_Amp: case OO_Pipe: case OO_Caret:
                        case OO_AmpEqual: case OO_PipeEqual: case OO_CaretEqual:
                        case OO_Exclaim:
                        // C++20 three-way comparison. CFlat has no `<=>` spelling, but the
                        // relational operators are REWRITTEN from it, so it must be bindable.
                        case OO_Spaceship:
                        // A class that overloads && / || loses short-circuiting in C++ too:
                        // both operands are evaluated and the operator is an ordinary call.
                        case OO_AmpAmp: case OO_PipePipe:
                            isBindableOperator = true; break;
                        case OO_PlusPlus: case OO_MinusMinus:
                            isBindableOperator = md->getNumParams() == 0; break;
                        case OO_Arrow:
                            isBindableOperator = true; break;
                        // CFlat's unary `~` hook is an arity-0 member, so only that form binds;
                        // a binary `operator~` does not exist in C++, but the guard is free.
                        case OO_Tilde:
                            isBindableOperator = md->getNumParams() == 0; break;
                        // `obj(args)` is CFlat's spelling for a member operator(). Every class
                        // exports it (not only a type-requested one), and each arity/parameter
                        // overload registers separately.
                        case OO_Call:
                            isBindableOperator = true; break;
                        /*
                         * Class-scope allocation functions: `new T` / `delete p` of this class
                         * call them instead of the global ones. Only the USUAL forms bind -
                         * operator new(size_t) and operator delete(void*[, size_t]); placement,
                         * aligned and destroying forms stay out.
                         */
                        case OO_New: case OO_Array_New:
                            isBindableOperator = md->getNumParams() == 1
                                && ctx.hasSameType(md->getParamDecl(0)->getType(),
                                                   ctx.getSizeType());
                            break;
                        case OO_Delete: case OO_Array_Delete:
                            isBindableOperator = !md->isDestroyingOperatorDelete()
                                && md->getParamDecl(0)->getType()->isVoidPointerType()
                                && (md->getNumParams() == 1
                                    || (md->getNumParams() == 2
                                        && ctx.hasSameType(md->getParamDecl(1)->getType(),
                                                           ctx.getSizeType())));
                            break;
                        default: break;
                    }
                    /*
                     * Every conversion function is exported, explicit or not, and the
                     * `explicit` bit travels with it: an implicit one converts at an
                     * initializer, assignment, call argument and return as well as at a cast,
                     * an explicit one only at a cast and in a boolean context. The registration
                     * side renames the member to "operator <CFlat spelling>" and refuses a
                     * target the type map cannot express.
                     */
                    const auto* conversion = llvm::dyn_cast<CXXConversionDecl>(md);
                    const bool isBindableBoolConversion = conversion != nullptr
                        && conversion->getConversionType().getCanonicalType()->isBooleanType();
                    if (conversion != nullptr) isBindableOperator = true;
                    if (ctor == nullptr && dtor == nullptr && md->getIdentifier() == nullptr
                        && !isAssignSpecial && !isBindableOperator)
                        continue;
                    // An `auto` member of a class template instantiation (MSVC STL span::subspan)
                    // has no return type until its body is instantiated: deduce it as a call would.
                    if (st.req.RecordsDefinitionDemand() && st.ci != nullptr && st.ci->hasSema()
                        && md->getReturnType()->isUndeducedType() && !md->isInvalidDecl()
                        && !md->isDependentContext()
                        && md->getTemplateInstantiationPattern() != nullptr)
                    {
                        Sema& sema = st.ci->getSema();
                        clang::Scope tuScope(nullptr, clang::Scope::DeclScope,
                                             st.ci->getDiagnostics());
                        const bool lendScope = sema.TUScope == nullptr;
                        if (lendScope) sema.TUScope = &tuScope;
                        sema.DeduceReturnType(const_cast<CXXMethodDecl*>(md), md->getLocation(),
                                              /*Diagnose*/ false);
                        if (lendScope) sema.TUScope = nullptr;
                    }

                    RawCxxMember m;
                    if (ctor != nullptr)
                    {
                        m.kind = RawCxxMember::Constructor;
                        m.name = "__ctor";
                        m.isDefaultCtor = ctor->isDefaultConstructor();
                        m.isCopyCtor = ctor->isCopyConstructor();
                        m.isMoveCtor = ctor->isMoveConstructor();
                        if (!m.isMoveCtor) m.isMoveCtor = IsSameClassRvalueParameter(md);
                        m.isExplicit = ctor->isExplicit();
                    }
                    else if (dtor != nullptr)
                    {
                        m.kind = RawCxxMember::Destructor;
                        m.name = "__dtor";
                    }
                    else
                    {
                        m.kind = md->isStatic() ? RawCxxMember::StaticMethod
                                                : RawCxxMember::Instance;
                        m.isConversion = conversion != nullptr;
                        m.isExplicit = conversion != nullptr && conversion->isExplicit();
                        m.name = isBindableBoolConversion ? "operator bool" : md->getNameAsString();
                        m.isCopyAssign = md->isCopyAssignmentOperator();
                        m.isMoveAssign = md->isMoveAssignmentOperator();
                        if (!m.isMoveAssign && m.name == "operator=")
                            m.isMoveAssign = IsSameClassRvalueParameter(md);
                        if (m.name.empty()) m.name = "operator=";
                    }
                    m.isConst = !md->isStatic() && md->isConst();
                    m.isVolatile = !md->isStatic() && md->isVolatile();
                    switch (md->getRefQualifier())
                    {
                        case RQ_LValue: m.refQualifier = CxxRefQualifierLValue; break;
                        case RQ_RValue: m.refQualifier = CxxRefQualifierRValue; break;
                        default:        m.refQualifier = CxxRefQualifierNone; break;
                    }
                    m.isVirtual = md->isVirtual();
                    m.isPureVirtual = md->isPureVirtual();
                    m.isOverride = md->hasAttr<OverrideAttr>();
                    m.isFinal = md->hasAttr<FinalAttr>();
                    /*
                     * Covariant return: the override returns a pointer/reference to a class
                     * DERIVED from what the overridden declaration returns. When that derived-to-
                     * base step has a non-zero offset (a non-primary base), the Itanium ABI needs
                     * a return-adjusting thunk, which is Clang's to emit and cflat's to refuse.
                     */
                    if (md->isVirtual())
                        for (const CXXMethodDecl* over : md->overridden_methods())
                        {
                            QualType mine = md->getReturnType().getCanonicalType();
                            QualType theirs = over->getReturnType().getCanonicalType();
                            if (mine == theirs) continue;
                            const auto* mineRd = mine->getPointeeType().isNull()
                                ? nullptr : mine->getPointeeType()->getAsCXXRecordDecl();
                            const auto* theirsRd = theirs->getPointeeType().isNull()
                                ? nullptr : theirs->getPointeeType()->getAsCXXRecordDecl();
                            if (mineRd == nullptr || theirsRd == nullptr
                                || mineRd->getDefinition() == nullptr
                                || theirsRd->getDefinition() == nullptr
                                || mineRd == theirsRd)
                            {
                                m.covariantReturnNeedsAdjust = true;   // cannot prove it is free
                                continue;
                            }
                            // getBaseClassOffset knows DIRECT bases only: walk the inheritance path
                            // so an indirect base neither asserts nor silently reads offset zero.
                            CXXBasePaths paths;
                            if (!mineRd->getDefinition()->isDerivedFrom(theirsRd->getDefinition(), paths)
                                || paths.begin() == paths.end())
                                m.covariantReturnNeedsAdjust = true;
                            else
                            {
                                int64_t total = 0;
                                bool unprovable = false;
                                for (const CXXBasePathElement& el : *paths.begin())
                                {
                                    const auto* baseRd = el.Base->getType()->getAsCXXRecordDecl();
                                    if (el.Base->isVirtual() || baseRd == nullptr
                                        || baseRd->getDefinition() == nullptr || el.Class == nullptr
                                        || el.Class->getDefinition() == nullptr)
                                    { unprovable = true; break; }
                                    total += ctx.getASTRecordLayout(el.Class->getDefinition())
                                                 .getBaseClassOffset(baseRd->getDefinition()).getQuantity();
                                }
                                if (unprovable || total != 0) m.covariantReturnNeedsAdjust = true;
                            }
                        }
                    m.isDeleted = md->isDeleted();
                    m.isDefaulted = md->isDefaulted();
                    m.isImplicit = md->isImplicit();
                    m.isTemplateSpecialization = md->getPrimaryTemplate() != nullptr;
                    m.isNoexcept = DeclIsNoexcept(md);
                    m.access = MapAccess(usingAccess.count(md) != 0 ? usingAccess.at(md)
                                                                    : md->getAccess());
                    m.variadic = md->isVariadic();
                    if (ctor != nullptr)
                    {
                        m.requiresConstructorWrapper = ctor->isInheritingConstructor();
                        for (const ParmVarDecl* p : md->parameters())
                            m.requiresConstructorWrapper = m.requiresConstructorWrapper
                                || llvm::isa<PackExpansionType>(p->getType());
                    }
                    // An implicit, defaulted or inline member has no symbol in the separately
                    // compiled library; emitting its body is Clang-CodeGen work (M5). Trivial
                    // operations need no call at all, which the backend handles from the
                    // triviality bits above.
                    // A member of an IMPLICIT class-template instantiation whose pattern the header
                    // defines (out of line, without `inline`) has no guaranteed library symbol.
                    const FunctionDecl* pattern =
                        md->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation
                            && !md->isPureVirtual()
                        ? md->getTemplateInstantiationPattern() : nullptr;
                    // `inline` may sit only on a later out-of-class definition (simdjson's -inl
                    // section): the in-class declaration alone does not say so.
                    const FunctionDecl* memberDefinition = md->getDefinition();
                    const bool inlineMember = md->isInlined()
                        || (memberDefinition != nullptr && memberDefinition->isInlined());
                    m.needsLocalDefinition = md->isImplicit() || md->isDefaulted()
                                          || inlineMember
                                          || (pattern != nullptr && pattern->isDefined());
                    // Structors on Itanium/Darwin hand 'this' back; the caller ignores it, so the
                    // declaration carries a void* result rather than a mistyped void.
                    if (ctor != nullptr || dtor != nullptr)
                    {
                        m.returnsThis = true;
                        m.retType = "void *";
                    }
                    else
                        m.retType = CanonicalSpelling(ctx, isBindableBoolConversion
                            ? conversion->getConversionType() : md->getReturnType());

                    if (m.kind == RawCxxMember::Instance || m.kind == RawCxxMember::Constructor
                        || m.kind == RawCxxMember::Destructor)
                    {
                        // 'this' first, spelled as a plain pointer to the record (const is
                        // dropped by CFlat, so the const overload differs only in `isConst`).
                        m.paramTypes.push_back(CanonicalSpelling(ctx,
                            ctx.getPointerType(ctx.getCanonicalTagType(cxx))));
                        m.paramNames.push_back("this");
                        m.defaultArgs.push_back({});
                    }
                    for (const ParmVarDecl* p : md->parameters())
                    {
                        m.paramTypes.push_back(CanonicalSpelling(ctx, p->getType()));
                        m.paramNames.push_back(p->getNameAsString());
                        m.defaultArgs.push_back(DefaultArgumentOf(p, ctx));
                        QueueIncompleteCxxType(st, ctx, p->getType());
                    }
                    QueueIncompleteCxxType(st, ctx, md->getReturnType());
                    // A default-argument wrapper spells the receiver, result and parameters at
                    // namespace scope; one naming a non-public member type cannot be built.
                    bool wrapperUnspellable = NamesNonPublicMember(ctx.getCanonicalTagType(cxx))
                        || NamesNonPublicMember(md->getReturnType());
                    for (const ParmVarDecl* p : md->parameters())
                        wrapperUnspellable = wrapperUnspellable || NamesNonPublicMember(p->getType());
                    if (wrapperUnspellable)
                        for (RawDefaultArg& d : m.defaultArgs)
                            if (!d.kind.empty()) { d.kind = "unsupported"; d.value.clear(); }
                    // Refuse before any arrangement: an incomplete by-value type has no layout.
                    for (const ParmVarDecl* p : md->parameters())
                    {
                        std::string bad = IncompleteByValueRecord(ctx, p->getType());
                        if (bad.empty()) continue;
                        m.bindRefusal = "takes '" + bad + "' by value, whose definition this "
                                        "translation unit does not have (include the header that "
                                        "defines it alongside this one)";
                        if (LazyCompletableMemberClass(p->getType())) m.lazyNestedSpelling = bad;
                        break;
                    }
                    // A signature holding an error node (an incomplete element's sizeof in a
                    // default template argument) has no Itanium mangling and no ABI arrangement.
                    if (m.bindRefusal.empty() && md->getType()->containsErrors())
                        m.bindRefusal = "has a signature clang could not instantiate for these "
                                        "template arguments (it names an invalid or incomplete type)";
                    if (m.bindRefusal.empty() && md->getReturnType()->isUndeducedType())
                        m.bindRefusal = "has a deduced return type ('auto') that this translation "
                                        "unit never deduced (its body was not instantiated)";
                    if (m.bindRefusal.empty() && ctor == nullptr && dtor == nullptr)
                    {
                        std::string bad = IncompleteByValueRecord(ctx, md->getReturnType());
                        if (!bad.empty())
                        {
                            m.bindRefusal = "returns '" + bad + "' by value, whose definition this "
                                            "translation unit does not have (include the header "
                                            "that defines it alongside this one)";
                            if (LazyCompletableMemberClass(md->getReturnType()))
                                m.lazyNestedSpelling = bad;
                        }
                    }

                    // With definition emission on, an inline / defaulted / implicit member is a
                    // CANDIDATE: the linkage name and the ABI arrangement are produced here, and
                    // the emission pass clears needsLocalDefinition only for the ones Clang really
                    // emitted. Without it the member stays declaration-only, as before.
                    const bool emitCandidate = st.req.RecordsDefinitionDemand()
                                            || st.req.assumeInlineDefinitions;
                    // LSP has no CodeGen module, so assume every header-defined member has a
                    // callable declaration. A real compile still proves the body below.
                    if (st.req.assumeInlineDefinitions && !st.req.RecordsDefinitionDemand()
                        && (inlineMember || md->isImplicit() || md->isDefaulted()))
                    {
                        m.definitionAssumed = m.needsLocalDefinition;
                        m.needsLocalDefinition = false;
                    }
                    // A template PATTERN is not a symbol: it exists only so the type request's
                    // stub can name the signature. Leave it with no linkage name (refused at any
                    // use site) - stage 2 replaces it with the instantiated specialization.
                    const bool isPattern = md->getDescribedFunctionTemplate() != nullptr;
                    if (m.bindRefusal.empty() && !m.isDeleted && !isPattern
                        && (!m.needsLocalDefinition || emitCandidate))
                        m.linkageName = CxxLinkageName(ctx, MemberGlobalDecl(md));
                    LocOfRaw(md, m.file, m.line, m.col);

                    if (m.isDeleted && ctor != nullptr && ctor->isDefaultConstructor())
                        rec.hasDeletedDefaultCtor = true;
                    if (m.isDeleted && m.isCopyCtor) rec.hasDeletedCopyCtor = true;

                    // Only members with a real symbol get an ABI arrangement; the rest are
                    // exported for diagnostics only.
                    outDecls.push_back((m.bindRefusal.empty() && !m.isDeleted && !isPattern
                                        && (!m.needsLocalDefinition || emitCandidate))
                                       ? md : nullptr);
                    rec.members.push_back(std::move(m));
                }

                // A static data member of a PUBLIC base is nameable on the derived class in C++
                // (`ios_base::failbit` lives in MSVC's `_Iosb<int>` base), so the base chain
                // contributes every member the class itself does not shadow.
                std::vector<const VarDecl*> staticDataMembers;
                std::set<std::string> staticDataMemberNames;
                auto collectStaticDataMembers = [&](const CXXRecordDecl* rd, auto&& self) -> void {
                    for (const Decl* d : rd->decls())
                    {
                        const auto* vd = llvm::dyn_cast<VarDecl>(d);
                        if (vd == nullptr || !vd->isStaticDataMember()
                            || vd->getIdentifier() == nullptr)
                            continue;
                        if (staticDataMemberNames.insert(vd->getNameAsString()).second)
                            staticDataMembers.push_back(vd);
                    }
                    for (const CXXBaseSpecifier& base : rd->bases())
                    {
                        if (base.getAccessSpecifier() != clang::AS_public) continue;
                        const CXXRecordDecl* baseDecl = base.getType()->getAsCXXRecordDecl();
                        if (baseDecl == nullptr || baseDecl->getDefinition() == nullptr) continue;
                        self(baseDecl->getDefinition(), self);
                    }
                };
                collectStaticDataMembers(cxx, collectStaticDataMembers);

                for (const VarDecl* vd : staticDataMembers)
                {
                    auto rememberInitializerFailure = [&](RawCxxStaticVar& sv) {
                        if (vd->isInvalidDecl()
                            || (vd->getInit() != nullptr && vd->getInit()->containsErrors()))
                            sv.initializerFailure = "pending:" + vd->getQualifiedNameAsString();
                    };
                    // A non-constexpr `static const T k = 41;` initialized IN CLASS has no symbol
                    // to link against (odr-use is ill-formed), so fold it exactly like constexpr.
                    const bool foldsFromInClassInit =
                        !vd->isConstexpr() && !vd->isInline() && vd->hasInit();
                    if ((vd->isConstexpr() || foldsFromInClassInit) && vd->getInit() != nullptr
                        && !vd->getInit()->isValueDependent())
                    {
                        Expr::EvalResult result;
                        if (vd->getInit()->EvaluateAsRValue(result, ctx) && result.Val.isInt())
                        {
                            RawCxxStaticVar sv;
                            sv.name = vd->getNameAsString();
                            sv.ctype = CanonicalSpelling(ctx, vd->getType());
                            sv.isCompileTimeConstant = true;
                            sv.constantValue = ApsIntToLongLong(result.Val.getInt());
                            sv.access = MapAccess(vd->getAccess());
                            rememberInitializerFailure(sv);
                            LocOfRaw(vd, sv.file, sv.line, sv.col);
                            rec.staticVars.push_back(std::move(sv));
                            continue;
                        }
                        if (result.Val.isFloat())
                        {
                            RawCxxStaticVar sv;
                            sv.name = vd->getNameAsString();
                            sv.ctype = CanonicalSpelling(ctx, vd->getType());
                            sv.isCompileTimeConstant = true;
                            sv.isFloatConstant = true;
                            bool losesInfo = false;
                            sv.floatValue = ApFloatToDouble(result.Val.getFloat(), &losesInfo);
                            if (losesInfo && st.req.verbose)
                                std::cout << "[verbose]   C++ static member "
                                          << vd->getQualifiedNameAsString()
                                          << " rounded long double to double (loss of precision)\n";
                            sv.access = MapAccess(vd->getAccess());
                            rememberInitializerFailure(sv);
                            LocOfRaw(vd, sv.file, sv.line, sv.col);
                            rec.staticVars.push_back(std::move(sv));
                            continue;
                        }
                    }
                    // Inline, constexpr and implicit template static members are emitted into
                    // the request companion; other out-of-line definitions remain library-owned.
                    // With definition emission on the storage is emitted into the companion module
                    // (linkonce_odr, so several importers merge), which makes it a real symbol.
                    const VarDecl* definition = vd->getDefinition();
                    const bool isImplicitTemplateMember =
                        vd->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation;
                    // An out-of-line `inline int C::k = 17;` marks only the DEFINITION inline; the
                    // in-class declaration reached here is not.
                    const bool isInlineVar =
                        vd->isInline() || (definition != nullptr && definition->isInline());
                    const bool emitLocal =
                        (st.req.RecordsDefinitionDemand() || st.req.assumeInlineDefinitions)
                        && ((vd->isConstexpr() || isInlineVar || isImplicitTemplateMember)
                            && definition != nullptr);
                    // Not instantiated yet: bind the symbol now, emit the definition after the walk.
                    if (definition == nullptr && isImplicitTemplateMember
                        && st.req.RecordsDefinitionDemand()
                        && !vd->getType()->isDependentType())
                    {
                        st.pendingStaticVarDefs.push_back(const_cast<VarDecl*>(vd));
                        RawCxxStaticVar sv;
                        sv.name = vd->getNameAsString();
                        sv.ctype = CanonicalSpelling(ctx, vd->getType());
                        sv.access = MapAccess(vd->getAccess());
                        sv.linkageName = CxxLinkageName(ctx, vd);
                        rememberInitializerFailure(sv);
                        LocOfRaw(vd, sv.file, sv.line, sv.col);
                        rec.staticVars.push_back(std::move(sv));
                        continue;
                    }
                    // Why a static data member was left out is invisible at the use site
                    // ("'count' does not name a value"), so name the reason under -v.
                    auto skipStaticVar = [&](const char* why) {
                        if (st.req.verbose)
                            std::cout << "[verbose]   C++ static member "
                                      << vd->getQualifiedNameAsString()
                                      << " not bound: " << why << "\n";
                    };
                    if (!emitLocal && (vd->isConstexpr() || isInlineVar))
                    { skipStaticVar("inline or constexpr storage is emitted per TU"); continue; }
                    if (!emitLocal && vd->hasInit())
                    { skipStaticVar("its initializer lives in the header, so it has no library symbol"); continue; }
                    if (emitLocal && st.req.RecordsDefinitionDemand())
                        st.varEmitWork.push_back(definition);
                    RawCxxStaticVar sv;
                    sv.name = vd->getNameAsString();
                    sv.ctype = CanonicalSpelling(ctx, vd->getType());
                    sv.access = MapAccess(vd->getAccess());
                    sv.linkageName = CxxLinkageName(ctx, vd);
                    rememberInitializerFailure(sv);
                    LocOfRaw(vd, sv.file, sv.line, sv.col);
                    rec.staticVars.push_back(std::move(sv));
                }
            }

            /*
             * M6 - flatten one C++ subobject's storage into rec.fields at ABSOLUTE offsets.
             *
             * A CFlat struct has no notion of a base subobject or a vptr, so a class that has
             * either is laid out as ONE flat field list built from Clang's own ASTRecordLayout:
             * a `void *` slot wherever Clang put a vptr, then every base's storage at its base
             * offset, then the class's own fields. Downstream this is indistinguishable from a
             * plain C struct - the existing padding insertion and layout verification both work
             * unchanged - and inherited fields become directly nameable on the derived type.
             *
             * A field that must not be nameable (inherited through a non-public base, or shadowed
             * by a more derived field of the same name) keeps its storage but loses its name; the
             * backend already treats a nameless field as padding.
             *
             * Returns false when the layout cannot be flattened, in which case the caller records
             * a refusal and leaves the record opaque.
             */
            bool FlattenCxxLayout(const CXXRecordDecl* cxx, uint64_t baseOff, bool nameable,
                                  const std::set<std::string>& ownNames, bool isOutermost,
                                  std::set<std::string>& taken, int& synth, RawRecord& rec)
            {
                if (cxx->getNumVBases() > 0) return false;
                const ASTRecordLayout& layout = ctx.getASTRecordLayout(cxx);
                if (layout.hasOwnVFPtr())
                {
                    // Every flattened slot keeps a NAME: downstream, a nameless field means
                    // "padding cflat inserted" and the layout verifier skips it. The vptr is real
                    // storage, so it gets a reserved name and private access instead.
                    RawField vp;
                    vp.name = "__vptr" + std::to_string(synth++);
                    vp.ctype = "void *";
                    vp.offsetBytes = baseOff;
                    vp.access = AccessPrivate;
                    rec.fields.push_back(std::move(vp));
                }
                for (const CXXBaseSpecifier& b : cxx->bases())
                {
                    const auto* brd = b.getType()->getAsCXXRecordDecl();
                    if (brd == nullptr || brd->getDefinition() == nullptr) return false;
                    brd = brd->getDefinition();
                    const uint64_t off = baseOff
                        + (uint64_t)(b.isVirtual() ? layout.getVBaseClassOffset(brd)
                                                   : layout.getBaseClassOffset(brd)).getQuantity();
                    const bool basePublic = b.getAccessSpecifier() == AS_public;
                    if (!FlattenCxxLayout(brd, off, nameable && basePublic, ownNames,
                                          /*isOutermost*/ false, taken, synth, rec))
                        return false;
                }
                unsigned idx = 0;
                for (const FieldDecl* f : cxx->fields())
                {
                    // A bitfield or a transparent anonymous member inside a hierarchy would need
                    // the bitfield packer and the synthetic-record path to agree on absolute
                    // offsets; neither is wired for that, so refuse instead of mislaying it out.
                    if (f->isBitField() || f->getDeclName().isEmpty()) return false;
                    RawField rf;
                    rf.name = f->getNameAsString();
                    rf.ctype = CanonicalSpelling(ctx, f->getType());
                    rf.isZeroSize = f->isZeroSize(ctx);
                    rf.offsetBytes = baseOff + layout.getFieldOffset(idx) / 8;
                    RecordRawFieldLayout(f->getType(), rf);
                    rf.access = MapAccess(f->getAccess());
                    // Shadowed by a more derived field of the same name: keep the storage, give it
                    // a reserved name nobody can write. Inherited through a non-public base: keep
                    // the real name but mark it private, so naming it says exactly that.
                    const bool shadowedByDerived = !isOutermost && ownNames.count(rf.name) != 0;
                    if (shadowedByDerived || !taken.insert(rf.name).second)
                    {
                        rf.name = "__hidden" + std::to_string(synth++);
                        rf.access = AccessPrivate;
                    }
                    else if (!nameable)
                        rf.access = AccessPrivate;
                    rec.fields.push_back(std::move(rf));
                    ++idx;
                }
                return true;
            }

            /*
             * Own fields first, so a derived field wins the name over a base field that shadows
             * it, then sort the whole flat list by offset - InsertCxxLayoutPadding downstream
             * requires monotonically increasing offsets.
             */
            bool CollectCxxFlatFields(const CXXRecordDecl* cxx, RawRecord& rec)
            {
                std::set<std::string> ownNames;
                for (const FieldDecl* f : cxx->fields())
                    if (!f->getDeclName().isEmpty()) ownNames.insert(f->getNameAsString());
                std::set<std::string> taken;
                int synth = 0;
                if (!FlattenCxxLayout(cxx, 0, true, ownNames, /*isOutermost*/ true, taken, synth, rec))
                    return false;
                if (std::none_of(rec.fields.begin(), rec.fields.end(),
                        [](const RawField& f) { return f.isZeroSize; }))
                    std::stable_sort(rec.fields.begin(), rec.fields.end(),
                        [](const RawField& a, const RawField& b) { return a.offsetBytes < b.offsetBytes; });
                return true;
            }

            bool VisitRecordDecl(RecordDecl* rd)
            {
                if (!rd->getIdentifier()) return true;
                // Opaque forward-declared handle (e.g. `typedef struct SDL_Window SDL_Window;` with
                // no body anywhere in this TU). Register it as an empty-field shell so the backend
                // creates an opaque struct: usable through a pointer (the C handle idiom), while a
                // by-value use errors with "incomplete layout". A struct defined elsewhere in the TU
                // is handled by its own definition decl below, so skip when a definition exists.
                if (!rd->isThisDeclarationADefinition())
                {
                    // Header-bind path only: the in-scope filter confines this to the bound header's
                    // own types. The .c auto-extern path has no scope filter, so emitting here would
                    // register every opaque handle from system headers (FILE, ...).
                    if (!st.req.requireInScope) return true;
                    if (rd->getDefinition() != nullptr) return true;
                    std::string ofile; int oline = 1, ocol = 0;
                    if (!LocOfRaw(rd, ofile, oline, ocol)) return true;
                    if (!st.InScope(ofile)) return true;
                    std::string tag = rd->getNameAsString();
                    if (!st.emittedOpaqueForward.insert(tag).second) return true;
                    RawRecord rec;
                    rec.name = std::move(tag);
                    rec.isUnion = rd->isUnion();
                    rec.file = ofile; rec.line = oline; rec.col = ocol;
                    rec.physicalFile = PhysicalFileOf(rd, ofile);
                    rec.inScope = true;  // gated above; empty fields -> opaque shell downstream
                    st.out.records.push_back(std::move(rec));
                    return true;
                }

                EmitDefinedRecord(rd, std::string());
                return true;
            }

            /*
             * Export ONE defined record. `nameOverride` is non-empty only for an M5b foreign type
             * request (a class template specialization, or a class named through a typedef): the
             * record is registered under the CFlat spelling the request carries, the in-scope
             * filter does not apply to it (it lives in a system header), and its storage is
             * exported as an opaque BLOB of the right size and alignment instead of a field walk -
             * a libc++ container's fields are private implementation detail CFlat never names, and
             * several of them have no CFlat spelling at all.
             */
            /*
             * Export every class-template specialization a record holds BY VALUE, under the
             * foreign identity the backend keys such a type on. A named class is emitted by the
             * general traversal, but a ClassTemplateSpecializationDecl is not a child of its
             * DeclContext, so without this the enclosing layout has no record for the field and
             * embeds it as opaque bytes. Pointer and reference fields are skipped: a handle needs
             * no layout, so they neither force an instantiation nor recurse.
             */
            void EmitFieldSpecializationRecords(const RecordDecl* rd)
            {
                if (!st.req.cxxMode) return;
                for (const FieldDecl* f : rd->fields())
                {
                    QualType qt = f->getType();
                    while (const ConstantArrayType* cat = ctx.getAsConstantArrayType(qt))
                        qt = cat->getElementType();
                    qt = qt.getCanonicalType().getUnqualifiedType();
                    if (qt->isPointerType() || qt->isReferenceType()) continue;
                    auto* cxx = qt->getAsCXXRecordDecl();
                    if (cxx == nullptr || !llvm::isa<ClassTemplateSpecializationDecl>(cxx)) continue;
                    CXXRecordDecl* def = cxx->getDefinition();
                    if (def == nullptr || def->isInvalidDecl() || def->isDependentContext()) continue;
                    // Only a specialization declared by the bound header itself. A standard-library
                    // one (a std::shared_ptr member) is owned by the request path, which keys it on
                    // its own CFlat spelling - registering it here would claim that identity first.
                    std::string defFile; int defLine = 1, defCol = 0;
                    if (st.req.requireInScope
                        && (!LocOfRaw(def, defFile, defLine, defCol)
                            || !st.InScope(defFile)))
                        continue;
                    const std::string identity = CxxForeignIdentity(CanonicalSpelling(ctx, qt));
                    if (identity.empty()) continue;
                    EmitDefinedRecord(def, identity);
                }
            }

            void EmitDefinedRecord(RecordDecl* rd, const std::string& nameOverride,
                                   bool forcedBase = false)
            {
                // A class-template PATTERN, a partial specialization, or any record nested inside
                // one is DEPENDENT: it has no record layout, and asking clang for one recurses
                // until the stack dies in an assertions-off build. Only the complete
                // specializations a request names (nameOverride) carry a layout.
                if (const auto* dep = llvm::dyn_cast<CXXRecordDecl>(rd))
                    if (dep->getDescribedClassTemplate() != nullptr
                        || llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(dep)
                        || dep->isDependentContext())
                        return;
                // A specialization that is only NAMED (a declared function's return type) is
                // never instantiated: it has no definition and no layout. Skip it here.
                if (!rd->isCompleteDefinition()) return;
                std::string file; int line = 1, col = 0;
                // Collect records regardless of scope (LocOfRaw, not LocOf): an in-scope struct
                // may reference an out-of-scope struct by value (e.g. MSG.pt is a POINT defined
                // in the SDK shared/ dir). The backend keeps the transitive closure of in-scope
                // records and drops the rest, so the dependency is available without registering
                // every unrelated SDK struct.
                if (!LocOfRaw(rd, file, line, col)) return;
                // Do not walk standard-library template catalogs as transitive C++ layout.
                if (st.req.requireInScope && nameOverride.empty() && !forcedBase
                    && llvm::isa<CXXRecordDecl>(rd) && !st.InScope(file)) return;
                if (nameOverride.empty()
                    && !st.emittedDefinedRecords.insert(rd).second)
                    return;
                if (!nameOverride.empty()
                    && !st.emittedRequestedRecords.insert(nameOverride).second)
                    return;
                // A hidden friend operator is reachable only through its own class. The
                // incremental request walk never visits it, so publish it with the record.
                if (st.req.cxxMode && !nameOverride.empty())
                    if (const auto* cxx = llvm::dyn_cast<CXXRecordDecl>(rd))
                        for (const FriendDecl* fr : cxx->friends())
                        {
                            auto* fn = llvm::dyn_cast_or_null<FunctionDecl>(fr->getFriendDecl());
                            if (fn == nullptr || fn->isInvalidDecl()
                                || fn->getType()->isDependentType())
                                continue;
                            switch (fn->getOverloadedOperator())
                            {
                            case OO_EqualEqual: case OO_ExclaimEqual: case OO_Less:
                            case OO_LessEqual: case OO_Greater: case OO_GreaterEqual:
                            case OO_Spaceship:
                                break;
                            default: continue;
                            }
                            // Only the homogeneous ADL comparisons of the requested type: a
                            // mixed-type friend drags in specializations this request never asked
                            // for.
                            const std::string self = CanonicalSpelling(
                                ctx, ctx.getCanonicalTagType(cxx));
                            bool homogeneous = fn->getNumParams() == 2;
                            for (const ParmVarDecl* p : fn->parameters())
                            {
                                QualType bare = p->getType().getNonReferenceType()
                                                    .getUnqualifiedType();
                                if (CanonicalSpelling(ctx, bare) != self) homogeneous = false;
                            }
                            if (homogeneous) st.pendingFriendOps.push_back(fn);
                        }

                RawRecord rec;
                rec.name = rd->getNameAsString();
                // An unnamed record keeps an empty name - the caller synthesizes its tag, and a
                // qualified spelling would hand it the "(unnamed struct at ...)" placeholder.
                rec.qualifiedName = rec.name;
                if (!nameOverride.empty())
                {
                    rec.name = nameOverride;
                    rec.qualifiedName = nameOverride;
                }
                else if (st.req.cxxMode && !rec.name.empty())
                {
                    if (rd->isInAnonymousNamespace()) return;
                    if (const auto* cxx = llvm::dyn_cast<CXXRecordDecl>(rd))
                    {
                        if (const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(cxx);
                            spec != nullptr
                            && spec->getSpecializationKind() == TSK_ExplicitSpecialization)
                            rec.qualifiedName = CxxForeignIdentity(CanonicalSpelling(
                                ctx, ctx.getCanonicalTagType(rd)));
                        else
                            rec.qualifiedName = CxxQualifiedName(rd);
                    }
                    else
                        rec.qualifiedName = CxxQualifiedName(rd);
                    if (!IsValidDottedName(rec.qualifiedName)) return;
                    rec.name = rec.qualifiedName;
                }
                rec.isUnion = rd->isUnion();
                rec.isCxx = st.req.cxxMode;
                rec.file = file; rec.line = line; rec.col = col;
                rec.physicalFile = PhysicalFileOf(rd, file);
                rec.inScope = forcedBase || !nameOverride.empty() || !st.req.requireInScope
                           || st.InScope(file);
                if (st.req.cxxMode && llvm::isa<CXXRecordDecl>(rd))
                    rec.canonicalCtype = CanonicalSpelling(ctx, ctx.getCanonicalTagType(rd));
                const ASTRecordLayout& layout = ctx.getASTRecordLayout(rd);
                rec.sizeBytes = layout.getSize().getQuantity();
                rec.alignBytes = layout.getAlignment().getQuantity();
                // #pragma pack(N) yields MaxFieldAlignmentAttr, not PackedAttr.
                rec.isPacked = rd->hasAttr<PackedAttr>() || rd->hasAttr<MaxFieldAlignmentAttr>();
                std::vector<const CXXMethodDecl*> memberDecls;
                bool flattened = false;
                if (const auto* cxx = llvm::dyn_cast<CXXRecordDecl>(rd))
                {
                    rec.isTrivial = cxx->isTrivial();
                    rec.isTriviallyCopyable = cxx->isTriviallyCopyable()
                        && !cxx->hasNonTrivialDestructor() && !cxx->isPolymorphic();
                    rec.isTriviallyRelocatable = st.ci != nullptr && st.ci->hasSema()
                        && st.ci->getSema().IsCXXTriviallyRelocatableType(*cxx);
                    if (cxx->hasDefinition())
                    {
                        if (cxx->getDefinition()->isInvalidDecl())
                            rec.layoutRefusal = InvalidDefinitionRefusal(cxx->getDefinition());
                        if (rec.inScope && nameOverride.empty())
                            PrepareHeaderSpecialMembers(cxx);
                        if (rec.inScope || !st.req.requireInScope || !nameOverride.empty())
                        {
                            CollectCxxMembers(cxx, rec, memberDecls);
                            rec.specialMembersPending = DeferHeaderSpecialMembers(st)
                                && HasDeferredSpecialMemberWork(ctx, cxx);
                            if (rec.specialMembersPending && st.req.specialMemberRecords != nullptr)
                                (*st.req.specialMemberRecords)[rec.name] =
                                    CompleteNonDependentCxxRecord(cxx);
                        }
                        for (const CXXBaseSpecifier& b : cxx->bases())
                        {
                            const auto* brd = b.getType()->getAsCXXRecordDecl();
                            RawCxxBase rb;
                            rb.canonicalType = CanonicalSpelling(ctx, b.getType());
                            rb.name = rb.canonicalType.empty() ? (brd != nullptr
                                ? CxxQualifiedName(brd) : std::string())
                                : CxxForeignIdentity(rb.canonicalType);
                            rb.access = MapAccess(b.getAccessSpecifier());
                            rb.isVirtual = b.isVirtual();
                            if (brd != nullptr && brd->getDefinition() != nullptr)
                                rb.offsetBytes = (uint64_t)(b.isVirtual()
                                    ? ctx.getASTRecordLayout(cxx).getVBaseClassOffset(brd->getDefinition())
                                    : ctx.getASTRecordLayout(cxx).getBaseClassOffset(brd->getDefinition())).getQuantity();
                            rec.bases.push_back(std::move(rb));
                        }
                        const clang::ASTRecordLayout& selfLayout = ctx.getASTRecordLayout(cxx);
                        for (const CXXBaseSpecifier& vb : cxx->vbases())
                        {
                            const auto* vrd = vb.getType()->getAsCXXRecordDecl();
                            if (vrd == nullptr || vrd->getDefinition() == nullptr) continue;
                            RawCxxBase rb;
                            rb.canonicalType = CanonicalSpelling(ctx, vb.getType());
                            rb.name = rb.canonicalType.empty() ? CxxQualifiedName(vrd)
                                : CxxForeignIdentity(rb.canonicalType);
                            rb.access = MapAccess(vb.getAccessSpecifier());
                            rb.isVirtual = true;
                            rb.offsetBytes = (uint64_t)selfLayout
                                .getVBaseClassOffset(vrd->getDefinition()).getQuantity();
                            rec.virtualBases.push_back(std::move(rb));
                        }
                        // A vptr or a base subobject has no CFlat spelling, so the layout is
                        // flattened out of Clang's own record layout instead of read field by
                        // field. Virtual inheritance needs a VTT and is refused outright.
                        flattened = rec.hasBases || rec.isPolymorphic;
                        if (flattened)
                        {
                            if (rec.hasVirtualBases)
                                rec.layoutRefusal = "uses virtual inheritance, which is not supported yet";
                            else if (!CollectCxxFlatFields(cxx, rec))
                                rec.layoutRefusal = "has a layout cflat cannot flatten (a bitfield or "
                                                    "an anonymous member inside a class hierarchy)";
                            // Refused: drop whatever the partial flatten produced and fall back to
                            // the plain field walk. The record is never laid out (the backend keeps
                            // an opaque shell), but the field list still drives the access-control
                            // diagnostic, so naming a member says WHY instead of "unknown".
                            if (!rec.layoutRefusal.empty())
                            {
                                rec.fields.clear();
                                flattened = false;
                            }
                        }
                    }
                }
                const bool pairValue = nameOverride.starts_with("std.pair$");
                if (!nameOverride.empty() && !pairValue && !flattened)
                {
                    bool hasPublicField = false;
                    const size_t nestedStart = st.out.records.size();
                    if (rec.layoutRefusal.empty())
                    {
                        CollectFields(rd, rec.name, rec);
                        hasPublicField = std::any_of(rec.fields.begin(), rec.fields.end(),
                            [](const RawField& field) {
                                return !field.isBitfield && field.access == AccessPublic
                                    && !field.name.starts_with("__anon");
                            });
                    }
                    if (!hasPublicField)
                    {
                        st.out.records.erase(st.out.records.begin() + nestedStart,
                                             st.out.records.end());
                        rec.fields.clear();
                        rec.layoutRefusal.clear();
                        EmitBlobStorage(rec);
                    }
                }
                else if (!flattened) CollectFields(rd, rec.name, rec);
                // Before the owner, so the batch already holds every by-value field's record.
                if (rec.layoutRefusal.empty()) EmitFieldSpecializationRecords(rd);
                st.out.records.push_back(std::move(rec));
                if (!memberDecls.empty())
                {
                    const size_t recIdx = st.out.records.size() - 1;
                    for (size_t i = 0; i < memberDecls.size(); ++i)
                        if (memberDecls[i] != nullptr)
                            st.memberAbiWork.push_back({ recIdx, i, memberDecls[i] });
                }
                if (rec.inScope && nameOverride.empty())
                    if (const auto* cxx = llvm::dyn_cast<CXXRecordDecl>(rd))
                    {
                        std::function<void(const CXXRecordDecl*)> emitBases;
                        emitBases = [&](const CXXRecordDecl* current) {
                            for (const CXXBaseSpecifier& b : current->bases())
                            {
                                const auto* base = b.getType()->getAsCXXRecordDecl();
                                if (base == nullptr || base->getDefinition() == nullptr) continue;
                                CXXRecordDecl* def = base->getDefinition();
                                const std::string baseType = CanonicalSpelling(ctx, b.getType());
                                EmitDefinedRecord(def, baseType.empty()
                                                       ? std::string()
                                                       : CxxForeignIdentity(baseType), true);
                                emitBases(def);
                            }
                        };
                        emitBases(cxx);
                    }
            }

            /*
             * Replace a requested record's field list with storage of the same size and alignment:
             * one array of the widest integer the alignment allows. CFlat then lays out a struct
             * that is byte-compatible with the C++ object, which is all a foreign class needs -
             * construction, destruction and every member call go through Clang's own symbols.
             */
            void EmitBlobStorage(RawRecord& rec)
            {
                uint64_t align = rec.alignBytes == 0 ? 1 : rec.alignBytes;
                if (align > 8) align = 8;
                while (align > 1 && rec.sizeBytes % align != 0) align /= 2;
                const char* elem = align == 8 ? "unsigned long long"
                                 : align == 4 ? "unsigned int"
                                 : align == 2 ? "unsigned short" : "unsigned char";
                const uint64_t count = align == 0 ? rec.sizeBytes : rec.sizeBytes / align;
                if (count == 0) return;
                RawField f;
                f.name = "__cxx_storage";
                f.ctype = std::string(elem) + "[" + std::to_string(count) + "]";
                f.access = AccessPrivate;
                f.offsetBytes = 0;
                rec.fields.push_back(std::move(f));
            }

            /*
             * Resolve every M5b type request through its marker typedef in the stub and export the
             * record it names. The general traversal is skipped in request mode, so this is the
             * only producer of records. A ClassTemplateSpecializationDecl is not a child of its
             * DeclContext, which is why the typedef (a real top-level decl) is the handle.
             */
            /*
             * A `decltype(<variable>)` request marker: publish the variable's constant value under
             * the marker name. A variable-template specialization is instantiated first (decltype
             * is unevaluated, so Sema has not done it). Non-constant or non-scalar -> nothing.
             */
            // constexpr, or a const whose initializer is a side-effect-free constant initializer.
            bool IsFoldableConstantInit(const VarDecl* vd, const Expr* init) const
            {
                if (vd->isConstexpr()) return true;
                return !init->HasSideEffects(ctx)
                    && init->isConstantInitializer(ctx, vd->getType()->isReferenceType());
            }

            /*
             * A class-typed constant: published under the marker with the symbol a CFlat use
             * links to. `def` set = header-owned storage (inline / constexpr), which the request's
             * definitions stage emits (linkonce_odr; an internal one gets the per-group alias the
             * header harvest uses); null = an `extern` object the library exports.
             */
            void PublishRequestedClassConstant(const VarDecl* vd, const VarDecl* def,
                                               const std::string& marker)
            {
                RawGlobalVar g;
                g.name = marker;
                g.qualifiedName = marker;
                g.ctype = CanonicalSpelling(ctx, vd->getType().getUnqualifiedType());
                g.isConst = vd->getType().isConstQualified() || vd->isConstexpr();
                g.isDllImport = def == nullptr && vd->hasAttr<DLLImportAttr>();
                LocOfRaw(vd, g.file, g.line, g.col);
                const std::string originalLinkage = CxxLinkageName(ctx, vd);
                g.linkageName = originalLinkage;
                if (def != nullptr)
                {
                    g.isCxxConstexpr = true;
                    if (!vd->hasExternalFormalLinkage() || vd->getStorageClass() == SC_Static)
                    {
                        g.linkageName = StaticCxxGlobalAlias(st.req.cxxImportGroupKey, originalLinkage);
                        if (st.req.demandPlan != nullptr)
                            st.req.demandPlan->renamed[g.linkageName] = originalLinkage;
                        st.out.weakPromoteSymbols.push_back(originalLinkage);
                        st.out.weakPromotePerGroupSymbols.push_back(originalLinkage);
                    }
                    if (st.req.RecordsDefinitionDemand()) st.varEmitWork.push_back(def);
                }
                st.out.globals.push_back(std::move(g));
            }

            void EmitRequestedConstant(const DecltypeType* dt, const TypedefNameDecl* td,
                                       const std::string& marker)
            {
                const auto* ref = llvm::dyn_cast_or_null<DeclRefExpr>(
                    dt->getUnderlyingExpr() != nullptr ? dt->getUnderlyingExpr()->IgnoreParens() : nullptr);
                // An unscoped enumerator reached through its parent scope: name the enum type so
                // the CFlat side binds that enum and the enumerator under the requested path.
                if (const auto* ec = ref != nullptr ? llvm::dyn_cast<EnumConstantDecl>(ref->getDecl()) : nullptr)
                {
                    const auto* ed = llvm::dyn_cast<EnumDecl>(ec->getDeclContext());
                    if (ed == nullptr || ed->isScoped() || ed->getIdentifier() == nullptr
                        || ed->isDependentContext())
                        return;
                    RawGlobalVar g;
                    g.name = marker;
                    g.qualifiedName = marker;
                    g.ctype = CanonicalSpelling(ctx, ec->getType().getUnqualifiedType());
                    g.isConst = true;
                    g.isCompileTimeConstant = true;
                    g.isEnumerator = true;
                    g.constantValue = ApsIntToLongLong(ec->getInitVal());
                    LocOfRaw(ec, g.file, g.line, g.col);
                    st.out.globals.push_back(std::move(g));
                    return;
                }
                auto* vd = ref != nullptr ? llvm::dyn_cast<VarDecl>(const_cast<ValueDecl*>(ref->getDecl())) : nullptr;
                if (vd == nullptr || !vd->isFileVarDecl() || vd->isStaticDataMember()) return;
                // A non-const `extern` class object the library exports (std::cout): an lvalue
                // of its class, bound by its symbol like an extern class constant.
                if (!vd->getType().isConstQualified() && !vd->isConstexpr()
                    && !vd->getType()->isDependentType()
                    && vd->getType().getCanonicalType()->getAsCXXRecordDecl() != nullptr
                    && vd->getDefinition() == nullptr && vd->getAnyInitializer() == nullptr
                    && vd->hasExternalFormalLinkage() && !vd->isInline()
                    && vd->getStorageClass() == SC_Extern)
                {
                    PublishRequestedClassConstant(vd, nullptr, marker);
                    return;
                }
                if (!vd->getType().isConstQualified() && !vd->isConstexpr()) return;
                if (vd->getAnyInitializer() == nullptr && st.ci != nullptr && st.ci->hasSema()
                    && llvm::isa<VarTemplateSpecializationDecl>(vd))
                    st.ci->getSema().InstantiateVariableDefinition(td->getLocation(), vd);
                const VarDecl* def = vd->getDefinition();
                const Expr* init = def != nullptr ? def->getInit() : vd->getAnyInitializer();
                // A class-typed constant (std::chrono::February, std::nullopt, a tag object) has
                // no scalar to fold: the real object is bound by its symbol.
                const bool classTyped = !vd->getType()->isDependentType()
                    && vd->getType().getCanonicalType()->getAsCXXRecordDecl() != nullptr;
                if (classTyped && def == nullptr && init == nullptr)
                {
                    // `extern const __ph<1> _1;`: the library exports the object.
                    if (vd->hasExternalFormalLinkage() && !vd->isInline() && !vd->isConstexpr())
                        PublishRequestedClassConstant(vd, nullptr, marker);
                    return;
                }
                if (init == nullptr || init->containsErrors() || init->isValueDependent()) return;
                // Folding skips the initializer, so only a constant initialization may fold
                // (`inline const int v = (++counter, 27);` must run). No live binding here.
                if (!IsFoldableConstantInit(vd, init))
                {
                    st.out.invalidCxxTypeRequestError = std::format(
                        "C++ variable '{}' {}", CxxQualifiedName(vd), kCxxNotConstantVariableRefusal);
                    return;
                }
                if (classTyped)
                {
                    if (def != nullptr) PublishRequestedClassConstant(vd, def, marker);
                    return;
                }
                Expr::EvalResult result;
                if (!init->EvaluateAsRValue(result, ctx)) return;
                RawGlobalVar g;
                g.name = marker;
                g.qualifiedName = marker;
                g.ctype = CanonicalSpelling(ctx, vd->getType().getUnqualifiedType());
                g.isConst = true;
                g.isCompileTimeConstant = true;
                g.isCxxConstexpr = true;
                if (result.Val.isInt())
                    g.constantValue = ApsIntToLongLong(result.Val.getInt());
                else if (result.Val.isFloat())
                {
                    bool losesInfo = false;
                    g.isFloatConstant = true;
                    g.floatValue = ApFloatToDouble(result.Val.getFloat(), &losesInfo);
                }
                else
                    return;
                LocOfRaw(vd, g.file, g.line, g.col);
                st.out.globals.push_back(std::move(g));
            }

            bool ProcessTypeRequests(TranslationUnitDecl* root)
            {
                // A completion request names header records by CFlat identity; its markers are
                // placeholders. Define what the header harvest deferred, then harvest the record.
                if (st.req.completeCxxSpecialMembers)
                {
                    std::vector<std::pair<const CXXRecordDecl*, std::string>> batch;
                    for (const auto& request : st.req.cxxTypeRequests)
                    {
                        if (st.req.specialMemberRecords == nullptr) break;
                        auto& index = *st.req.specialMemberRecords;
                        auto found = index.find(request.cflatName);
                        // A group opened without a live harvest (warm cache) has no index yet.
                        if (found == index.end() && index.emplace("#indexed", nullptr).second)
                        {
                            // Each incremental chunk has its own TU decl; walk the chain.
                            for (const TranslationUnitDecl* tu : root->redecls())
                                IndexSpecialMemberRecords(ctx, tu, index);
                            found = index.find(request.cflatName);
                        }
                        if (found == index.end() || found->second == nullptr) continue;
                        // Snapshot the whole batch first: one record's completion can define
                        // another's members, and those count as defined by the completion.
                        for (const CXXMethodDecl* md : found->second->methods())
                            if (md->isDefined()) st.completionPredefined.insert(md);
                        batch.emplace_back(found->second, request.cflatName);
                    }
                    for (const auto& [decl, name] : batch)
                        CompleteRequestedSpecialMembers(st, ctx, decl);
                    for (const auto& [decl, name] : batch)
                        EmitDefinedRecord(const_cast<CXXRecordDecl*>(decl), name);
                    return true;
                }
                std::vector<Decl*> operatorCandidates;
                auto isComparisonOperator = [](OverloadedOperatorKind op) {
                    switch (op)
                    {
                        case OO_EqualEqual: case OO_ExclaimEqual:
                        case OO_Less: case OO_Greater:
                        case OO_LessEqual: case OO_GreaterEqual:
                            return true;
                        default: return false;
                    }
                };
                auto collectCandidates = [&](auto&& self, Decl* decl,
                                             std::vector<Decl*>& candidates) -> void {
                    if (auto* fd = llvm::dyn_cast<FunctionDecl>(decl))
                    {
                        bool concreteFunction = !fd->getReturnType()->isDependentType();
                        if (concreteFunction)
                            for (const ParmVarDecl* p : fd->parameters())
                                concreteFunction = concreteFunction
                                    && !p->getType()->isDependentType();
                        const bool instantiatedFriend = !llvm::isa<CXXMethodDecl>(fd)
                            && concreteFunction;
                        const bool specialization = fd->isFunctionTemplateSpecialization()
                            || (fd->getPrimaryTemplate() != nullptr
                                && fd->getDescribedFunctionTemplate() == nullptr);
                        if (!llvm::isa<CXXMethodDecl>(fd)
                            && (specialization || instantiatedFriend)
                            && isComparisonOperator(fd->getOverloadedOperator()))
                            candidates.push_back(decl);
                        return;
                    }
                    if (auto* ftd = llvm::dyn_cast<FunctionTemplateDecl>(decl))
                    {
                        const FunctionDecl* pattern = ftd->getTemplatedDecl();
                        if (IsBindableFreeOperatorTemplate(pattern))
                            candidates.push_back(decl);
                    }
                    if (auto* dc = llvm::dyn_cast<DeclContext>(decl))
                        for (Decl* child : dc->decls()) self(self, child, candidates);
                };
                TranslationUnitDecl* indexRoot = st.contextRoot != nullptr
                    ? st.contextRoot : root;
                const DeclContext* cacheRoot = indexRoot;
                std::vector<Decl*> contextCandidates;
                if (st.req.operatorIndex != nullptr)
                {
                    auto& index = *st.req.operatorIndex;
                    auto found = index.find(indexRoot);
                    if (found == index.end())
                    {
                        std::vector<Decl*> indexed;
                        collectCandidates(collectCandidates, indexRoot, indexed);
                        found = index.emplace(indexRoot, std::move(indexed)).first;
                    }
                    contextCandidates = found->second;
                }
                else
                    collectCandidates(collectCandidates, indexRoot, contextCandidates);
                if (cacheRoot != root)
                    collectCandidates(collectCandidates, root, operatorCandidates);
                operatorCandidates.insert(operatorCandidates.end(), contextCandidates.begin(),
                                          contextCandidates.end());
                for (size_t i = 0; i < st.req.cxxTypeRequests.size(); ++i)
                {
                    const std::string marker = st.req.cxxRequestMarkerPrefix
                                              + std::to_string(i);
                    const TypedefNameDecl* td = nullptr;
                    for (Decl* d : root->decls())
                    {
                        auto* cand = llvm::dyn_cast<TypedefNameDecl>(d);
                        if (cand != nullptr && cand->getNameAsString() == marker) { td = cand; break; }
                    }
                    if (td == nullptr) continue;
                    // `decltype(ns::v)` / `decltype(ns::v_t<T>)`: a namespace-scope constant
                    // (std::numbers::pi), folded to its value; never bound as a type.
                    if (const auto* dt = llvm::dyn_cast<DecltypeType>(
                            td->getUnderlyingType().getTypePtr()))
                    {
                        EmitRequestedConstant(dt, td, marker);
                        if (!st.out.invalidCxxTypeRequestError.empty()) return false;
                        continue;
                    }
                    QualType canon = td->getUnderlyingType().getCanonicalType();
                    auto* cxx = canon->getAsCXXRecordDecl();
                    if (cxx == nullptr)
                    {
                        /*
                         * An enum (std::byte, std::endian): a head entry named by the marker carries
                         * the type, then every enumerator as "<marker>.<name>". The enum may live
                         * outside the header's in-scope region, so no in-scope gate applies here.
                         */
                        if (const auto* et = canon->getAs<EnumType>();
                            et != nullptr && !canon->isDependentType())
                        {
                            const EnumDecl* ed = et->getDecl()->getDefinition();
                            if (ed != nullptr && !ed->getIntegerType().isNull())
                            {
                                RawEnum head;
                                head.name = marker;
                                head.enumType = CxxQualifiedName(ed);
                                head.underlyingType = CanonicalSpelling(ctx, ed->getIntegerType());
                                if (!ed->isScoped() && !ed->getPromotionType().isNull())
                                    head.promotedType = CanonicalSpelling(ctx, ed->getPromotionType());
                                head.isScoped = ed->isScoped();
                                LocOfRaw(ed, head.file, head.line, head.col);
                                st.out.enums.push_back(head);
                                for (const EnumConstantDecl* ec : ed->enumerators())
                                {
                                    if (!ec->getIdentifier()) continue;
                                    RawEnum e = head;
                                    e.name = marker + "." + ec->getNameAsString();
                                    e.value = ApsIntToLongLong(ec->getInitVal());
                                    LocOfRaw(ec, e.file, e.line, e.col);
                                    st.out.enums.push_back(std::move(e));
                                }
                            }
                            continue;
                        }
                        // A typedef of a builtin or a pointer to one (::uint32_t, ::intptr_t), or
                        // a function pointer (std::new_handler): clang's canonical spelling.
                        QualType leaf = canon;
                        while (leaf->isPointerType()) leaf = leaf->getPointeeType().getCanonicalType();
                        if (!canon->isDependentType()
                            && (leaf->isBuiltinType() || canon->isFunctionPointerType()))
                        {
                            RawTypedef t;
                            t.name = marker;
                            FillTypedefUnderlying(td->getUnderlyingType(), t);
                            st.out.typedefs.push_back(std::move(t));
                        }
                        continue;
                    }
                    CXXRecordDecl* def = cxx->getDefinition();
                    // A request spelled as a plain ALIAS carries no explicit instantiation and a
                    // typedef never requires completeness, so complete it silently through Sema.
                    // A member class of a specialization (NamedDict<K, V>::Item) instantiates too.
                    if (def == nullptr && st.ci != nullptr && st.ci->hasSema()
                        && (llvm::isa<ClassTemplateSpecializationDecl>(cxx)
                            || cxx->getInstantiatedFromMemberClass() != nullptr))
                    {
                        st.ci->getSema().isCompleteType(td->getLocation(), canon);
                        def = cxx->getDefinition();
                    }
                    if (cxx->isInvalidDecl() || (def != nullptr && def->isInvalidDecl()))
                    {
                        st.out.invalidCxxTypeRequestError = InvalidTypeRequestRefusal(
                            def != nullptr ? def : cxx,
                            st.req.cxxTypeRequests[i].cxxSpelling);
                        return false;
                    }
                    if (def == nullptr) continue;
                    std::function<void(const CXXRecordDecl*)> emitBases;
                    emitBases = [&](const CXXRecordDecl* current) {
                        for (const CXXBaseSpecifier& base : current->bases())
                        {
                            const auto* baseDecl = base.getType()->getAsCXXRecordDecl();
                            if (baseDecl == nullptr || baseDecl->getDefinition() == nullptr) continue;
                            emitBases(baseDecl->getDefinition());
                            const std::string baseType = CanonicalSpelling(ctx, base.getType());
                            EmitDefinedRecord(baseDecl->getDefinition(),
                                              CxxForeignIdentity(baseType), true);
                        }
                    };
                    emitBases(def);
                    EmitDefinedRecord(def, st.req.cxxTypeRequests[i].cflatName);
                    auto mentionsRequested = [&](QualType qt) {
                        qt = qt.getCanonicalType();
                        if (qt->isReferenceType()) qt = qt->getPointeeType().getCanonicalType();
                        qt = qt.getUnqualifiedType().getCanonicalType();
                        const auto* rd = qt->getAsCXXRecordDecl();
                        return qt == canon || (rd != nullptr
                            && rd->getCanonicalDecl() == cxx->getCanonicalDecl());
                    };
                    /*
                     * The requested type is normally a class-template SPECIALIZATION, and the
                     * free operators over it are declared as function TEMPLATES (every libc++
                     * basic_string operator is). Their parameters are dependent, so the concrete
                     * match above never sees them; match the class template itself instead and
                     * publish the operator template for on-demand instantiation.
                     */
                    const ClassTemplateDecl* requestedTemplate = nullptr;
                    if (const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(cxx);
                        spec != nullptr && spec->getSpecializedTemplate() != nullptr)
                        requestedTemplate = spec->getSpecializedTemplate()->getCanonicalDecl();
                    auto mentionsRequestedTemplate = [&](QualType qt) {
                        if (requestedTemplate == nullptr) return false;
                        if (qt->isReferenceType()) qt = qt->getPointeeType();
                        qt = qt.getUnqualifiedType();
                        const Type* type = qt.getTypePtrOrNull();
                        if (type == nullptr) return false;
                        if (const auto* tst = type->getAs<TemplateSpecializationType>())
                        {
                            const auto* ctd = llvm::dyn_cast_or_null<ClassTemplateDecl>(
                                tst->getTemplateName().getAsTemplateDecl());
                            if (ctd != nullptr && ctd->getCanonicalDecl() == requestedTemplate)
                                return true;
                        }
                        if (const auto* injected = type->getAs<InjectedClassNameType>())
                        {
                            const CXXRecordDecl* rd = injected->getDecl();
                            if (rd != nullptr && rd->getDescribedClassTemplate() != nullptr
                                && rd->getDescribedClassTemplate()->getCanonicalDecl()
                                       == requestedTemplate)
                                return true;
                        }
                        return false;
                    };
                    std::unordered_set<const FunctionDecl*> seenOperators;
                    std::vector<Decl*> matchedOperatorDecls;
                    auto collectOperator = [&](Decl* decl) {
                        const auto* fd = llvm::dyn_cast<FunctionDecl>(decl);
                        bool concreteFunction = fd != nullptr && !fd->getReturnType()->isDependentType();
                        if (concreteFunction)
                            for (const ParmVarDecl* p : fd->parameters())
                                concreteFunction = concreteFunction
                                    && !p->getType()->isDependentType();
                        const bool instantiatedFriend = fd != nullptr
                            && !llvm::isa<CXXMethodDecl>(fd)
                            && concreteFunction;
                        const bool specialization = fd != nullptr
                            && (fd->isFunctionTemplateSpecialization()
                                || (fd->getPrimaryTemplate() != nullptr
                                    && fd->getDescribedFunctionTemplate() == nullptr));
                        if ((!specialization && !instantiatedFriend) || llvm::isa<CXXMethodDecl>(fd)
                            || !seenOperators.insert(fd).second)
                            return;
                        if (!isComparisonOperator(fd->getOverloadedOperator())) return;
                        bool matches = false;
                        for (const ParmVarDecl* p : fd->parameters())
                            matches = matches || mentionsRequested(p->getType());
                        if (matches) matchedOperatorDecls.push_back(const_cast<FunctionDecl*>(fd));
                    };
                    auto collectOperatorTemplate = [&](Decl* decl) {
                        auto* ftd = llvm::dyn_cast<FunctionTemplateDecl>(decl);
                        if (ftd == nullptr) return;
                        const FunctionDecl* pattern = ftd->getTemplatedDecl();
                        if (!IsBindableFreeOperatorTemplate(pattern)) return;
                        bool matches = false;
                        for (const ParmVarDecl* p : pattern->parameters())
                            matches = matches || mentionsRequestedTemplate(p->getType());
                        if (matches) matchedOperatorDecls.push_back(ftd);
                    };
                    auto collectFrom = [&](const std::vector<Decl*>& decls) {
                        seenOperators.clear();
                        matchedOperatorDecls.clear();
                        for (Decl* decl : decls)
                        {
                            collectOperator(decl);
                            collectOperatorTemplate(decl);
                        }
                    };
                    /*
                     * Requested specializations and their instantiated friends are reached
                     * through the context walk, but only a small fraction are operators. Index
                     * the cheap-filtered declarations once and keep their original walk order.
                     */
                    std::vector<Decl*> candidates = operatorCandidates;
                    candidates.insert(candidates.end(), st.announcedDecls.begin(),
                                      st.announcedDecls.end());
                    collectFrom(candidates);
                    for (Decl* decl : matchedOperatorDecls)
                    {
                        if (auto* fd = llvm::dyn_cast<FunctionDecl>(decl))
                            VisitFunctionDecl(fd);
                        else if (auto* ftd = llvm::dyn_cast<FunctionTemplateDecl>(decl))
                            VisitFunctionTemplateDecl(ftd);
                    }
                    std::string ret;
                    std::string params;
                    const std::string& spelling = st.req.cxxTypeRequests[i].cxxSpelling;
                    if (SplitStdFunctionSpelling(spelling, ret, params))
                    {
                        auto& rec = st.out.records.back();
                        RawCxxMember ctor;
                        ctor.kind = RawCxxMember::Constructor;
                        ctor.name = "__ctor";
                        ctor.linkageName = "__cflat_std_function_ctor_";
                        for (char c : spelling)
                            ctor.linkageName += std::isalnum((unsigned char)c) ? c : '_';
                        ctor.retType = "void *";
                        ctor.paramTypes = { spelling + " *", ret + " (*) (" + params + ")" };
                        ctor.paramNames = { "this", "a1" };
                        ctor.returnsThis = true;
                        ctor.access = AccessPublic;
                        ctor.abi.valid = true;
                        ctor.abi.ret.kind = RawAbiSlot::Direct;
                        ctor.abi.params.resize(2);
                        ctor.abi.params[0].kind = RawAbiSlot::Direct;
                        ctor.abi.params[1].kind = RawAbiSlot::Direct;
                        rec.members.push_back(std::move(ctor));
                    }
                }
                return true;
            }

            void ProcessFunctionRequests(TranslationUnitDecl* root)
            {
                std::function<void(Decl*)> walk = [&](Decl* decl) {
                    if (decl == nullptr) return;
                    if (llvm::isa<FunctionDecl>(decl))
                    {
                        VisitFunctionDecl(llvm::cast<FunctionDecl>(decl));
                        return;
                    }
                    if (auto* ud = llvm::dyn_cast<UsingDecl>(decl))
                    {
                        VisitUsingDecl(ud);
                        return;
                    }
                    if (auto* dc = llvm::dyn_cast<DeclContext>(decl))
                        for (Decl* child : dc->decls()) walk(child);
                };
                walk(root);
            }

            bool VisitTypedefNameDecl(TypedefNameDecl* td)
            {
                if (!td->getIdentifier()) return true;
                if (td->getName() == kHeaderScopeSentinel) return true;
                if (auto* alias = llvm::dyn_cast<TypeAliasDecl>(td);
                    alias != nullptr && alias->getDescribedAliasTemplate() != nullptr)
                    return true;
                std::string name = td->getNameAsString();
                QualType u = td->getUnderlyingType();
                RawTypedef t;
                t.name = name;
                t.qualifiedName = st.req.cxxMode ? CxxQualifiedName(td) : name;
                LocOfRaw(td, t.file, t.line, t.col);
                if (st.req.requireInScope && !st.InScope(t.file)) return true;
                if (const RecordType* rt = u->getAs<RecordType>())
                {
                    const RecordDecl* rd = rt->getDecl();
                    t.isAnonymousRecord = rd->isStruct() && !rd->getIdentifier()
                                       && rd->getTypedefNameForAnonDecl() == td;
                }
                FillTypedefUnderlying(u, t);
                st.out.typedefs.push_back(std::move(t));
                QueueFunctionPointerAbi(st, ctx, u);
                return true;
            }

            void FillTypedefUnderlying(QualType u, RawTypedef& t)
            {
                const std::string& name = t.name;
                std::string sugared = u.getAsString(ctx.getPrintingPolicy());
                std::string canon = u.getCanonicalType().getAsString(ctx.getPrintingPolicy());
                // Prefer the canonical underlying (chases HANDLE -> void *); but for the
                // `typedef enum {} X;` self-referential shape clang canonicalizes to the typedef
                // name itself ("ML_Mode"), so fall back to the sugared spelling ("enum ML_Mode")
                // which the mapper strips to int. Mirrors the old CollectCTypedefsLibclang.
                if (!canon.empty() && canon != name) t.underlying = canon;
                else if (!sugared.empty())           t.underlying = sugared;
                else                                  t.underlying = canon;
                if (st.req.cxxMode && u->getAs<TemplateSpecializationType>() != nullptr)
                {
                    t.cxxSpecialization = canon.empty() ? sugared : canon;
                    for (const char* prefix : { "class ", "struct " })
                        if (t.cxxSpecialization.rfind(prefix, 0) == 0)
                            t.cxxSpecialization.erase(0, std::strlen(prefix));
                }
            }

            /*
             * Record an alias template's parameter DEFAULTS and its target STRUCTURALLY: the
             * target's dotted qualified name plus each pattern argument printed on its own. The
             * flat `cxxAliasPattern` string cannot be re-split safely (a nested
             * `AWrap<NBox<T, 9>>` carries commas inside its own angle brackets) and, printed with
             * the default policy, names a target in the alias's own namespace or at global scope
             * without any qualifier at all.
             */
            void HarvestAliasTemplatePattern(ASTContext& ctx, TypeAliasTemplateDecl* atd,
                                             const TypeAliasDecl* alias, RawTypedef& t)
            {
                PrintingPolicy pp = ctx.getPrintingPolicy();
                pp.FullyQualifiedName = true;
                pp.SuppressTagKeyword = true;
                // The fully-qualified policy also qualifies a reference to the alias's OWN
                // parameter ("alna.N"); strip that back, since the pattern binds by param name.
                auto normalize = [&t](std::string text) {
                    for (size_t p = 0; (p = text.find("::", p)) != std::string::npos; p += 1)
                        text.replace(p, 2, ".");
                    for (const std::string& param : t.cxxAliasParams)
                    {
                        const std::string suffix = "." + param;
                        size_t at = 0;
                        while ((at = text.find(suffix, at)) != std::string::npos)
                        {
                            const size_t end = at + suffix.size();
                            const bool wholeWord = end == text.size()
                                || (std::isalnum((unsigned char)text[end]) == 0 && text[end] != '_');
                            size_t start = at;
                            while (start > 0 && (std::isalnum((unsigned char)text[start - 1]) != 0
                                                 || text[start - 1] == '_' || text[start - 1] == '.'))
                                --start;
                            if (wholeWord)
                            {
                                text.erase(start, end - start - param.size());
                                at = start + param.size();
                            }
                            else at = end;
                        }
                    }
                    return text;
                };
                auto printArgument = [&](const TemplateArgument& arg) {
                    std::string text;
                    llvm::raw_string_ostream os(text);
                    arg.print(pp, os, /*IncludeType=*/false);
                    os.flush();
                    return normalize(std::move(text));
                };
                // An alias parameter's own DEFAULT is the alias's, not the target's: dropping it
                // would silently fall back to whatever default the target declares.
                for (const NamedDecl* param : *atd->getTemplateParameters())
                {
                    const TemplateArgumentLoc* fallback = nullptr;
                    if (const auto* typeParam = llvm::dyn_cast<TemplateTypeParmDecl>(param))
                    {
                        if (typeParam->hasDefaultArgument())
                            fallback = &typeParam->getDefaultArgument();
                    }
                    else if (const auto* valueParam =
                                 llvm::dyn_cast<NonTypeTemplateParmDecl>(param))
                    {
                        if (valueParam->hasDefaultArgument())
                            fallback = &valueParam->getDefaultArgument();
                    }
                    t.cxxAliasParamDefaults.push_back(
                        fallback != nullptr ? printArgument(fallback->getArgument())
                                            : std::string{});
                }
                const auto* tst = alias->getUnderlyingType()->getAs<TemplateSpecializationType>();
                if (tst == nullptr) return;
                if (const TemplateDecl* target = tst->getTemplateName().getAsTemplateDecl())
                    t.cxxAliasTargetBase = CxxQualifiedName(target);
                if (t.cxxAliasTargetBase.empty()) return;
                for (const TemplateArgument& arg : tst->template_arguments())
                    t.cxxAliasArgs.push_back(printArgument(arg));
            }

            bool VisitTypeAliasTemplateDecl(TypeAliasTemplateDecl* atd)
            {
                if (!st.req.cxxMode) return true;
                auto* alias = atd != nullptr ? atd->getTemplatedDecl() : nullptr;
                if (alias == nullptr || !alias->getIdentifier()) return true;
                RawTypedef t;
                t.name = alias->getNameAsString();
                t.qualifiedName = CxxQualifiedName(alias);
                LocOfRaw(atd, t.file, t.line, t.col);
                if (st.req.requireInScope && !st.InScope(t.file)) return true;
                if (atd->getDeclContext()->isTranslationUnit() && !t.qualifiedName.empty()
                    && st.emittedClassTemplateNames.insert(t.qualifiedName).second)
                    st.out.classTemplateNames.push_back(t.qualifiedName);
                t.isCxxAliasTemplate = true;
                t.cxxAliasPattern = alias->getUnderlyingType().getAsString(ctx.getPrintingPolicy());
                t.underlying = t.cxxAliasPattern;
                // EVERY parameter, type and non-type alike, and in declaration order: the pattern
                // binds use-site arguments positionally, so a skipped `int N` shifts the rest.
                for (const NamedDecl* param : *atd->getTemplateParameters())
                    t.cxxAliasParams.push_back(param->getNameAsString());
                HarvestAliasTemplatePattern(ctx, atd, alias, t);
                st.out.typedefs.push_back(std::move(t));
                return true;
            }

            /*
             * Harvest a C++ namespace-scope (or global-scope) object. Three outcomes:
             *   - a const/constexpr object whose initializer folds to an integer or a float is
             *     bound as a compile-time constant, with no symbol at all;
             *   - an externally-linkable object is bound to Clang's MANGLED name, which is the
             *     only spelling that finds `nsv::counter` in the library. A header-only inline or
             *     constexpr definition has no library symbol, so its storage is emitted into the
             *     companion module first;
             *   - an internal-linkage const object of class type has no symbol anywhere, so its
             *     storage is emitted locally and promoted to weak_odr after codegen.
             * Returns true (continue traversal) unconditionally.
             */
            bool HarvestCxxNamespaceVar(VarDecl* vd)
            {
                if (vd->getIdentifier() == nullptr) return true;
                if (vd->getType()->isFunctionType()) return true;   // not a data symbol

                std::string file; int line = 1, col = 0;
                if (!LocOf(vd, file, line, col)) return true;
                const std::string qualified = CxxQualifiedName(vd);
                auto skipVar = [&](const char* why) {
                    if (st.req.verbose)
                        std::cout << "[verbose]   C++ namespace variable " << qualified
                                  << " not bound: " << why << "\n";
                };
                if (!IsValidDottedName(qualified))
                { skipVar("name is not a valid CFlat qualified name"); return true; }
                // A variable template's pattern (or a specialization of it) is no object of its
                // own name; `ns.v<T>` binds the specialization on use (decltype request).
                if (vd->getDescribedVarTemplate() != nullptr
                    || llvm::isa<VarTemplateSpecializationDecl>(vd))
                { skipVar("it is a variable template, bound per specialization on use"); return true; }
                // A thread_local object is reached through a TLS access sequence, not a plain
                // load of its symbol, so binding it would link wrong or not at all.
                if (vd->getTSCSpec() != TSCS_unspecified)
                { skipVar("it is thread_local, which CFlat cannot address yet"); return true; }
                if (!st.emittedGlobals.insert(qualified).second) return true;

                const VarDecl* def = vd->getDefinition();
                const Expr* init = def != nullptr ? def->getInit() : vd->getAnyInitializer();
                const bool isConst = vd->getType().isConstQualified() || vd->isConstexpr();

                RawGlobalVar g;
                g.name = vd->getNameAsString();
                g.qualifiedName = qualified;
                g.ctype = CanonicalSpelling(ctx, vd->getType().getUnqualifiedType());
                g.isConst = isConst;
                g.file = file; g.line = line; g.col = col;
                // One object per TU (per import group), folded or not.
                g.isInternalLinkage = !vd->hasExternalFormalLinkage()
                    || vd->getStorageClass() == SC_Static;

                // A folded scalar needs no storage, so it works whatever the linkage is.
                if (isConst && init != nullptr && !init->containsErrors() && !init->isValueDependent()
                    && IsFoldableConstantInit(vd, init))
                {
                    Expr::EvalResult result;
                    const bool evaluated = init->EvaluateAsRValue(result, ctx);
                    if (evaluated && result.Val.isInt())
                    {
                        g.isCompileTimeConstant = true;
                        g.constantValue = ApsIntToLongLong(result.Val.getInt());
                        g.isCxxConstexpr = true;
                        st.out.globals.push_back(std::move(g));
                        return true;
                    }
                    if (evaluated && result.Val.isFloat())
                    {
                        g.isCompileTimeConstant = true;
                        g.isFloatConstant = true;
                        bool losesInfo = false;
                        g.floatValue = ApFloatToDouble(result.Val.getFloat(), &losesInfo);
                        if (losesInfo && st.req.verbose)
                            std::cout << "[verbose]   C++ namespace variable " << qualified
                                      << " rounded long double to double (loss of precision)\n";
                        g.isCxxConstexpr = true;
                        st.out.globals.push_back(std::move(g));
                        return true;
                    }
                }

                // Anything else needs a real symbol. Decide who provides it.
                // `extern int g; inline int g = 3;` marks only the definition inline.
                const bool headerOnly = vd->isInline() || (def != nullptr && def->isInline())
                    || vd->isConstexpr()
                    || !vd->hasExternalFormalLinkage() || vd->getStorageClass() == SC_Static;
                const bool canEmit = st.req.RecordsDefinitionDemand()
                                  || st.req.assumeInlineDefinitions;
                if (headerOnly)
                {
                    if (def == nullptr || init == nullptr)
                    { skipVar("no definition to emit and no library symbol to bind"); return true; }
                    if (!canEmit)
                    { skipVar("its initializer lives in the header, so it has no library symbol"); return true; }
                    if (def->getType()->isDependentType()
                        || def->getDeclContext()->isDependentContext())
                    { skipVar("its type or initializer is dependent"); return true; }
                    if (st.req.RecordsDefinitionDemand()) st.varEmitWork.push_back(def);
                    const std::string originalLinkage = CxxLinkageName(ctx, vd);
                    const bool internalStorage = !vd->hasExternalFormalLinkage()
                        || vd->getStorageClass() == SC_Static;
                    // Each import group owns its internal storage, constants included: two
                    // groups' headers may give the same static different values.
                    g.linkageName = internalStorage
                        ? StaticCxxGlobalAlias(st.req.cxxImportGroupKey, originalLinkage)
                        : originalLinkage;
                    if (internalStorage && st.req.demandPlan != nullptr)
                        st.req.demandPlan->renamed[g.linkageName] = originalLinkage;
                    // Value identity of a const object: two groups' copies of one declaration
                    // read alike only when the evaluated initializers match (defines may differ).
                    // An address names a per-group entity even when it prints alike: no hash.
                    if (internalStorage && isConst)
                        if (const APValue* value = const_cast<VarDecl*>(def)->evaluateValue();
                            value != nullptr && !ApValueHoldsAddress(*value))
                        {
                            uint64_t key = 14695981039346656037ULL;
                            for (unsigned char c : value->getAsString(ctx, def->getType()))
                                key = (key ^ c) * 1099511628211ULL;
                            g.constInitHash = key == 0 ? 1 : key;
                        }
                    // An internal-linkage definition is invisible outside the companion module.
                    if (internalStorage)
                    {
                        st.out.weakPromoteSymbols.push_back(originalLinkage);
                        st.out.weakPromotePerGroupSymbols.push_back(originalLinkage);
                    }
                }
                else
                {
                    // .c auto-extern mode never reaches here (that path is C-only), but a C++
                    // definitions-only request still binds only what this TU defines.
                    if (st.req.definitionsOnly && def == nullptr) return true;
                    g.linkageName = CxxLinkageName(ctx, vd);
                }
                st.out.globals.push_back(std::move(g));
                return true;
            }

            // Harvest an externally-linkable file-scope global variable - a header `extern int x;`
            // declaration or a .c-defined `int x = 5;`. Skips statics (internal linkage), locals,
            // and (in definitionsOnly / .c mode) pure declarations with no definition in this TU.
            // Dedups redeclarations by name. Returns true (continue traversal) unconditionally.
            bool HarvestGlobalVar(VarDecl* vd)
            {
                if (!vd->isFileVarDecl()) return true;            // locals, params, instance members
                if (vd->isStaticDataMember()) return true;       // reached through its class
                if (st.req.cxxMode) return HarvestCxxNamespaceVar(vd);

                if (vd->getStorageClass() == SC_Static) return true;  // internal linkage
                if (!vd->hasExternalFormalLinkage()) return true;
                if (vd->getType()->isFunctionType()) return true; // not a data symbol
                // .c auto-extern mode: only globals this TU actually defines.
                if (st.req.definitionsOnly && !vd->isThisDeclarationADefinition()) return true;

                std::string file; int line = 1, col = 0;
                if (!LocOf(vd, file, line, col)) return true;

                std::string name = vd->getNameAsString();
                if (!st.emittedGlobals.insert(name).second) return true;  // already emitted

                RawGlobalVar g;
                g.name = name;
                g.ctype = CanonicalSpelling(ctx, vd->getType().getUnqualifiedType());
                g.file = file; g.line = line; g.col = col;
                st.out.globals.push_back(std::move(g));
                return true;
            }

            // Read a macro probe variable (`__cflat_macro_<i> = (MACRO)`): the deduced type and
            // constant-folded initializer give the macro's natural type and value.
            bool VisitVarDecl(VarDecl* vd)
            {
                const IdentifierInfo* ii = vd->getIdentifier();
                if (!ii) return true;
                StringRef nm = ii->getName();
                if (nm == kHeaderScopeSentinel) return true;
                if (!nm.starts_with(kProbePrefix)) return HarvestGlobalVar(vd);
                unsigned idx = 0;
                // `__cflat_macro_<i>` or `__cflat_macro_<i>_<tag>`: a lazy single-macro probe is
                // tagged so a later probe chunk in the same incremental TU never redeclares it.
                StringRef slot = nm.drop_front(sizeof(kProbePrefix) - 1).split('_').first;
                if (slot.getAsInteger(10, idx)) return true;
                if (idx >= st.probes.size()) return true;
                const CxxMacroProbe& mp = st.probes[idx];
                RawMacro m;
                m.name = mp.name; m.file = mp.file; m.line = mp.line; m.col = mp.col;
                m.aliasTarget = mp.aliasTarget;
                m.kind = RawMacro::Skip;

                // No usable initializer: the probe did not parse (a type name, an unknown
                // identifier). An alias-shaped body is still worth reporting to the binder.
                const Expr* init = vd->getInit();
                if (!init)
                {
                    if (!m.aliasTarget.empty())
                    {
                        st.emittedProbes.insert(idx);
                        st.out.macros.push_back(std::move(m));
                    }
                    return true;
                }

                // EvaluateAsRValue must not be called on an error-recovery or value-dependent
                // initializer (it can assert on the inconsistent node). Probes for non-value
                // macros are built unparenthesized so they fail to parse and leave a null init
                // (handled above), but skip defensively here for any that still produce one.
                if (init->containsErrors() || init->isValueDependent())
                {
                    st.emittedProbes.insert(idx);
                    st.out.macros.push_back(std::move(m));
                    return true;
                }

                if (auto* sl = llvm::dyn_cast<StringLiteral>(init->IgnoreParenImpCasts()))
                {
                    if (sl->isOrdinary())
                    {
                        m.kind = RawMacro::String;
                        m.stringValue = sl->getString().str();
                        m.naturalType = "char *";
                    }
                    st.emittedProbes.insert(idx);
                    st.out.macros.push_back(std::move(m));
                    return true;
                }

                Expr::EvalResult ev;
                if (init->EvaluateAsRValue(ev, ctx))
                {
                    // Strip the probe's top-level `const` (from `static const __auto_type`) so a
                    // function-pointer macro reads "int (*)(int, int)", not "int (*const)(...)".
                    m.naturalType = CanonicalSpelling(ctx, vd->getType().getUnqualifiedType());
                    const APValue& v = ev.Val;
                    if (v.isInt())        { m.kind = RawMacro::Int; m.intValue = ApsIntToLongLong(v.getInt()); }
                    else if (v.isFloat())
                    {
                        // convertToDouble() asserts unless the APFloat already has IEEEdouble
                        // semantics; a long double macro (e.g. math.h constants) carries wider
                        // (80-bit/quad) semantics. Narrow to double first, then read it out.
                        m.kind = RawMacro::Float;
                        llvm::APFloat f = v.getFloat();
                        bool losesInfo = false;
                        f.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &losesInfo);
                        m.floatValue = f.convertToDouble();
                    }
                    else if (v.isLValue() && v.getLValueBase().isNull())
                                          { m.kind = RawMacro::Int;   m.intValue = v.getLValueOffset().getQuantity(); }
                }
                st.emittedProbes.insert(idx);
                st.out.macros.push_back(std::move(m));
                return true;
            }
        };

        std::string LlvmTypeText(llvm::Type* t) { return cflat_llvm::StructuralTypeText(t); }

        int AbiKindOf(const clang::CodeGen::ABIArgInfo& ai)
        {
            using K = clang::CodeGen::ABIArgInfo;
            switch (ai.getKind())
            {
            case K::Direct:          return RawAbiSlot::Direct;
            case K::Extend:          return RawAbiSlot::Extend;
            case K::Indirect:        return RawAbiSlot::Indirect;
            case K::IndirectAliased: return RawAbiSlot::IndirectAliased;
            case K::Ignore:          return RawAbiSlot::Ignore;
            case K::Expand:          return RawAbiSlot::Expand;
            case K::CoerceAndExpand: return RawAbiSlot::CoerceAndExpand;
            case K::InAlloca:        return RawAbiSlot::InAlloca;
            case K::TargetSpecific:  return RawAbiSlot::TargetSpecific;
            }
            return RawAbiSlot::Unknown;
        }

        // Copy one ABIArgInfo into the clang-free slot description. The LLVM argument count
        // follows clang's own ClangToLLVMArgMapping: Ignore consumes none, a Direct with a
        // flattenable struct coerce type consumes one argument per element, everything else one.
        RawAbiSlot DescribeAbiSlot(const clang::CodeGen::ABIArgInfo& ai)
        {
            RawAbiSlot s;
            s.kind = AbiKindOf(ai);
            if (ai.isDirect() || ai.isExtend() || ai.isIndirect() || ai.isTargetSpecific())
                s.inReg = ai.getInReg();
            if (ai.isDirect() || ai.isExtend())
            {
                s.coerceType  = LlvmTypeText(ai.getCoerceToType());
                s.paddingType = LlvmTypeText(ai.getPaddingType());
                s.directOffset = (uint64_t)ai.getDirectOffset();
                s.canBeFlattened = ai.isDirect() ? ai.getCanBeFlattened() : false;
                if (ai.isExtend())
                {
                    s.signExt = ai.isSignExt();
                    s.zeroExt = ai.isZeroExt();
                }
            }
            else if (ai.isCoerceAndExpand())
                s.coerceType = LlvmTypeText(ai.getCoerceToType());
            else if (ai.isIndirect() || ai.isIndirectAliased())
            {
                s.indirectAlign   = (uint64_t)ai.getIndirectAlign().getQuantity();
                if (ai.isIndirect())
                {
                    s.indirectByVal   = ai.getIndirectByVal();
                    s.indirectRealign = ai.getIndirectRealign();
                    s.sretAfterThis   = ai.isSRetAfterThis();
                }
            }

            if (s.kind == RawAbiSlot::Ignore)
                s.llvmArgCount = 0;
            else if (s.kind == RawAbiSlot::Direct && s.canBeFlattened)
            {
                auto* sty = llvm::dyn_cast_or_null<llvm::StructType>(ai.getCoerceToType());
                if (sty != nullptr) s.llvmArgCount = sty->getNumElements();
            }
            return s;
        }

        // A by-value parameter or return whose record was only NAMED (an uninstantiated
        // specialization) has no layout; clang CodeGen cannot arrange such a prototype.
        static bool ProtoHasIncompleteRecord(const FunctionProtoType* fpt)
        {
            auto incomplete = [](QualType t) {
                t = t.getCanonicalType();
                return t->isRecordType() && t->isIncompleteType();
            };
            if (incomplete(fpt->getReturnType())) return true;
            for (QualType p : fpt->getParamTypes())
                if (incomplete(p)) return true;
            return false;
        }

        // A record clang marked invalid (instantiated over an incomplete CFlat record), named by
        // value or through pointers / references: CodeGen's layout query asserts on it.
        static bool ProtoHasInvalidRecord(const FunctionProtoType* fpt)
        {
            auto invalid = [](QualType t) {
                t = t.getCanonicalType();
                while (t->isPointerType() || t->isReferenceType())
                    t = t->getPointeeType().getCanonicalType();
                const CXXRecordDecl* rd = t->getAsCXXRecordDecl();
                return rd != nullptr && rd->isInvalidDecl();
            };
            if (invalid(fpt->getReturnType())) return true;
            for (QualType p : fpt->getParamTypes())
                if (invalid(p)) return true;
            return false;
        }

        // A return type that is still an undeduced `auto` (the MSVC STL writes
        // `auto insert(node_type&&)`): an explicit class instantiation never instantiates that
        // body, so there is no return type to arrange and CodeGen dereferences the placeholder.
        static bool ProtoHasUndeducedReturn(const FunctionProtoType* fpt)
        {
            return fpt->getReturnType()->isUndeducedType();
        }

        /*
         * Under the Microsoft ABI a pointer-to-member type has no representation until Sema has
         * locked in its class's inheritance model, which a real translation unit does the first
         * time such a type must be complete (a definition, a call, a sizeof). A declaration-only
         * header never gets there, and CodeGen then dereferences the missing MSInheritanceAttr
         * while converting the prototype. Lock the model in the way RequireCompleteType does;
         * a prototype whose model still cannot be settled is left unarranged (the backend refuses
         * every member-pointer parameter or return by its spelling anyway).
         */
        static bool ProtoHasUnmodeledMemberPointer(ExtractState& st, ASTContext& ctx,
                                                   const FunctionProtoType* fpt)
        {
            if (!ctx.getTargetInfo().getCXXABI().isMicrosoft()) return false;
            auto unmodeled = [&](QualType t) {
                const auto* mpt = llvm::dyn_cast<MemberPointerType>(t.getCanonicalType());
                if (mpt == nullptr) return false;
                CXXRecordDecl* rd = mpt->getMostRecentCXXRecordDecl();
                if (rd == nullptr || rd->isDependentType()) return true;
                if (st.ci != nullptr && st.ci->hasSema())
                    (void)st.ci->getSema().isCompleteType(rd->getLocation(), t);
                return !rd->getMostRecentDecl()->hasAttr<MSInheritanceAttr>();
            };
            if (unmodeled(fpt->getReturnType())) return true;
            for (QualType p : fpt->getParamTypes())
                if (unmodeled(p)) return true;
            return false;
        }

        RawAbi DescribeAbi(const clang::CodeGen::CGFunctionInfo& fi)
        {
            RawAbi abi;
            abi.valid = true;
            abi.callingConv = fi.getEffectiveCallingConvention();
            abi.ret = DescribeAbiSlot(fi.getReturnInfo());
            unsigned next = (abi.ret.kind == RawAbiSlot::Indirect
                          || abi.ret.kind == RawAbiSlot::IndirectAliased) ? 1u : 0u;
            for (const auto& a : fi.arguments())
            {
                RawAbiSlot s = DescribeAbiSlot(a.info);
                s.llvmArgIndex = next;
                next += s.llvmArgCount;
                abi.params.push_back(std::move(s));
            }
            return abi;
        }

        /*
         * cxxMode: ask Clang for the calling convention of every collected C++ free function and
         * serialize it onto the RawSig. The CodeGenerator (and its LLVMContext / module) is
         * transient - everything the backend needs is plain data by the time this returns, which
         * is what lets the recipe survive the on-disk signature cache.
         */
        void ComputeCxxMemberAbi(ExtractState& st, ASTContext& ctx,
                                 TranslationUnitDecl* root,
                                 clang::CodeGen::CodeGenModule& cgm, CodeGenerator& cg);
        void EmitCxxDefinitions(ExtractState& st, ASTContext& ctx,
                                TranslationUnitDecl* root, CodeGenerator& cg);

        /*
         * Plain header records are not template requests, so no later instantiation pass asks
         * Sema to declare their lazy special members. Declare the non-trivial ones before the
         * record's method list is exported; definition and ODR-use happen after parsing below.
         */
        /*
         * A class whose definition clang rejected (a header that names std::string without
         * including <string>, say) has no members cflat could bind: Sema marks the whole
         * definition invalid and CodeGen never arranges its methods. Refusing the record with the
         * diagnostic that broke it says so at the use site, instead of "no constructor takes N
         * arguments". The first error located inside the definition is the one quoted.
         */
        std::string DeclVisitor::InvalidDefinitionRefusal(const CXXRecordDecl* def) const
        {
            const auto* diags = st.ci != nullptr
                ? dynamic_cast<const PrereqDiagConsumer*>(st.ci->getDiagnostics().getClient())
                : nullptr;
            std::string file; int first = 0, last = 0, col = 0;
            const bool located = diags != nullptr && LocOfRaw(def, file, first, col);
            if (located)
            {
                PresumedLoc endLoc = sm.getPresumedLoc(def->getEndLoc());
                last = endLoc.isValid() && endLoc.getFilename() == file ? (int)endLoc.getLine() : first;
                for (const auto& e : diags->errors)
                    if (e.file == file && (int)e.line >= first && (int)e.line <= last)
                        return std::format("does not compile as C++: {} ({}:{})",
                                           e.message, e.file, e.line);
            }
            return "does not compile as C++ (clang reported an error inside its definition)";
        }

        std::string DeclVisitor::InvalidTypeRequestRefusal(const CXXRecordDecl* def,
                                                            const std::string& spelling) const
        {
            const auto* diags = st.ci != nullptr
                ? dynamic_cast<const PrereqDiagConsumer*>(st.ci->getDiagnostics().getClient())
                : nullptr;
            std::string detail;
            std::string file; int first = 0, last = 0, col = 0;
            if (def != nullptr && LocOfRaw(def, file, first, col))
            {
                PresumedLoc endLoc = sm.getPresumedLoc(def->getEndLoc());
                last = endLoc.isValid() && endLoc.getFilename() == file
                    ? (int)endLoc.getLine() : first;
                if (diags != nullptr)
                    for (const auto& e : diags->errors)
                        if (e.file == file && (int)e.line >= first && (int)e.line <= last)
                        {
                            detail = e.message;
                            break;
                        }
            }
            if (detail.empty() && diags != nullptr) detail = diags->firstError;
            if (detail.empty()) detail = "clang reported an invalid specialization";
            return std::format("C++ type '{}' could not be instantiated: {}", spelling, detail);
        }

        void DeclVisitor::PrepareHeaderSpecialMembers(const CXXRecordDecl* cxx, bool completing)
        {
            const CXXRecordDecl* def = CompleteNonDependentCxxRecord(cxx);
            if ((!st.req.RecordsDefinitionDemand() && !st.req.assumeInlineDefinitions)
                || (!st.req.requireInScope && !completing)
                || def == nullptr
                || !st.headerSpecialMemberSeen.insert(def).second)
                return;
            if (!st.ci->hasSema()) return;
            Sema& sema = st.ci->getSema();
            CXXRecordDecl* rd = const_cast<CXXRecordDecl*>(def);

            if (rd->needsImplicitDestructor() && rd->hasNonTrivialDestructor())
            {
                CXXDestructorDecl* dtor = sema.LookupDestructor(rd);
                if (dtor == nullptr) dtor = sema.DeclareImplicitDestructor(rd);
            }
            if (rd->needsImplicitCopyConstructor() && rd->hasNonTrivialCopyConstructor())
            {
                CXXConstructorDecl* ctor = sema.LookupCopyingConstructor(rd, Qualifiers::Const);
                if (ctor == nullptr) ctor = sema.DeclareImplicitCopyConstructor(rd);
            }
            if (rd->needsImplicitMoveConstructor() && rd->hasNonTrivialMoveConstructor())
            {
                CXXConstructorDecl* ctor = sema.LookupMovingConstructor(rd, 0);
                if (ctor == nullptr) ctor = sema.DeclareImplicitMoveConstructor(rd);
            }
            if (rd->needsImplicitCopyAssignment() && rd->hasNonTrivialCopyAssignment())
            {
                CXXMethodDecl* op = sema.LookupCopyingAssignment(rd, Qualifiers::Const, false, 0);
                if (op == nullptr) op = sema.DeclareImplicitCopyAssignment(rd);
            }
            if (rd->needsImplicitMoveAssignment() && rd->hasNonTrivialMoveAssignment())
            {
                CXXMethodDecl* op = sema.LookupMovingAssignment(rd, 0, false, 0);
                if (op == nullptr) op = sema.DeclareImplicitMoveAssignment(rd);
            }
            st.headerSpecialMemberWork.push_back(def);
        }

        void DefineHeaderImplicitSpecialMembers(ExtractState& st)
        {
            if (!st.req.emitDefinitions || !st.ci->hasSema()
                || st.headerSpecialMemberWork.empty())
                return;
            Sema& sema = st.ci->getSema();
            clang::Scope tuScope(nullptr, clang::Scope::DeclScope, st.ci->getDiagnostics());
            const bool lendScope = sema.TUScope == nullptr;
            if (lendScope) sema.TUScope = &tuScope;
            struct ScopeReset
            {
                Sema& sema; bool active;
                ~ScopeReset() { if (active) sema.TUScope = nullptr; }
            } scopeReset{sema, lendScope};

            for (const CXXRecordDecl* queued : st.headerSpecialMemberWork)
            {
                const CXXRecordDecl* rd = CompleteNonDependentCxxRecord(queued);
                if (rd == nullptr) continue;
                CXXRecordDecl* mutableRd = const_cast<CXXRecordDecl*>(rd);
                auto mark = [&](CXXMethodDecl* md) {
                    if (md == nullptr || !md->isImplicit() || md->hasBody() || md->isDeleted()
                        || md->isInvalidDecl()
                        || md->getType()->isDependentType()
                        || CompleteNonDependentCxxRecord(md->getParent()) == nullptr)
                        return;
                    sema.MarkFunctionReferenced(md->getLocation(), md,
                                                /*MightBeOdrUse*/ true);
                };

                if (mutableRd->hasNonTrivialDestructor())
                {
                    CXXDestructorDecl* dtor = sema.LookupDestructor(mutableRd);
                    if (dtor != nullptr && dtor->isImplicit() && !dtor->hasBody() && !dtor->isDeleted()
                        && !dtor->isInvalidDecl())
                        sema.DefineImplicitDestructor(dtor->getLocation(), dtor);
                    mark(dtor);
                }
                if (mutableRd->hasNonTrivialCopyConstructor())
                {
                    CXXConstructorDecl* ctor =
                        sema.LookupCopyingConstructor(mutableRd, Qualifiers::Const);
                    if (ctor != nullptr && ctor->isImplicit() && !ctor->hasBody() && !ctor->isDeleted()
                        && !ctor->isInvalidDecl())
                        sema.DefineImplicitCopyConstructor(ctor->getLocation(), ctor);
                    mark(ctor);
                }
                if (mutableRd->hasNonTrivialMoveConstructor())
                {
                    CXXConstructorDecl* ctor = sema.LookupMovingConstructor(mutableRd, 0);
                    if (ctor != nullptr && ctor->isImplicit() && !ctor->hasBody() && !ctor->isDeleted()
                        && !ctor->isInvalidDecl())
                        sema.DefineImplicitMoveConstructor(ctor->getLocation(), ctor);
                    mark(ctor);
                }
                if (mutableRd->hasNonTrivialCopyAssignment())
                {
                    CXXMethodDecl* op =
                        sema.LookupCopyingAssignment(mutableRd, Qualifiers::Const, false, 0);
                    if (op != nullptr && op->isImplicit() && !op->hasBody() && !op->isDeleted()
                        && !op->isInvalidDecl())
                        sema.DefineImplicitCopyAssignment(op->getLocation(), op);
                    mark(op);
                }
                if (mutableRd->hasNonTrivialMoveAssignment())
                {
                    CXXMethodDecl* op =
                        sema.LookupMovingAssignment(mutableRd, 0, false, 0);
                    if (op != nullptr && op->isImplicit() && !op->hasBody() && !op->isDeleted()
                        && !op->isInvalidDecl())
                        sema.DefineImplicitMoveAssignment(op->getLocation(), op);
                    mark(op);
                }
            }
            sema.PerformPendingInstantiations();
        }

        /*
         * A DEFAULTED special member (`~parser() = default`, an implicit copy constructor) has no
         * body until something odr-uses it. Sema is still alive here (ParseAST runs the consumer
         * before tearing it down), so odr-use each one now: Sema synthesizes the body at once,
         * with the transitive members it needs. This MUST run before anything asks CodeGen for
         * the member's address (the ABI loop below does, for every member): CodeGen decides
         * whether to queue a body the first time it creates the symbol, and a bodiless
         * declaration created then is returned as is by every later request. A member Sema
         * cannot define (deleted, ill-formed) simply stays bodiless and is refused at the use site.
         */
        void DefineDefaultedSpecialMembers(ExtractState& st)
        {
            if (!st.ci->hasSema()) return;
            Sema& sema = st.ci->getSema();
            // Parsing is over, so the parser's translation-unit scope is gone. Defining a
            // defaulted copy assignment looks up __builtin_memcpy through Sema::TUScope
            // (LookupBuiltin pushes the lazily created builtin onto it); give it a scope.
            clang::Scope tuScope(nullptr, clang::Scope::DeclScope, st.ci->getDiagnostics());
            const bool lendScope = sema.TUScope == nullptr;
            if (lendScope) sema.TUScope = &tuScope;
            struct ScopeReset
            {
                Sema& sema; bool active;
                ~ScopeReset() { if (active) sema.TUScope = nullptr; }
            } scopeReset{sema, lendScope};
            for (const auto& w : st.memberAbiWork)
            {
                const CXXMethodDecl* md = w.md;
                if (md == nullptr || md->hasBody() || !md->isDefaulted() || md->isDeleted()
                    || md->isInvalidDecl() || md->getType()->isDependentType()
                    || CompleteNonDependentCxxRecord(md->getParent()) == nullptr)
                    continue;
                sema.MarkFunctionReferenced(md->getLocation(), const_cast<CXXMethodDecl*>(md),
                                            /*MightBeOdrUse*/ true);
            }
            sema.PerformPendingInstantiations();
        }

        /*
         * Microsoft ABI only. A constructor stores the vfptr, so emitting one references the
         * vftable, and MSVC-compatible CodeGen emits that vftable linkonce_odr in every TU - even
         * for an explicit instantiation DECLARATION (`extern template struct X<int>;`), where
         * Itanium would leave the vtable to the library. The vftable then pulls in the deleting
         * destructor and every virtual member, but Sema never instantiated those inline bodies
         * (the explicit instantiation declaration suppresses it and nothing in the header used
         * the vtable), and CodeGen crashes generating a structor with no body. Let Sema do what
         * it does for a real TU that uses the vtable: mark it used and define its members.
         */
        void DefineMicrosoftVTableMembers(ExtractState& st, ASTContext& ctx)
        {
            if (!st.ci->hasSema() || !ctx.getTargetInfo().getCXXABI().isMicrosoft()) return;
            Sema& sema = st.ci->getSema();
            std::unordered_set<const CXXRecordDecl*> seen;
            bool marked = false;
            auto markVTable = [&](const CXXRecordDecl* queued) {
                const CXXRecordDecl* rd = CompleteNonDependentCxxRecord(queued);
                if (rd == nullptr || !rd->isDynamicClass() || rd->isInvalidDecl()
                    || !seen.insert(rd).second)
                    return;
                sema.MarkVTableUsed(rd->getLocation(), const_cast<CXXRecordDecl*>(rd),
                                    /*DefinitionRequired*/ true);
                marked = true;
            };
            for (const CXXRecordDecl* rd : st.headerSpecialMemberWork) markVTable(rd);
            for (const auto& w : st.memberAbiWork)
                if (w.md != nullptr) markVTable(w.md->getParent());
            if (!marked) return;
            sema.DefineUsedVTables();
            sema.PerformPendingInstantiations();
        }

        /*
         * Itanium counterpart of the above, for exactly the vtables the companion emits itself
         * (vtableWork minus a key-function or `extern template` anchor - the same filter as the
         * HandleVTable loop). A blind HandleVTable references every virtual member, but Sema only
         * instantiates a class template's virtual bodies once the vtable is USED, so an inline
         * virtual (libc++'s hidden basic_stringbuf<char>::seekpos when the dylib does not export
         * the specialization) stayed a declaration and the final link could not resolve it.
         */
        void DefineEmittedVTableMembers(ExtractState& st, ASTContext& ctx)
        {
            if (!st.ci->hasSema() || !st.req.RecordsDefinitionDemand()
                || ctx.getTargetInfo().getCXXABI().isMicrosoft())
                return;
            Sema& sema = st.ci->getSema();
            bool marked = false;
            for (const CXXRecordDecl* rd : st.vtableWork)
            {
                const CXXRecordDecl* def = CompleteNonDependentCxxRecord(rd);
                if (def == nullptr || !def->isDynamicClass() || def->isInvalidDecl()
                    || ctx.getCurrentKeyFunction(def) != nullptr
                    || def->getTemplateSpecializationKind()
                        == clang::TSK_ExplicitInstantiationDeclaration)
                    continue;
                sema.MarkVTableUsed(def->getLocation(), const_cast<CXXRecordDecl*>(def),
                                    /*DefinitionRequired*/ true);
                marked = true;
            }
            if (!marked) return;
            sema.DefineUsedVTables();
            sema.PerformPendingInstantiations();
        }

        /*
         * An error clang raised inside a header the caller asked to bind poisons everything
         * downstream: Sema marks the offending declarations invalid, every instantiation that
         * touches them comes out holding error expressions, and companion CodeGen would then walk
         * an AST clang's own driver would never have handed it (that walk is what crashes on an
         * STL-using header). Record the first such diagnostic so the bind can be refused with it.
         * Errors in the in-memory stub - the intentional macro probes and default-argument
         * wrapper requests - and in system headers - an ill-formed STL instantiation, which the
         * error-body sweep already contains - are not this case and stay tolerated.
         */
        void RecordHeaderScopeError(ExtractState& st, ASTContext& ctx)
        {
            if (!st.req.checkHeaderScope || st.ci == nullptr || st.out.headerErrors > 0) return;
            // A balanced stub declares the sentinel at file scope, where this lookup finds it.
            // Only a sentinel swallowed by a header scope needs the full AST walk.
            for (const NamedDecl* found :
                 ctx.getTranslationUnitDecl()->lookup(&ctx.Idents.get(kHeaderScopeSentinel)))
                if (st.ci->getSourceManager().isInMainFile(found->getLocation()))
                {
                    st.scopeSentinel = found;
                    break;
                }
            if (st.scopeSentinel == nullptr)
            {
                HeaderScopeSentinelVisitor visitor(st);
                visitor.TraverseDecl(ctx.getTranslationUnitDecl());
            }

            const SourceManager& sm = st.ci->getSourceManager();
            const DeclContext* scope = st.scopeSentinel == nullptr
                ? nullptr : st.scopeSentinel->getDeclContext();
            bool balanced = scope != nullptr && scope->isTranslationUnit();
            if (scope != nullptr && llvm::isa<LinkageSpecDecl>(scope))
            {
                const Decl* linkage = Decl::castFromDeclContext(scope);
                balanced = scope->getParent()->isTranslationUnit()
                        && sm.isInMainFile(linkage->getLocation());
            }
            if (balanced) return;

            std::string scopeName = "an unknown scope";
            std::string scopePath = st.req.scopeHeaderPath;
            if (scope != nullptr)
            {
                if (const auto* ns = llvm::dyn_cast<NamespaceDecl>(scope))
                    scopeName = ns->getQualifiedNameAsString();
                else if (const auto* record = llvm::dyn_cast<RecordDecl>(scope))
                    scopeName = record->getQualifiedNameAsString();
                else if (llvm::isa<LinkageSpecDecl>(scope))
                    scopeName = "extern \"C\"";
                else
                    scopeName = scope->getDeclKindName();
                if (const auto* decl = llvm::dyn_cast<Decl>(scope))
                {
                    PresumedLoc pl = sm.getPresumedLoc(decl->getLocation());
                    if (pl.isValid()) scopePath = pl.getFilename();
                }
            }
            st.out.headerErrors = 1;
            st.out.firstHeaderError = std::format(
                "header leaves a namespace or brace scope open: {}{}", scopeName,
                scopePath.empty() ? std::string() : std::format(" at {}", scopePath));
        }

        void RecordInScopeHeaderErrors(ExtractState& st)
        {
            st.out.headerErrors = 0;
            st.out.firstHeaderError.clear();
            if (!st.req.cxxMode || !st.req.requireInScope || st.normDirs.empty()
                || st.ci == nullptr)
                return;
            const auto* diags =
                dynamic_cast<const PrereqDiagConsumer*>(st.ci->getDiagnostics().getClient());
            if (diags == nullptr) return;
            for (const auto& e : diags->errors)
            {
                // The stub is remapped onto a path inside the scope dirs for the default-wrapper
                // parse; a wrapper whose forwarded call fails is dropped by the error-body sweep,
                // not a reason to refuse the header.
                if (e.inMainFile) continue;
                if (e.file.empty() || !st.InScope(e.file)) continue;
                ++st.out.headerErrors;
                if (st.out.firstHeaderError.empty())
                    st.out.firstHeaderError =
                        std::format("{} at {}:{}", e.message, e.file, e.line);
            }
        }

        /*
         * Names every complete, non-dependent record of the TU the way the header harvest does,
         * so a completion request in a group that never ran the harvest still finds its record.
         */
        void IndexSpecialMemberRecords(ASTContext& ctx, const DeclContext* dc,
            std::unordered_map<std::string, const CXXRecordDecl*>& index)
        {
            auto add = [&](const CXXRecordDecl* rd) {
                const CXXRecordDecl* def = CompleteNonDependentCxxRecord(rd);
                if (def == nullptr || def->getIdentifier() == nullptr
                    || def->isInAnonymousNamespace())
                    return;
                const auto* spec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(def);
                // The harvest names implicit specializations and records nested in them by
                // foreign identity (typedef, base and field paths); index that name as well.
                if (const std::string identity = CxxForeignIdentity(
                        CanonicalSpelling(ctx, ctx.getCanonicalTagType(def)));
                    !identity.empty())
                    index.emplace(identity, def);
                const std::string name = spec != nullptr
                        && spec->getSpecializationKind() == TSK_ExplicitSpecialization
                    ? CxxForeignIdentity(CanonicalSpelling(ctx, ctx.getCanonicalTagType(def)))
                    : CxxQualifiedName(def);
                if (IsValidDottedName(name)) index.emplace(name, def);
                IndexSpecialMemberRecords(ctx, def, index);
            };
            for (const Decl* d : dc->decls())
            {
                if (const auto* ns = llvm::dyn_cast<NamespaceDecl>(d))
                    IndexSpecialMemberRecords(ctx, ns, index);
                else if (const auto* ls = llvm::dyn_cast<LinkageSpecDecl>(d))
                    IndexSpecialMemberRecords(ctx, ls, index);
                else if (const auto* ex = llvm::dyn_cast<ExportDecl>(d))
                    IndexSpecialMemberRecords(ctx, ex, index);
                else if (const auto* ct = llvm::dyn_cast<ClassTemplateDecl>(d))
                {
                    for (const auto* spec : ct->specializations()) add(spec);
                }
                else if (const auto* rd = llvm::dyn_cast<CXXRecordDecl>(d);
                         rd != nullptr && !llvm::isa<ClassTemplateSpecializationDecl>(rd))
                    add(rd);
            }
        }

        /*
         * R1: a live group's header harvest (chunk 0) defines no implicit / defaulted special
         * member and no inline-vtable virtual. Those definitions are the transitive instantiation
         * of every field and base special member of every harvested record, most of which the
         * program never constructs; a record that needs them is completed by a later request
         * (CompleteRequestedSpecialMembers) when the program first projects it. A one-shot TU
         * (a .cpp import, LSP) has no later request to complete in, so it stays eager.
         */
        bool DeferHeaderSpecialMembers(const ExtractState& st)
        {
            return st.req.cxxMode && st.req.requireInScope && st.req.RecordsDefinitionDemand()
                && st.req.demandPlan != nullptr && !st.req.completeCxxSpecialMembers;
        }

        /*
         * The user-declared special members of an implicit class-template specialization whose
         * bodies are not instantiated yet. The eager passes instantiated these through every
         * record holding the specialization by value or as a base (a container's implicit copy
         * constructor calls the element's), so a completion instantiates them for the record.
         */
        std::vector<CXXMethodDecl*> UninstantiatedSpecialMembers(const CXXRecordDecl* def)
        {
            std::vector<CXXMethodDecl*> out;
            if (def->getTemplateSpecializationKind() != clang::TSK_ImplicitInstantiation)
                return out;
            for (CXXMethodDecl* md : def->methods())
            {
                if (md->isImplicit() || md->isDefaulted() || md->hasBody() || md->isDeleted()
                    || md->isInvalidDecl() || md->getType()->isDependentType())
                    continue;
                const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md);
                const bool special = llvm::isa<CXXDestructorDecl>(md)
                    || (ctor != nullptr && (ctor->isDefaultConstructor()
                                            || ctor->isCopyOrMoveConstructor()))
                    || md->isCopyAssignmentOperator() || md->isMoveAssignmentOperator();
                if (!special) continue;
                const FunctionDecl* pattern = md->getTemplateInstantiationPattern();
                if (pattern == nullptr || !pattern->isDefined()) continue;
                out.push_back(md);
            }
            return out;
        }

        // True when one of the four definition passes would define something of this record:
        // a defaulted (implicit or `= default`) member without a body that Sema would define on
        // odr-use, the virtual members of a vtable the companion emits, or an uninstantiated
        // user-declared special member of a class-template specialization.
        bool HasDeferredSpecialMemberWork(ASTContext& ctx, const CXXRecordDecl* cxx)
        {
            const CXXRecordDecl* def = CompleteNonDependentCxxRecord(cxx);
            if (def == nullptr || def->isInvalidDecl()) return false;
            if (!UninstantiatedSpecialMembers(def).empty()) return true;
            for (const CXXMethodDecl* md : def->methods())
            {
                if (!md->isDefaulted() || md->hasBody() || md->isDeleted() || md->isInvalidDecl()
                    || md->getType()->isDependentType())
                    continue;
                // Sema never defines a trivial default constructor or destructor.
                const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md);
                if (md->isTrivial() && ((ctor != nullptr && ctor->isDefaultConstructor())
                                        || llvm::isa<CXXDestructorDecl>(md)))
                    continue;
                return true;
            }
            if (!def->isDynamicClass()) return false;
            if (ctx.getTargetInfo().getCXXABI().isMicrosoft()) return true;
            return def->isPolymorphic() && def->getNumVBases() == 0
                && ctx.getCurrentKeyFunction(def) == nullptr
                && def->getTemplateSpecializationKind()
                    != clang::TSK_ExplicitInstantiationDeclaration;
        }

        /*
         * Completion request: run the four definition passes for one header record exactly as
         * the header harvest would have run them over it, before its members are harvested. The
         * work lists are swapped in so only this record is defined here.
         */
        void CompleteRequestedSpecialMembers(ExtractState& st, ASTContext& ctx,
                                             const CXXRecordDecl* record)
        {
            const CXXRecordDecl* rd = CompleteNonDependentCxxRecord(record);
            if (st.ci == nullptr || !st.ci->hasSema() || rd == nullptr) return;
            llvm::TimeTraceScope scope("CxxCompleteSpecialMembers");
            auto savedHeaderWork = std::move(st.headerSpecialMemberWork);
            auto savedMembers = std::move(st.memberAbiWork);
            auto savedVtables = std::move(st.vtableWork);
            st.headerSpecialMemberWork.clear();
            st.memberAbiWork.clear();
            st.vtableWork.clear();
            DeclVisitor visitor(ctx, st);
            visitor.PrepareHeaderSpecialMembers(rd, true);
            if (st.headerSpecialMemberWork.empty()) st.headerSpecialMemberWork.push_back(rd);
            for (const CXXMethodDecl* md : rd->methods())
                st.memberAbiWork.push_back({SIZE_MAX, SIZE_MAX, md});
            if (rd->isPolymorphic() && rd->getNumVBases() == 0) st.vtableWork.push_back(rd);
            if (const auto instantiate = UninstantiatedSpecialMembers(rd); !instantiate.empty())
            {
                Sema& sema = st.ci->getSema();
                clang::Scope tuScope(nullptr, clang::Scope::DeclScope, st.ci->getDiagnostics());
                const bool lendScope = sema.TUScope == nullptr;
                if (lendScope) sema.TUScope = &tuScope;
                for (CXXMethodDecl* md : instantiate)
                    sema.MarkFunctionReferenced(md->getLocation(), md, /*MightBeOdrUse*/ true);
                sema.PerformPendingInstantiations();
                if (lendScope) sema.TUScope = nullptr;
            }
            DefineHeaderImplicitSpecialMembers(st);
            DefineDefaultedSpecialMembers(st);
            DefineMicrosoftVTableMembers(st, ctx);
            DefineEmittedVTableMembers(st, ctx);
            st.headerSpecialMemberWork = std::move(savedHeaderWork);
            st.memberAbiWork = std::move(savedMembers);
            st.vtableWork = std::move(savedVtables);
        }

        /*
         * A completion reports only what the deferred passes changed: the special members and
         * virtuals, plus any other member whose body those passes instantiated. A member that was
         * already defined (by the harvest or a later request) keeps the harvest's facts, as the
         * eager passes would have left them.
         */
        void StripPredefinedCompletionMembers(ExtractState& st)
        {
            using Member = RawCxxMember;
            std::map<size_t, std::vector<size_t>> strip;
            for (const auto& w : st.memberAbiWork)
            {
                if (w.md == nullptr || st.completionPredefined.count(w.md) == 0
                    || w.recordIdx >= st.out.records.size()
                    || w.memberIdx >= st.out.records[w.recordIdx].members.size())
                    continue;
                const Member& m = st.out.records[w.recordIdx].members[w.memberIdx];
                if (m.kind == Member::Destructor || m.isDefaultCtor || m.isCopyCtor
                    || m.isMoveCtor || m.isCopyAssign || m.isMoveAssign || m.isVirtual)
                    continue;
                strip[w.recordIdx].push_back(w.memberIdx);
            }
            for (auto& [recordIdx, members] : strip)
            {
                std::sort(members.begin(), members.end());
                members.erase(std::unique(members.begin(), members.end()), members.end());
                auto& list = st.out.records[recordIdx].members;
                for (auto it = members.rbegin(); it != members.rend(); ++it)
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(*it));
            }
        }

        void ComputeCxxAbi(ExtractState& st, ASTContext& ctx, TranslationUnitDecl* root)
        {
            if (st.ci == nullptr) return;
            bool deferred = DeferHeaderSpecialMembers(st);
            if (deferred)
            {
                /*
                 * Small-harvest cutoff (maintainer ruling 2026-09-29). A completion chunk has a
                 * fixed cost the eager passes do not. With few records to complete (nlohmann/json
                 * 13, simdjson 37, fmt 24; torch 2331) the passes are cheaper than the chunks the
                 * program would ask for: deferring measured +1.1% cold instructions on simdjson and
                 * +0.5% on json. Eager also suits the cache better: every record's facts land in
                 * the header entry, so a sibling program using a record the first one never touched
                 * needs no completion chunk. Deferral pays off only on large harvests like torch.
                 * The value is a tuning constant; a later perf review may make it adjustable.
                 */
                constexpr size_t kMinDeferredRecords = 64;
                size_t pending = 0;
                for (const RawRecord& rec : st.out.records) pending += rec.specialMembersPending;
                if (pending < kMinDeferredRecords)
                {
                    deferred = false;
                    for (RawRecord& rec : st.out.records) rec.specialMembersPending = false;
                }
            }
            if (!deferred)
            {
                DefineHeaderImplicitSpecialMembers(st);
                DefineDefaultedSpecialMembers(st);
                DefineMicrosoftVTableMembers(st, ctx);
                DefineEmittedVTableMembers(st, ctx);
            }
            if (st.abiWork.empty() && st.functionPointerAbiWork.empty()
                && st.memberAbiWork.empty() && !st.req.RecordsDefinitionDemand()) return;
            using namespace clang::CodeGen;

            llvm::LLVMContext llvmCtx;
            std::unique_ptr<CodeGenerator> cg(
                clang::CreateLLVMCodeGen(*st.ci, "cflat_cxx_abi", llvmCtx));
            if (!cg) return;
            cg->Initialize(ctx);
            CodeGenModule& cgm = cg->CGM();

            for (const auto& [idx, fd] : st.abiWork)
            {
                if (idx >= st.out.sigs.size()) continue;
                CanQualType canon = fd->getType()->getCanonicalTypeUnqualified();
                if (canon->getAs<FunctionProtoType>() == nullptr) continue;  // K&R / no prototype
                CanQual<FunctionProtoType> fpt = canon.castAs<FunctionProtoType>();
                if (ProtoHasIncompleteRecord(fpt.getTypePtr()))
                {
                    if (st.out.sigs[idx].bindRefusal.empty())
                        st.out.sigs[idx].bindRefusal =
                            "uses a C++ class template specialization that the header never instantiates";
                    continue;
                }
                if (ProtoHasInvalidRecord(fpt.getTypePtr()))
                {
                    if (st.out.sigs[idx].bindRefusal.empty())
                        st.out.sigs[idx].bindRefusal =
                            "uses a C++ class whose instantiation failed";
                    continue;
                }
                if (ProtoHasUnmodeledMemberPointer(st, ctx, fpt.getTypePtr())) continue;
                if (ProtoHasUndeducedReturn(fpt.getTypePtr())) continue;
                const CGFunctionInfo& fi = arrangeFreeFunctionType(cgm, fpt);

                RawAbi abi = DescribeAbi(fi);
                abi.fnTypeText = LlvmTypeText(convertFreeFunctionType(cgm, fd));
                st.out.sigs[idx].abi = std::move(abi);
            }

            for (const QualType& queued : st.functionPointerAbiWork)
            {
                QualType t = queued.getCanonicalType();
                if (t->isPointerType()) t = t->getPointeeType().getCanonicalType();
                const auto* fptPtr = t->getAs<FunctionProtoType>();
                if (fptPtr == nullptr) continue;
                if (t->isDependentType())
                {
                    if (st.req.verbose)
                        std::cout << "[verbose]   skipped dependent function-pointer ABI type '"
                                  << CanonicalSpelling(ctx, t) << "'\n";
                    continue;
                }
                if (ProtoHasIncompleteRecord(fptPtr)) continue;
                if (ProtoHasInvalidRecord(fptPtr)) continue;
                if (ProtoHasUnmodeledMemberPointer(st, ctx, fptPtr)) continue;
                if (ProtoHasUndeducedReturn(fptPtr)) continue;
                CanQual<FunctionProtoType> fpt =
                    CanQual<FunctionProtoType>::CreateUnsafe(t);
                const CGFunctionInfo& fi = arrangeFreeFunctionType(cgm, fpt);
                RawFunctionPointerAbi plan;
                plan.signature = CanonicalSpelling(ctx, t);
                plan.retType = CanonicalSpelling(ctx, fptPtr->getReturnType());
                for (QualType p : fptPtr->getParamTypes())
                    plan.paramTypes.push_back(CanonicalSpelling(ctx, p));
                plan.abi = DescribeAbi(fi);
                st.out.functionPointerAbis.push_back(std::move(plan));
            }

            ComputeCxxMemberAbi(st, ctx, root, cgm, *cg);

            if (st.req.RecordsDefinitionDemand())
            {
                llvm::TimeTraceScope emitScope("CxxDefinitionEmit");
                EmitCxxDefinitions(st, ctx, root, *cg);
            }
            if (st.req.completeCxxSpecialMembers) StripPredefinedCompletionMembers(st);
            (void)cg->ReleaseModule();
        }

        /*
         * Point a virtual member at the extern "C" thunk the request source synthesized for it
         * (LLVMBackend::BuildCxxVirtualThunks). The thunk body is a plain C++ virtual call, so
         * Clang owns the vftable load, the vbtable adjustment and the covariant return
         * adjustment; the member stops being virtual TO CFLAT and binds down the ordinary
         * direct-call path. Leaves the member untouched - and therefore refused - when no thunk
         * was emitted, so nothing here can turn an unbindable member into a wrong call.
         */
        void BindCxxVirtualThunk(ExtractState& st, ASTContext& ctx,
                                 TranslationUnitDecl* root,
                                 clang::CodeGen::CodeGenModule& cgm, RawCxxMember& m,
                                 const std::string& name)
        {
            using namespace clang::CodeGen;
            const FunctionDecl* thunk = nullptr;
            for (NamedDecl* nd : root->lookup(
                     DeclarationName(&ctx.Idents.get(name))))
                if (const auto* fd = llvm::dyn_cast<FunctionDecl>(nd))
                    if (fd->doesThisDeclarationHaveABody()) { thunk = fd; break; }
            if (thunk == nullptr || thunk->isInvalidDecl()) return;
            CanQualType canon = thunk->getType()->getCanonicalTypeUnqualified();
            if (canon->getAs<FunctionProtoType>() == nullptr) return;
            CanQual<FunctionProtoType> fpt = canon.castAs<FunctionProtoType>();
            RawAbi abi = DescribeAbi(arrangeFreeFunctionType(cgm, fpt));
            abi.fnTypeText = LlvmTypeText(convertFreeFunctionType(cgm, thunk));
            if (m.paramTypes.size() != abi.params.size()) return;
            m.abi = std::move(abi);
            m.linkageName = name;
            m.isVirtual = false;
            m.covariantReturnNeedsAdjust = false;
            m.vtableIndex = -1;
            m.vtableIndexDeleting = -1;
            // The thunk constructs or destroys the complete object in place and returns nothing,
            // whatever the structor ABI would have done with a returned 'this'.
            if (m.kind == RawCxxMember::Destructor || m.kind == RawCxxMember::Constructor)
            { m.retType = "void"; m.returnsThis = false; }
            // Prove the thunk is a DEFINITION in the companion module the way every other
            // synthesized helper is proved, instead of trusting that the request source compiled.
            m.needsLocalDefinition = true;
        }

        // Fill in the per-slot arrangement of every exported class member. The slot info comes
        // from arrangeCXXMethodType (which prepends 'this') for instance methods and structors,
        // and from arrangeFreeFunctionType for static ones. The FUNCTION TYPE is always taken
        // from Clang's own GetAddrOfGlobal declaration - that is the only source that knows a
        // structor returns 'this' on Itanium/Darwin.
        void ComputeCxxMemberAbi(ExtractState& st, ASTContext& ctx,
                                 TranslationUnitDecl* root,
                                 clang::CodeGen::CodeGenModule& cgm, CodeGenerator& cg)
        {
            using namespace clang::CodeGen;
            for (const auto& w : st.memberAbiWork)
            {
                if (w.recordIdx >= st.out.records.size()) continue;
                RawRecord& rec = st.out.records[w.recordIdx];
                if (w.memberIdx >= rec.members.size()) continue;
                RawCxxMember& m = rec.members[w.memberIdx];
                const CXXMethodDecl* md = w.md;
                if (md == nullptr || md->isInvalidDecl() || md->getType()->isDependentType()
                    || md->isVariadic() || CompleteNonDependentCxxRecord(md->getParent()) == nullptr)
                    continue;

                /*
                 * M6 - the vtable slot of a virtual member, straight from Clang's vtable layout.
                 * The index is relative to the address point of the vtable of the class that
                 * DECLARES the member, which is exactly the subobject the call site will have
                 * adjusted `this` to. Itanium: a virtual destructor gets both of its slots, D1
                 * (complete object) and D0 (deleting, which also releases the storage).
                 * Microsoft: the vftable holds ONE destructor slot, the deleting destructor
                 * (`this`, flags; bit 0 = release the storage), so both indices name it. A slot in a vfptr that is not at offset zero of the declaring class, or one
                 * reached through a virtual base, is left unknown and the member is refused.
                 */
                if (md->isVirtual())
                {
                    const auto* dd = llvm::dyn_cast<CXXDestructorDecl>(md);
                    if (auto* itanium = llvm::dyn_cast<clang::ItaniumVTableContext>(
                            ctx.getVTableContext()))
                    {
                        if (dd != nullptr)
                        {
                            m.vtableIndex = (int)itanium->getMethodVTableIndex(
                                GlobalDecl(dd, Dtor_Complete));
                            m.vtableIndexDeleting = (int)itanium->getMethodVTableIndex(
                                GlobalDecl(dd, Dtor_Deleting));
                        }
                        else
                            m.vtableIndex = (int)itanium->getMethodVTableIndex(GlobalDecl(md));
                    }
                    else if (auto* microsoft = llvm::dyn_cast<clang::MicrosoftVTableContext>(
                                 ctx.getVTableContext()))
                    {
                        // The slot is keyed by the deleting variant this target's vftable
                        // references (vector deleting on current targets); asking for the
                        // other variant misses the map, which asserts only in a Debug LLVM.
                        const CXXDtorType deleting =
                            ctx.getTargetInfo().emitVectorDeletingDtors(ctx.getLangOpts())
                                ? Dtor_VectorDeleting : Dtor_Deleting;
                        const clang::MethodVFTableLocation loc =
                            microsoft->getMethodVFTableLocation(
                                dd != nullptr ? GlobalDecl(dd, deleting) : GlobalDecl(md));
                        // Mirror MicrosoftCXXABI::adjustThisArgumentForVirtualFunctionCall.
                        // Virtual bases stay refused because their vfptr offset is not constant.
                        if (loc.VBase == nullptr)
                        {
                            m.vtableIndex = (int)loc.Index;
                            m.vtableOffsetBytes = loc.VFPtrOffset.getQuantity();
                            if (dd != nullptr) m.vtableIndexDeleting = (int)loc.Index;
                        }
                    }
                }

                CanQualType canon = md->getType()->getCanonicalTypeUnqualified();
                if (canon->getAs<FunctionProtoType>() == nullptr) continue;
                CanQual<FunctionProtoType> fpt = canon.castAs<FunctionProtoType>();
                if (ProtoHasIncompleteRecord(fpt.getTypePtr())) continue;
                if (ProtoHasInvalidRecord(fpt.getTypePtr())) continue;
                if (ProtoHasUnmodeledMemberPointer(st, ctx, fpt.getTypePtr())) continue;
                if (ProtoHasUndeducedReturn(fpt.getTypePtr())) continue;

                const CGFunctionInfo* fi = nullptr;
                if (m.kind == RawCxxMember::StaticMethod)
                    fi = &arrangeFreeFunctionType(cgm, fpt);
                else
                    fi = &arrangeCXXMethodType(cgm, md->getParent(), fpt.getTypePtr(), md);
                if (fi == nullptr) continue;

                RawAbi abi = DescribeAbi(*fi);
                // Structors: the arrangement above says the result is Ignore/void, but the
                // emitted declaration returns 'this'. Take the real thing and mark the return
                // as one plain pointer register the caller drops.
                llvm::Constant* addr = cg.GetAddrOfGlobal(MemberGlobalDecl(md), false);
                auto* fn = addr != nullptr ? llvm::dyn_cast<llvm::Function>(addr->stripPointerCasts())
                                           : nullptr;
                if (fn == nullptr) continue;
                abi.fnTypeText = LlvmTypeText(fn->getFunctionType());
                if (m.returnsThis)
                {
                    llvm::Type* rt = fn->getFunctionType()->getReturnType();
                    if (rt->isPointerTy())
                    {
                        abi.ret = RawAbiSlot{};
                        abi.ret.kind = RawAbiSlot::Direct;
                        abi.ret.coerceType = LlvmTypeText(rt);
                    }
                    else
                    {
                        // A target whose structors return void: keep the declaration honest.
                        m.returnsThis = false;
                        m.retType = "void";
                    }
                }
                if (m.paramTypes.size() != abi.params.size())
                    continue;   // arrangement disagrees with the exported signature: refuse it
                m.abi = std::move(abi);

                // Slot unusable and no fallback path to the member, or an implicit most-derived
                // argument cflat cannot pass: hand it to the thunk Clang generated for it.
                if (CxxMemberNeedsVirtualThunk(m))
                    BindCxxVirtualThunk(st, ctx, root, cgm, m,
                                        CxxVirtualThunkName(m.linkageName) + st.req.cxxThunkSuffix);
                else if (CxxCtorNeedsVbaseThunk(rec, m))
                    BindCxxVirtualThunk(st, ctx, root, cgm, m,
                                        CxxVbaseCtorThunkName(m.linkageName) + st.req.cxxThunkSuffix);
            }
        }

        /*
         * M5 - definition emission. Clang's own CodeGenerator is driven over the parsed header
         * exactly as it would be over a .cpp that consists of that header, so the companion
         * module ends up holding whatever a real C++ translation unit would contribute:
         * linkonce_odr inline bodies, vtables and RTTI with their COMDATs, guard variables for
         * static locals, inline static data members and their initializers.
         *
         * Clang DEFERS an inline definition until something references it, which is the whole
         * point here - the pass references only what cflat binds (free functions, methods,
         * structors, inline static members, and the vtable of every polymorphic class), then lets
         * Release() emit those bodies plus everything they reach transitively. A giant header
         * therefore costs a parse, not a full translation.
         *
         * The referencing loop takes plain GlobalDecls, so a later milestone can add template
         * specializations to it without changing anything else.
         */
        void EmitCxxDefinitions(ExtractState& st, ASTContext& ctx,
                                TranslationUnitDecl* root, CodeGenerator& cg)
        {
            /*
             * Plain-header implicit special members are defined and marked while Sema is still
             * alive, before this CodeGen pass. Explicitly defaulted members use the companion
             * path in DefineDefaultedSpecialMembers for the same reason.
             */

            /*
             * The macro-probe stubs raise intentional errors (that is how a macro's type is
             * deduced), and ModuleBuilder throws the module away when the DiagnosticsEngine has
             * seen any error. Clear the tally - the swallowing consumer already made these
             * diagnostics non-fatal for extraction - so real CodeGen errors are the only thing
             * that can still discard the companion module.
             */
            // An error expression can only exist in a body when this parse actually reported an
            // error, so the whole error-containment walk below is skipped for a clean TU.
            const bool sawParseErrors = st.ci->getDiagnostics().getNumErrors() > 0;
            st.ci->getDiagnostics().Reset(/*soft*/ true);

            auto inScopeDecl = [&](const Decl* d) {
                if (!st.req.requireInScope) return true;
                PresumedLoc pl = st.ci->getSourceManager().getPresumedLoc(d->getLocation());
                return pl.isValid() && st.InScope(pl.getFilename());
            };

            auto rememberDroppedWrapper = [&](const FunctionDecl* fd) {
                if (fd == nullptr) return;
                std::string name = fd->getNameAsString();
                if (!name.starts_with("__cflat_dflt_")) return;
                if (name.ends_with("_cpp")) name.resize(name.size() - 4);
                if (std::find(st.out.droppedCxxDefaultWrappers.begin(),
                              st.out.droppedCxxDefaultWrappers.end(), name)
                    == st.out.droppedCxxDefaultWrappers.end())
                    st.out.droppedCxxDefaultWrappers.push_back(std::move(name));
            };
            /*
             * An instantiated body can carry an ERROR EXPRESSION: libc++'s vector(size_type)
             * value-initializes its element, so instantiating it for a class with no default
             * constructor leaves a RecoveryExpr inside the body. CodeGen cannot lower that
             * ("cannot compile this l-value expression yet"), and ModuleBuilder throws the WHOLE
             * companion module away once the diagnostic engine has seen an error - which refuses
             * every member of the requested class, not just the ill-formed one. So a body that
             * REACHES such an expression through its call graph must stay unrequested. The walk
             * is memoized per definition and shared by all the request loops below.
             */
            struct ErrorReachScan
            {
                std::unordered_map<const FunctionDecl*, bool> memo;
                // Off for a translation unit that reported no error at all: no body can hold an
                // error expression then, so the whole walk is skipped.
                bool active = true;
                // A live Interpreter's bodies emptied by an earlier chunk (see ExtractRequest).
                const std::unordered_map<const FunctionDecl*, std::string>* poisoned = nullptr;

                // The reason of the first poisoned body a walk reached, for the refusal text.
                std::string poisonReason;
                // Clang error groups per failed instantiation (ExtractRequest::errorCauses), and
                // per refused definition the diagnostic lines of the body that doomed it.
                const std::unordered_map<const FunctionDecl*, std::string>* causes = nullptr;
                std::unordered_map<const FunctionDecl*, std::string> causeMemo;

                static const FunctionDecl* DefinitionOf(const FunctionDecl* fd)
                {
                    const FunctionDecl* def = nullptr;
                    if (!fd->hasBody(def) || def == nullptr) def = fd;
                    return def;
                }

                static bool InvalidRecord(const CXXRecordDecl* rd)
                {
                    if (rd == nullptr) return false;
                    const CXXRecordDecl* def = rd->getDefinition();
                    return rd->isInvalidDecl() || (def != nullptr && def->isInvalidDecl());
                }

                /*
                 * An expression can be error-free and still name an INVALID record: a class
                 * template instantiated while a template argument was incomplete (the earlier
                 * chunk's `std::map<int, Leaf>` with `Leaf` forward-declared) is marked invalid
                 * and keeps its members' bodies, but CodeGen asserts the moment one of those
                 * bodies asks for its layout. Such a body must stay unrequested like an error body.
                 */
                static bool TouchesInvalidRecord(const Expr* expr)
                {
                    if (expr->containsErrors()) return true;
                    QualType type = expr->getType();
                    if (type.isNull()) return false;
                    if (InvalidRecord(type->getAsCXXRecordDecl())) return true;
                    if (InvalidRecord(type->getPointeeCXXRecordDecl())) return true;
                    if (const auto* member = llvm::dyn_cast<MemberExpr>(expr))
                    {
                        const ValueDecl* vd = member->getMemberDecl();
                        if (vd->isInvalidDecl()) return true;
                        if (InvalidRecord(llvm::dyn_cast<CXXRecordDecl>(vd->getDeclContext())))
                            return true;
                    }
                    return false;
                }

                std::string OwnCause(const FunctionDecl* def) const
                {
                    if (poisoned != nullptr)
                        if (auto it = poisoned->find(def); it != poisoned->end()) return it->second;
                    if (causes != nullptr)
                        if (auto it = causes->find(def); it != causes->end()) return it->second;
                    return std::string();
                }

                std::string CauseOf(const FunctionDecl* fd) const
                {
                    if (fd == nullptr) return std::string();
                    auto it = causeMemo.find(DefinitionOf(fd));
                    return it == causeMemo.end() ? std::string() : it->second;
                }

                bool IsPoisoned(const FunctionDecl* fd)
                {
                    if (poisoned == nullptr || poisoned->empty() || fd == nullptr) return false;
                    const FunctionDecl* def = nullptr;
                    if (!fd->hasBody(def) || def == nullptr) def = fd;
                    auto it = poisoned->find(def);
                    if (it == poisoned->end()) it = poisoned->find(fd);
                    if (it == poisoned->end()) return false;
                    if (poisonReason.empty()) poisonReason = it->second;
                    return true;
                }

                // The body of THIS function contains an error expression (as opposed to
                // reaching one through a call).
                bool HasOwnError(const FunctionDecl* fd)
                {
                    if (fd == nullptr) return false;
                    if (fd->isInvalidDecl() || IsPoisoned(fd)) return true;
                    struct OwnErrorVisitor : RecursiveASTVisitor<OwnErrorVisitor>
                    {
                        bool found = false;
                        bool VisitExpr(Expr* expr)
                        {
                            found = found || TouchesInvalidRecord(expr);
                            return !found;
                        }
                    } visitor;
                    if (fd->getBody() != nullptr) visitor.TraverseStmt(fd->getBody());
                    return visitor.found;
                }

                bool Reaches(const FunctionDecl* fd, unsigned depth = 0)
                {
                    if (fd == nullptr || !active) return false;
                    // Past the depth cap the answer is a conservative REFUSAL, not "clean": a
                    // body that reaches an error expression through a longer chain would
                    // otherwise be emitted and take the whole companion module down with it.
                    if (depth > 64) return true;
                    const FunctionDecl* def = nullptr;
                    if (!fd->hasBody(def) || def == nullptr) def = fd;
                    auto it = memo.find(def);
                    if (it != memo.end()) return it->second;
                    memo[def] = false;   // cycles: a body being walked counts as clean

                    struct BodyVisitor : RecursiveASTVisitor<BodyVisitor>
                    {
                        bool found = false;
                        std::vector<const FunctionDecl*> callees;

                        bool VisitExpr(Expr* expr)
                        {
                            found = found || TouchesInvalidRecord(expr);
                            return !found;
                        }
                        bool VisitCallExpr(CallExpr* call)
                        {
                            callees.push_back(call->getDirectCallee());
                            return true;
                        }
                        bool VisitCXXConstructExpr(CXXConstructExpr* ctor)
                        {
                            callees.push_back(ctor->getConstructor());
                            return true;
                        }
                        bool VisitDeclRefExpr(DeclRefExpr* ref)
                        {
                            callees.push_back(llvm::dyn_cast<FunctionDecl>(ref->getDecl()));
                            return true;
                        }
                    } visitor;
                    if (def->isInvalidDecl() || IsPoisoned(def))
                    {
                        memo[def] = true;
                        causeMemo[def] = OwnCause(def);
                        return true;
                    }
                    if (def->getBody() != nullptr) visitor.TraverseStmt(def->getBody());
                    bool bad = visitor.found;
                    if (bad) causeMemo[def] = OwnCause(def);
                    for (const FunctionDecl* callee : visitor.callees)
                    {
                        if (bad) break;
                        if (callee == nullptr || callee == def) continue;
                        bad = Reaches(callee, depth + 1);
                        if (bad) causeMemo[def] = CauseOf(callee);
                    }
                    memo[def] = bad;
                    return bad;
                }
            };
            auto errorReach = std::make_shared<ErrorReachScan>();
            errorReach->poisoned = st.req.poisonedFunctions;
            errorReach->causes = st.req.errorCauses;
            errorReach->active = sawParseErrors
                || (st.req.poisonedFunctions != nullptr && !st.req.poisonedFunctions->empty());
            auto declHasErrors = [errorReach](const Decl* d) {
                if (d == nullptr || d->isInvalidDecl()) return true;
                if (const auto* fd = llvm::dyn_cast<FunctionDecl>(d))
                    return errorReach->active ? errorReach->Reaches(fd)
                                              : errorReach->HasOwnError(fd);
                if (const auto* vd = llvm::dyn_cast<VarDecl>(d))
                {
                    if (vd->getInit() != nullptr && vd->getInit()->containsErrors()) return true;
                    // Lowering a member pointer needs its class's MS inheritance model, and an
                    // incremental rollback can leave that class an invalid instantiation.
                    if (const auto* mpt = vd->getType()->getAs<MemberPointerType>())
                    {
                        const CXXRecordDecl* cls = mpt->getMostRecentCXXRecordDecl();
                        return cls == nullptr || cls->isInvalidDecl() || !cls->hasDefinition()
                            || cls->getDefinition()->isInvalidDecl();
                    }
                    return false;
                }
                return false;
            };
            // A requested template wrapper whose body is not emitted must not bind: its call
            // would link against nothing. Say which instantiation failed instead.
            auto refuseDroppedRequestWrapper = [&](const FunctionDecl* fd) {
                if (fd == nullptr || st.req.cxxFunctionWrapperNames.empty()) return;
                const std::string name = fd->getNameAsString();
                if (std::find(st.req.cxxFunctionWrapperNames.begin(),
                              st.req.cxxFunctionWrapperNames.end(), name)
                    == st.req.cxxFunctionWrapperNames.end())
                    return;
                const std::string reason = errorReach->poisonReason.substr(
                    0, errorReach->poisonReason.find('\n'));
                for (auto& sig : st.out.sigs)
                    if ((sig.name == name || sig.linkageName == name) && sig.bindRefusal.empty())
                    {
                        sig.bindRefusal = reason.empty()
                            ? std::string("clang reported an error inside the body it generated")
                            : reason;
                        sig.refusalCause = errorReach->CauseOf(fd);
                        if (sig.refusalCause.empty()) sig.refusalCause = errorReach->poisonReason;
                    }
            };
            auto isDependentCodeGenDecl = [](const Decl* d) {
                if (d == nullptr || d->getDeclContext()->isDependentContext()) return true;
                if (d->isTemplated()) return true;
                const auto* value = llvm::dyn_cast<ValueDecl>(d);
                return value != nullptr && value->getType()->isDependentType();
            };
            // A live group records CodeGen work for its one demand pass instead of emitting now.
            CxxDemandPlan* const plan = st.req.demandPlan;
            if (plan != nullptr) plan->cxxImportGroupKey = st.req.cxxImportGroupKey;
            auto hand = [&](Decl* d) {
                if (plan != nullptr) plan->Add(d);
                else cg.HandleTopLevelDecl(DeclGroupRef(d));
            };
            auto emitDecl = [&](Decl* d, auto&& emitDeclRef) -> void {
                if (d == nullptr || !inScopeDecl(d)) return;
                if (const auto* linkage = llvm::dyn_cast<LinkageSpecDecl>(d))
                {
                    for (Decl* member : linkage->decls()) emitDeclRef(member, emitDeclRef);
                    return;
                }
                // Per member, so the error checks below see every declaration CodeGen would emit.
                if (const auto* ns = llvm::dyn_cast<NamespaceDecl>(d))
                {
                    for (Decl* member : ns->decls()) emitDeclRef(member, emitDeclRef);
                    return;
                }
                if (declHasErrors(d))
                {
                    rememberDroppedWrapper(llvm::dyn_cast<FunctionDecl>(d));
                    refuseDroppedRequestWrapper(llvm::dyn_cast<FunctionDecl>(d));
                    return;
                }
                // The extern "C" half of a default wrapper only forwards to the C++ half. If that
                // half was dropped (its forwarded call did not compile), emitting the forwarder
                // leaves a call to a symbol nothing defines, and the final link fails on it.
                if (const auto* fd = llvm::dyn_cast<FunctionDecl>(d))
                {
                    std::string name = fd->getNameAsString();
                    if (name.starts_with("__cflat_dflt_"))
                    {
                        if (name.ends_with("_cpp")) name.resize(name.size() - 4);
                        if (std::find(st.out.droppedCxxDefaultWrappers.begin(),
                                      st.out.droppedCxxDefaultWrappers.end(), name)
                            != st.out.droppedCxxDefaultWrappers.end())
                            return;
                    }
                }
                if (st.req.emitDefinitions && isDependentCodeGenDecl(d))
                {
                    if (st.req.verbose)
                    {
                        const auto* named = llvm::dyn_cast<NamedDecl>(d);
                        std::cout << "[verbose]   skipped dependent C++ CodeGen declaration "
                                  << (named != nullptr ? named->getQualifiedNameAsString() : "<unnamed>")
                                  << "\n";
                    }
                    return;
                }
                hand(d);
            };

            /*
             * Phase 0. An instantiated body that came out with an error expression in it (libc++
             * value-initializing an element type that has no default constructor, say) cannot be
             * lowered: CodeGen raises "cannot compile this l-value expression yet" and
             * ModuleBuilder then throws the WHOLE companion module away, which refuses every
             * member of the requested class instead of the ill-formed one. Skipping the request
             * is not enough - Clang emits a deferred body whenever another emitted body
             * references it. So give those bodies an EMPTY one: the module survives, and no
             * correct copy of such a specialization can exist anywhere to be displaced by ODR
             * merging, since the instantiation is ill-formed in every translation unit. Every
             * member that REACHES one is refused below, so cflat never calls into the empty body.
             */
            struct ErrorBodySweep : RecursiveASTVisitor<ErrorBodySweep>
            {
                ErrorReachScan& scan;
                const Stmt* skippedBody = nullptr;
                std::vector<FunctionDecl*> direct;

                explicit ErrorBodySweep(ErrorReachScan& s) : scan(s) {}
                bool shouldVisitTemplateInstantiations() const { return true; }
                // VisitFunctionDecl scans its own body through ErrorReachScan when needed.
                bool shouldWalkTypesOfTypeLocs() const { return false; }
                bool TraverseFunctionDecl(FunctionDecl* fd)
                {
                    if (fd == nullptr || fd->getBody() == nullptr)
                        return RecursiveASTVisitor<ErrorBodySweep>::TraverseFunctionDecl(fd);
                    const Stmt* previous = skippedBody;
                    skippedBody = fd->getBody();
                    const bool result = RecursiveASTVisitor<ErrorBodySweep>::TraverseFunctionDecl(fd);
                    skippedBody = previous;
                    return result;
                }
                bool TraverseStmt(Stmt* stmt,
                                  RecursiveASTVisitor<ErrorBodySweep>::DataRecursionQueue* queue = nullptr)
                {
                    if (stmt == skippedBody) return true;
                    return RecursiveASTVisitor<ErrorBodySweep>::TraverseStmt(stmt, queue);
                }
                bool VisitFunctionDecl(FunctionDecl* fd)
                {
                    if (fd == nullptr || !fd->doesThisDeclarationHaveABody()) return true;
                    // Memoize the whole call graph BEFORE any body is emptied, so a later query
                    // cannot mistake an emptied body for a clean one.
                    if (!scan.IsPoisoned(fd) && scan.Reaches(fd) && scan.HasOwnError(fd))
                        direct.push_back(fd);
                    return true;
                }
            } errorBodies(*errorReach);
            {
                llvm::TimeTraceScope scope("CxxErrorBodySweep");
                if (errorReach->active && !st.req.demandOnlyDefinitions)
                {
                    errorBodies.TraverseDecl(root);
                    for (Decl* d : st.announcedDecls) errorBodies.TraverseDecl(d);
                    for (FunctionDecl* fd : errorBodies.direct)
                    {
                        if (st.req.verbose)
                            std::cout << "[verbose]   C++ body emptied, its instantiation reported an "
                                         "error: " << fd->getQualifiedNameAsString() << "\n";
                        fd->setBody(CompoundStmt::CreateEmpty(ctx, /*NumStmts*/ 0, /*HasFPFeatures*/ false));
                        // A live Interpreter keeps this specialization; a later chunk must not see it clean.
                        if (st.req.poisonedFunctions != nullptr)
                            st.req.poisonedFunctions->emplace(fd, "clang reported an error inside the "
                                "body it generated for '" + fd->getQualifiedNameAsString() + "'");
                    }
                }
            }

            // Phase 1: show Clang the whole translation unit. Inline definitions stay deferred.
            for (Decl* d : root->decls())
                emitDecl(d, emitDecl);
            // Plus everything Sema announced that decls() does not contain (see announcedDecls).
            for (Decl* d : st.announcedDecls)
                emitDecl(d, emitDecl);

            // In a live Interpreter, members instantiated by an EARLIER chunk were announced then,
            // not now. Feed the resolved records' body-bearing methods directly as well.
            if (st.contextRoot != nullptr)
            {
                std::unordered_set<const CXXRecordDecl*> records;
                for (const auto& w : st.memberAbiWork)
                    if (w.md != nullptr) records.insert(w.md->getParent()->getDefinition());
                // Skip what the announcement replay above already handed to CodeGen.
                std::unordered_set<const FunctionDecl*> methods;
                for (Decl* d : st.announcedDecls)
                    if (auto* fd = llvm::dyn_cast_or_null<FunctionDecl>(d)) methods.insert(fd);
                for (const CXXRecordDecl* rd : records)
                {
                    if (rd == nullptr) continue;
                    for (const CXXMethodDecl* md : rd->methods())
                    {
                        if (!md->doesThisDeclarationHaveABody() || md->isInvalidDecl()
                            || isDependentCodeGenDecl(md) || !methods.insert(md).second)
                            continue;
                        hand(const_cast<CXXMethodDecl*>(md));
                    }
                }
            }
            /*
             * An inline static data member is not a top-level decl, and unlike a member function
             * there is no lexical fallback that finds it later. Hand each one to CodeGen so an
             * emitted body can resolve it when needed.
             */
            // Initial entries are global or static data variables cflat binds directly. Later
            // entries are inline or constexpr members reached only through emitted C++ bodies.
            const size_t boundStaticVarCount = st.varEmitWork.size();
            // A requested template can instantiate an inline or constexpr static data member
            // transitively (nlohmann::detail::static_const<T>::value is one example). Those
            // specializations are not top-level declarations and are not members of the
            // requested record, but an emitted body can still odr-use them. Register them so
            // CodeGen emits their linkonce_odr storage only when referenced.
            struct UsedStaticVarVisitor : RecursiveASTVisitor<UsedStaticVarVisitor>
            {
                std::vector<const VarDecl*>& work;
                std::unordered_set<const VarDecl*> seen;

                explicit UsedStaticVarVisitor(std::vector<const VarDecl*>& w) : work(w)
                {
                    for (const VarDecl* vd : work) seen.insert(vd);
                }

                bool shouldVisitTemplateInstantiations() const { return true; }
                // A static data member is declared only in a class body, never in a statement or
                // a local class (C++ forbids that), so function bodies and initializers are skipped.
                bool shouldWalkTypesOfTypeLocs() const { return false; }
                bool TraverseStmt(Stmt*, DataRecursionQueue* = nullptr) { return true; }
                bool TraverseFunctionDecl(FunctionDecl*) { return true; }
                bool TraverseCXXMethodDecl(CXXMethodDecl*) { return true; }
                bool TraverseCXXConstructorDecl(CXXConstructorDecl*) { return true; }
                bool TraverseCXXDestructorDecl(CXXDestructorDecl*) { return true; }
                bool TraverseCXXConversionDecl(CXXConversionDecl*) { return true; }
                bool TraverseCXXDeductionGuideDecl(CXXDeductionGuideDecl*) { return true; }

                bool VisitVarDecl(VarDecl* vd)
                {
                    if (!vd->isStaticDataMember() || !vd->isUsed()
                        || (!vd->isConstexpr() && !vd->isInline()))
                        return true;
                    const VarDecl* definition = vd->getDefinition();
                    if (definition == nullptr) definition = vd;
                    if (seen.insert(definition).second) work.push_back(definition);
                    return true;
                }
            } usedStaticVars(st.varEmitWork);
            {
                llvm::TimeTraceScope scope("CxxUsedStaticVars");
                usedStaticVars.TraverseDecl(root);
                for (Decl* d : st.requestDecls) usedStaticVars.TraverseDecl(d);
            }

            auto handOverStaticVar = [&](const VarDecl* vd) {
                if (vd == nullptr || declHasErrors(vd)) return;
                if (st.req.emitDefinitions && isDependentCodeGenDecl(vd))
                {
                    if (st.req.verbose)
                        std::cout << "[verbose]   skipped dependent C++ static variable "
                                  << vd->getQualifiedNameAsString() << "\n";
                    return;
                }
                hand(const_cast<VarDecl*>(vd));
            };
            for (const VarDecl* vd : st.varEmitWork) handOverStaticVar(vd);
            /*
             * A live Interpreter: a static data member an EARLIER chunk used sits in the shared
             * roots with its used-bit set. Hand it to CodeGen so a body emitted here can still
             * reach its storage, but do not request it (Phase 2 below): only what this module
             * references gets emitted.
             */
            // A demand plan already holds what earlier chunks handed over, so the whole-header
            // re-walk (the old per-request floor) is skipped there.
            std::vector<const VarDecl*> sharedVarWork(st.varEmitWork);
            if (plan == nullptr)
            {
                UsedStaticVarVisitor sharedStaticVars(sharedVarWork);
                for (Decl* d : st.sharedDecls) sharedStaticVars.TraverseDecl(d);
                for (size_t i = st.varEmitWork.size(); i < sharedVarWork.size(); ++i)
                    handOverStaticVar(sharedVarWork[i]);
            }

            // Phase 2: reference what cflat binds so the deferred bodies become emission work.
            std::unique_ptr<MangleContext> planMangler(
                plan != nullptr ? ctx.createMangleContext() : nullptr);
            auto request = [&](GlobalDecl gd) {
                if (plan == nullptr)
                {
                    cg.GetAddrOfGlobal(gd, /*isForDefinition*/ false);
                    return;
                }
                plan->bound[DemandSymbolName(*planMangler, gd)] = gd.getAsOpaquePtr();
            };
            // hasBody(), not getDefinition(): a defaulted or deleted member is already "a
            // definition" in the AST, and only a real body is something CodeGen can emit.
            for (const auto& [idx, fd] : st.abiWork)
                if (fd != nullptr && !declHasErrors(fd) && !fd->getType()->isDependentType()
                    && fd->hasBody())
                    request(GlobalDecl(fd));
            for (const auto& w : st.memberAbiWork)
                if (w.md != nullptr && !declHasErrors(w.md)
                    && !isDependentCodeGenDecl(w.md))
                    request(MemberGlobalDecl(w.md));
            for (size_t i = 0; i < boundStaticVarCount; ++i)
            {
                const VarDecl* vd = st.varEmitWork[i];
                if (vd != nullptr && !declHasErrors(vd) && !isDependentCodeGenDecl(vd))
                    request(GlobalDecl(vd));
            }
            // Register concrete free-function helpers that emitted C++ bodies may use. Clang
            // emits each deferred definition only when a Phase 2 root references it.
            std::vector<const FunctionDecl*> usedFunctionWork;
            struct UsedFunctionVisitor : RecursiveASTVisitor<UsedFunctionVisitor>
            {
                std::vector<const FunctionDecl*>& work;
                std::unordered_set<const FunctionDecl*> seen;

                explicit UsedFunctionVisitor(std::vector<const FunctionDecl*>& w) : work(w) {}
                // Track referenced variables whose storage this module provides (inline,
                // static constexpr and instantiated members); strong definitions stay library-owned.
                const ASTContext* astContext = nullptr;
                std::vector<const VarDecl*> vars;
                std::unordered_set<const VarDecl*> varSeen;
                void AddVariable(const ValueDecl* decl)
                {
                    const auto* vd = llvm::dyn_cast_or_null<VarDecl>(decl);
                    if (vd == nullptr || astContext == nullptr || !vd->hasGlobalStorage()
                        || vd->isStaticLocal())
                        return;
                    const VarDecl* definition = vd->getDefinition();
                    if (definition == nullptr || definition->isInvalidDecl()
                        || definition->getType()->isDependentType()
                        || definition->getDeclContext()->isDependentContext()
                        || definition->isTemplated())
                        return;
                    // A strong definition is exported by the bound library; emitting it here
                    // would duplicate the symbol.
                    const GVALinkage linkage = astContext->GetGVALinkageForVariable(definition);
                    if (linkage != GVA_DiscardableODR && linkage != GVA_Internal) return;
                    if (varSeen.insert(definition).second) vars.push_back(definition);
                }
                bool VisitMemberExpr(MemberExpr* member)
                {
                    if (member != nullptr) AddVariable(member->getMemberDecl());
                    return true;
                }
                bool shouldVisitTemplateInstantiations() const { return true; }
                /*
                 * Clang finds a member body defined INSIDE its class on first reference, so only an
                 * out-of-line member (libc++'s `inline ios_base::flags() const`, or an instantiated
                 * out-of-line member of a class template) needs promoting like a free function.
                 * Constructors and destructors stay with their structor-kind requests.
                 */
                static bool SkippedMember(const FunctionDecl* fd)
                {
                    if (!llvm::isa<CXXMethodDecl>(fd)) return false;
                    if (llvm::isa<CXXConstructorDecl>(fd) || llvm::isa<CXXDestructorDecl>(fd))
                        return true;
                    const FunctionDecl* body = nullptr;
                    if (!fd->hasBody(body) || body == nullptr) return true;
                    return llvm::isa<CXXRecordDecl>(body->getLexicalDeclContext());
                }
                // Every callee seen, and every definition whose body a traversal already walked.
                std::vector<const FunctionDecl*> reach;
                std::unordered_set<const FunctionDecl*> reachSeen, walked;
                void AddFunction(const FunctionDecl* fd)
                {
                    if (fd != nullptr && reachSeen.insert(fd).second) reach.push_back(fd);
                    if (fd == nullptr || SkippedMember(fd)) return;
                    const FunctionDecl* definition = fd;
                    if (!definition->hasBody() || definition->getType()->isDependentType())
                    {
                        const FunctionDecl* candidate = fd->getDefinition();
                        if (candidate != nullptr) definition = candidate;
                    }
                    if (!definition->hasBody()) return;
                    const bool ownDefinitionNeeded = fd->getTemplateSpecializationKind()
                            != TSK_Undeclared
                        || definition->hasAttr<AlwaysInlineAttr>()
                        || definition->hasAttr<InternalLinkageAttr>()
                        || definition->isInlined()
                        || definition->getTemplateSpecializationKind() != TSK_Undeclared
                        || definition->getFormalLinkage() == Linkage::Internal;
                    if (!ownDefinitionNeeded || definition->getType()->isDependentType()
                        || definition->getDeclContext()->isDependentContext())
                        return;
                    if (seen.insert(definition).second) work.push_back(definition);
                }
                bool VisitCallExpr(CallExpr* call)
                {
                    if (call != nullptr) AddFunction(call->getDirectCallee());
                    return true;
                }
                bool VisitDeclRefExpr(DeclRefExpr* ref)
                {
                    if (ref == nullptr) return true;
                    AddFunction(llvm::dyn_cast<FunctionDecl>(ref->getDecl()));
                    AddVariable(ref->getDecl());
                    return true;
                }
                bool VisitCXXRewrittenBinaryOperator(CXXRewrittenBinaryOperator* op)
                {
                    if (op == nullptr) return true;
                    // `a != b` is `!(a == b)` and `a < b` is `(a <=> b) < 0`: the call sits
                    // under the negation or the comparison against zero.
                    const Expr* form = op->getSemanticForm()->IgnoreImplicit();
                    if (const auto* un = llvm::dyn_cast<UnaryOperator>(form))
                        form = un->getSubExpr()->IgnoreImplicit();
                    else if (const auto* bin = llvm::dyn_cast<BinaryOperator>(form))
                        form = llvm::isa<CallExpr>(bin->getLHS()->IgnoreImplicit())
                            ? bin->getLHS()->IgnoreImplicit() : bin->getRHS()->IgnoreImplicit();
                    auto* call = llvm::dyn_cast<CallExpr>(const_cast<Expr*>(form));
                    return call == nullptr || VisitCallExpr(call);
                }
                bool VisitCXXConstructExpr(CXXConstructExpr* ce)
                {
                    if (ce != nullptr && ce->getConstructor() != nullptr
                        && reachSeen.insert(ce->getConstructor()).second)
                        reach.push_back(ce->getConstructor());
                    return true;
                }
                bool VisitFunctionDecl(FunctionDecl* fd)
                {
                    if (fd != nullptr && fd->doesThisDeclarationHaveABody()) walked.insert(fd);
                    // Anything whose definition this module would have to provide itself: an
                    // internal-linkage helper (libc++'s _LIBCPP_HIDE_FROM_ABI), an inline body, or
                    // a template instantiation. A plain external non-inline function is NOT
                    // promoted - the bound library exports that strong symbol already.
                    const bool ownDefinitionNeeded = fd != nullptr
                        && (fd->hasAttr<AlwaysInlineAttr>() || fd->hasAttr<InternalLinkageAttr>()
                            || fd->isInlined()
                            || fd->getTemplateSpecializationKind() == TSK_ImplicitInstantiation
                            || fd->getFormalLinkage() == Linkage::Internal);
                    if (fd == nullptr || SkippedMember(fd) || !fd->hasBody()
                        || !fd->isUsed() || !ownDefinitionNeeded
                        || fd->getType()->isDependentType()
                        || fd->getDeclContext()->isDependentContext())
                        return true;
                    const FunctionDecl* definition = fd->getDefinition();
                    if (definition == nullptr) definition = fd;
                    if (!definition->getType()->isDependentType()
                        && !definition->getDeclContext()->isDependentContext()
                        && seen.insert(definition).second)
                        work.push_back(definition);
                    return true;
                }
            } usedFunctions(usedFunctionWork);
            usedFunctions.astContext = &ctx;
            usedFunctions.TraverseDecl(root);
            for (Decl* d : st.requestDecls) usedFunctions.TraverseDecl(d);
            // Members resolved by an earlier chunk are emitted through Phase 2, not announced
            // now; their bodies still seed the reach closure below.
            for (const auto& w : st.memberAbiWork)
            {
                const FunctionDecl* body = nullptr;
                if (w.md != nullptr && w.md->hasBody(body) && body != nullptr
                    && !isDependentCodeGenDecl(w.md))
                    usedFunctions.TraverseDecl(const_cast<FunctionDecl*>(body));
            }
            /*
             * The TU walk never enters a member of an `extern template` specialization, yet its
             * hidden inline members (libc++ basic_string::__grow_by_without_replace) are lowered
             * here and call further helpers. Close over every reached body clang emits locally.
             */
            // Also walk constructor initializers (not in getBody(): libc++ string_view calls
            // std::to_address there) and referenced variables' initializers, to a fixpoint.
            for (size_t i = 0, v = 0;
                 i < usedFunctions.reach.size() || v < usedFunctions.vars.size();)
            {
                if (i >= usedFunctions.reach.size())
                {
                    const VarDecl* var = usedFunctions.vars[v++];
                    if (const Expr* init = var->getInit())
                        usedFunctions.TraverseStmt(const_cast<Expr*>(init));
                    continue;
                }
                const FunctionDecl* body = nullptr;
                if (!usedFunctions.reach[i++]->hasBody(body) || body == nullptr
                    || body->getType()->isDependentType() || body->isDependentContext()
                    || !usedFunctions.walked.insert(body).second)
                    continue;
                const GVALinkage linkage = ctx.GetGVALinkageForFunction(body);
                if (linkage != GVA_DiscardableODR && linkage != GVA_Internal) continue;
                if (const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(body))
                    for (CXXCtorInitializer* init : ctor->inits())
                        usedFunctions.TraverseConstructorInitializer(init);
                usedFunctions.TraverseStmt(body->getBody());
            }
            // A prototype or class over an invalid record has no layout to arrange.
            auto overInvalidRecord = [](const FunctionDecl* fd) {
                const auto* method = llvm::dyn_cast<CXXMethodDecl>(fd);
                const auto* proto = fd->getType()->getAs<FunctionProtoType>();
                return (method != nullptr && method->getParent()->isInvalidDecl())
                    || (proto != nullptr && ProtoHasInvalidRecord(proto));
            };
            for (const FunctionDecl* fd : usedFunctionWork)
            {
                if (overInvalidRecord(fd)) continue;
                hand(const_cast<FunctionDecl*>(fd));
            }
            // Registered lazily, like the shared static members above: CodeGen emits the storage
            // only when a body in this module references it.
            {
                std::unordered_set<const VarDecl*> handed(sharedVarWork.begin(),
                                                          sharedVarWork.end());
                for (const VarDecl* vd : usedFunctions.vars)
                    if (handed.insert(vd).second) handOverStaticVar(vd);
            }
            /*
             * A live Interpreter: every specialization an EARLIER chunk instantiated hangs off
             * the shared header templates with its used-bit set, and CodeGen emits a free
             * function only if it was handed the declaration. Register each one lazily - no
             * address request - so it is emitted only when a body in THIS module calls it.
             * Requesting them all is what made each request's module carry every body the
             * group ever emitted (and a cache entry keyed on one request link another
             * program's thunks).
             */
            if (!st.sharedDecls.empty() && plan == nullptr)
            {
                std::vector<const FunctionDecl*> sharedWork;
                UsedFunctionVisitor sharedFunctions(sharedWork);
                sharedFunctions.seen = usedFunctions.seen;
                for (Decl* d : st.sharedDecls) sharedFunctions.TraverseDecl(d);
                for (const FunctionDecl* fd : sharedWork)
                    if (!overInvalidRecord(fd))
                        cg.HandleTopLevelDecl(DeclGroupRef(const_cast<FunctionDecl*>(fd)));
            }
            /*
             * A vtable belongs to exactly ONE translation unit: the Itanium ABI anchors it in the
             * TU that defines the class's KEY function (the first non-pure, non-inline virtual
             * member). Emitting it here as well would duplicate the strong symbol the bound
             * library already exports - which is exactly what a blind HandleVTable does, since
             * Sema normally applies this rule before ever calling it. So ask Clang for the key
             * function and emit only when there is none, i.e. the all-inline hierarchy this
             * milestone is about, where the vtable is linkonce_odr and merges by ODR.
             */
            for (const CXXRecordDecl* rd : st.vtableWork)
            {
                const CXXRecordDecl* def = rd != nullptr ? rd->getDefinition() : nullptr;
                if (def == nullptr || !def->isDynamicClass() || def->isDependentContext()) continue;
                if (ctx.getCurrentKeyFunction(def) != nullptr) continue;   // anchored elsewhere
                // The C++ runtime owns type_info's vtable (vcruntime / libc++abi); clang's own RTTI
                // descriptors reference that external symbol. The Microsoft ABI has no key
                // function to say so, and a local copy splits the name on the way into the program.
                if (def->getIdentifier() != nullptr && def->getName() == "type_info"
                    && (def->getDeclContext()->getRedeclContext()->isTranslationUnit()
                        || def->getDeclContext()->getRedeclContext()->isStdNamespace()))
                    continue;
                /*
                 * MSVC's ::type_info has no key function under the Microsoft ABI, yet its vftable
                 * lives in vcruntime and Sema never marks it used. CodeGen's RTTI descriptors
                 * declare `??_7type_info@@6B@` as a plain global, so handing the class over makes
                 * the vftable emitter treat that declaration as its RTTI alias and crash.
                 */
                if (ctx.getTargetInfo().getCXXABI().isMicrosoft() && def->getIdentifier() != nullptr
                    && def->getName() == "type_info"
                    && def->getDeclContext()->getRedeclContext()->isTranslationUnit())
                    continue;
                /*
                 * `extern template class X<char>;` (libc++ does this for basic_ios, basic_istream,
                 * basic_ostream, basic_streambuf, ...) is an explicit instantiation DECLARATION: a
                 * template specialization has no key function, but the vtable is still anchored in
                 * the translation unit that carries the matching explicit instantiation DEFINITION,
                 * i.e. inside the library. Clang gives such a vtable plain external linkage, so a
                 * blind HandleVTable emits a STRONG duplicate of a symbol the library exports - two
                 * companion modules that both touch the class then collide on the way into the
                 * program module ("symbol multiply defined"). Leave it to the library.
                 */
                if (def->getTemplateSpecializationKind()
                    == clang::TSK_ExplicitInstantiationDeclaration)
                {
                    if (st.req.verbose)
                        std::cout << "[verbose]   vtable left to the library, '"
                                  << def->getQualifiedNameAsString()
                                  << "' is an explicit instantiation declaration\n";
                    continue;
                }
                if (plan == nullptr)
                {
                    cg.HandleVTable(const_cast<CXXRecordDecl*>(def));
                    continue;
                }
                // The Microsoft ABI names no single vftable symbol; the demand pass hands those
                // over unconditionally.
                std::string vtableName = "vftable:" + def->getQualifiedNameAsString();
                if (!ctx.getTargetInfo().getCXXABI().isMicrosoft())
                {
                    vtableName.clear();
                    llvm::raw_string_ostream os(vtableName);
                    planMangler->mangleCXXVTable(def, os);
                }
                plan->vtables.emplace(std::move(vtableName), def);
            }

            if (plan != nullptr)
            {
                /*
                 * No module to inspect: a member binds locally when the demand pass WILL emit
                 * its body - it has one, reaches no error and is not dependent - or a generated
                 * thunk stands in for it (BindCxxVirtualThunk proved that body). Otherwise the
                 * library must own the symbol, or the member is refused at the use site.
                 */
                for (const auto& w : st.memberAbiWork)
                {
                    if (w.recordIdx >= st.out.records.size()) continue;
                    RawRecord& rec = st.out.records[w.recordIdx];
                    if (w.memberIdx >= rec.members.size()) continue;
                    RawCxxMember& m = rec.members[w.memberIdx];
                    if (w.md != nullptr && errorReach->Reaches(w.md))
                    {
                        if (m.bindRefusal.empty())
                        {
                            m.bindRefusal = "cannot be instantiated for these template arguments "
                                            "(clang reported an error inside the body it generated)";
                            m.refusalCause = errorReach->CauseOf(w.md);
                        }
                        m.linkageName.clear();
                        m.abi = RawAbi{};
                        continue;
                    }
                    if (!m.needsLocalDefinition) continue;
                    if (m.linkageName.empty() && w.md != nullptr)
                        m.linkageName = CxxLinkageName(ctx, MemberGlobalDecl(w.md));
                    if (m.linkageName.empty()) continue;
                    const FunctionDecl* body = nullptr;
                    const bool emitted = m.linkageName.starts_with("__cflat_")
                        || (w.md != nullptr && !isDependentCodeGenDecl(w.md)
                            // Sema never defines a trivial defaulted member; nothing emits it.
                            && !(w.md->isDefaulted() && w.md->isTrivial())
                            && ((w.md->hasBody(body) && body != nullptr)
                                || w.md->isImplicitlyInstantiable() || w.md->isDefaulted()));
                    if (emitted || (w.md != nullptr && LibraryOwnsMemberSymbol(w.md)))
                        m.needsLocalDefinition = false;
                    else
                    {
                        m.linkageName.clear();
                        m.abi = RawAbi{};
                    }
                }
                plan->weakPromoteSymbols.insert(plan->weakPromoteSymbols.end(),
                                                st.out.weakPromoteSymbols.begin(),
                                                st.out.weakPromoteSymbols.end());
                plan->weakPromotePerGroupSymbols.insert(plan->weakPromotePerGroupSymbols.end(),
                    st.out.weakPromotePerGroupSymbols.begin(),
                    st.out.weakPromotePerGroupSymbols.end());
                for (const auto& [mangled, programName] : st.out.localInlineAliases)
                    plan->renamed[programName] = mangled;
                ++plan->recordedChunks;
                st.out.demandRecorded = true;
                return;
            }

            // Phase 3: flush. This emits the deferred definitions and finalizes the module.
            if (st.req.verbose)
                std::cout << std::format(
                    "[verbose]   extraction codegen flush: {} clang error(s) so far\n",
                    ctx.getDiagnostics().getClient()->getNumErrors());
            cg.HandleTranslationUnit(ctx);

            llvm::Module* mod = cg.GetModule();
            if (mod == nullptr) return;   // CodeGen error: nothing is bound to a local definition

            // An internal-linkage namespace constant (`constexpr Color kRed{1,0,0};`) exists in
            // no library and is invisible across modules; weak_odr makes the emitted storage
            // bindable and still merges if two companion modules carry it.
            for (const std::string& sym : st.out.weakPromoteSymbols)
            {
                llvm::GlobalVariable* gv = mod->getGlobalVariable(sym, /*AllowInternal*/ true);
                if (gv == nullptr || gv->isDeclaration()) continue;
                const bool perGroup = std::find(st.out.weakPromotePerGroupSymbols.begin(),
                              st.out.weakPromotePerGroupSymbols.end(), sym)
                    != st.out.weakPromotePerGroupSymbols.end();
                if (!gv->hasLocalLinkage() && !perGroup) continue;
                if (perGroup)
                    RenamePerGroupGlobal(gv, StaticCxxGlobalAlias(st.req.cxxImportGroupKey, sym));
                if (gv->hasLocalLinkage())
                    gv->setLinkage(llvm::GlobalValue::WeakODRLinkage);
                gv->setVisibility(llvm::GlobalValue::DefaultVisibility);
            }
            for (const auto& [mangled, programName] : st.out.localInlineAliases)
                if (llvm::Function* fn = mod->getFunction(mangled);
                    fn != nullptr && fn->hasLocalLinkage())
                    AddStaticInlineProgramThunk(*mod, fn, programName);

            unsigned defs = 0;
            for (const llvm::Function& f : mod->functions())
                if (!f.isDeclaration()) ++defs;
            for (const llvm::GlobalVariable& g : mod->globals())
                if (!g.isDeclaration()) ++defs;
            if (defs == 0)
            {
                for (const auto& w : st.memberAbiWork)
                {
                    if (w.recordIdx >= st.out.records.size()
                        || w.memberIdx >= st.out.records[w.recordIdx].members.size())
                        continue;
                    RawCxxMember& m = st.out.records[w.recordIdx].members[w.memberIdx];
                    if (!m.needsLocalDefinition || w.md == nullptr) continue;
                    if (LibraryOwnsMemberSymbol(w.md)) m.needsLocalDefinition = false;
                }
                return;
            }

            /*
             * A member whose body Clang did not emit must stay refused: prove the symbol is a
             * DEFINITION in this module rather than trusting the request above. Members that came
             * with an out-of-line definition in the bound library keep needsLocalDefinition=false,
             * which is why only the candidates are re-checked here.
             */
            for (const auto& w : st.memberAbiWork)
            {
                if (w.recordIdx >= st.out.records.size()) continue;
                RawRecord& rec = st.out.records[w.recordIdx];
                if (w.memberIdx >= rec.members.size()) continue;
                RawCxxMember& m = rec.members[w.memberIdx];
                /*
                 * Refuse a member whose instantiation, or anything it calls, Clang reported an
                 * error in (Phase 0 above). Its body is gone or empty, so binding it would call
                 * into nothing; the rest of the class stays usable.
                 */
                if (w.md != nullptr && errorReach->Reaches(w.md))
                {
                    if (m.bindRefusal.empty())
                    {
                        m.bindRefusal = "cannot be instantiated for these template arguments "
                                        "(clang reported an error inside the body it generated)";
                        m.refusalCause = errorReach->CauseOf(w.md);
                    }
                    m.linkageName.clear();
                    m.abi = RawAbi{};
                    continue;
                }
                if (!m.needsLocalDefinition) continue;
                if (m.linkageName.empty() && w.md != nullptr)
                    m.linkageName = CxxLinkageName(ctx, MemberGlobalDecl(w.md));
                if (m.linkageName.empty()) continue;
                /*
                 * getNamedValue, not getFunction: on Itanium the COMPLETE-object destructor (D1) of
                 * a class with no virtual bases is emitted as a GlobalAlias onto the base-object
                 * destructor (D2), and an alias is not an llvm::Function. The symbol is still a
                 * definition in this module and a legal call target, so resolve through it.
                 */
                const llvm::GlobalValue* gv = mod->getNamedValue(m.linkageName);
                if (const auto* ga = llvm::dyn_cast_or_null<llvm::GlobalAlias>(gv))
                    gv = llvm::dyn_cast_or_null<llvm::GlobalValue>(ga->getAliasee());
                const auto* fn = llvm::dyn_cast_or_null<llvm::Function>(gv);
                if (fn != nullptr && !fn->isDeclaration())
                    m.needsLocalDefinition = false;   // the companion module carries the body
                else if (LibraryOwnsMemberSymbol(w.md))
                    m.needsLocalDefinition = false;
                else
                {
                    m.linkageName.clear();            // no symbol anywhere: refuse at the use site
                    m.abi = RawAbi{};
                }
            }
            // A static data member can be owned by a separately compiled explicit template
            // instantiation. The request companion only sees the header, so its declaration is
            // not evidence that the library symbol is absent; preserve the mangled name.

            {
                llvm::TimeTraceScope serializeScope("CxxBitcodeSerialize");
                llvm::raw_string_ostream os(st.out.bitcode);
                CxxExtractionStageTimer serialize(st.req.verbose && st.req.cxxMode,
                                                  "bitcode serialization");
                llvm::WriteBitcodeToFile(*mod, os);
                os.flush();
            }
            st.out.emittedDefinitions = defs;
        }

        void ResetHarvestState(ExtractState& st)
        {
            st.out.sigs.clear();
            st.out.functionTemplates.clear();
            st.out.enums.clear();
            st.out.records.clear();
            st.out.typedefs.clear();
            st.out.functionPointerAbis.clear();
            st.out.globals.clear();
            st.out.macros.clear();
            st.out.funcMacros.clear();
            st.out.usingDirectives.clear();
            st.out.namespaceAliases.clear();
            st.out.classUsings.clear();
            st.out.weakPromoteSymbols.clear();
            st.out.weakPromotePerGroupSymbols.clear();

            st.emittedProbes.clear();
            st.emittedGlobals.clear();
            st.emittedUsingDecls.clear();
            st.emittedOpaqueForward.clear();
            st.emittedDefinedRecords.clear();
            st.emittedRequestedRecords.clear();
            st.abiWork.clear();
            st.functionPointerAbiWork.clear();
            st.functionPointerAbiSeen.clear();
            st.emittedFunctionTemplates.clear();
            st.memberAbiWork.clear();
            st.headerSpecialMemberWork.clear();
            st.headerSpecialMemberSeen.clear();
            st.vtableWork.clear();
            st.varEmitWork.clear();
            st.pendingStaticVarDefs.clear();
        }

        size_t CompleteIncompleteCxxTypes(ExtractState& st)
        {
            if (st.ci == nullptr || !st.ci->hasSema() || st.incompleteCxxTypes.empty()) return 0;
            Sema& sema = st.ci->getSema();
            clang::Scope tuScope(nullptr, clang::Scope::DeclScope, st.ci->getDiagnostics());
            const bool lendScope = sema.TUScope == nullptr;
            if (lendScope) sema.TUScope = &tuScope;
            struct ScopeReset
            {
                Sema& sema;
                bool active;
                ~ScopeReset() { if (active) sema.TUScope = nullptr; }
            } scopeReset{sema, lendScope};

            for (const auto& queued : st.incompleteCxxTypes)
            {
                if (!queued.type->isIncompleteType()) continue;
                const auto* cxx = queued.type->getAsCXXRecordDecl();
                if (cxx == nullptr) continue;
                sema.RequireCompleteType(cxx->getLocation(), queued.type,
                                         diag::err_incomplete_type);
            }
            sema.PerformPendingInstantiations();

            size_t completed = 0;
            for (const auto& queued : st.incompleteCxxTypes)
                if (!queued.type->isIncompleteType()) ++completed;
            return completed;
        }

        void HarvestTranslationUnit(ExtractState& st, ASTContext& ctx,
                                     TranslationUnitDecl* root, bool checkHeader,
                                     bool runAbi = true)
        {
            if (checkHeader)
            {
                RecordInScopeHeaderErrors(st);
                RecordHeaderScopeError(st, ctx);
                if (st.out.headerErrors > 0)
                {
                    if (st.req.verbose)
                        std::cout << std::format(
                            "[verbose]   bound header does not compile: {}\n",
                            st.out.firstHeaderError);
                    return;
                }
            }
            DeclVisitor v(ctx, st);
            {
                llvm::TimeTraceScope harvestScope("CxxHarvestWalk");
                CxxExtractionStageTimer harvest(st.req.verbose && st.req.cxxMode,
                                                 "record/sig harvest");
                if (st.req.cxxTypeRequests.empty() && st.req.cxxFunctionWrapperNames.empty())
                    v.TraverseDecl(root);
                else if (!st.req.cxxTypeRequests.empty() && !v.ProcessTypeRequests(root)) return;
                else v.ProcessFunctionRequests(root);
                v.PublishPendingFriendOperators();
                v.PublishPendingStaticVarDefs();
            }
            if (st.req.cxxMode && st.req.autoInstantiateCxxTypes
                && !st.incompleteCxxTypes.empty())
            {
                const size_t completed = CompleteIncompleteCxxTypes(st);
                if (st.req.verbose)
                    std::cout << std::format(
                        "[verbose]   completed {} incomplete C++ specializations in place\n",
                        completed);
                if (completed != 0)
                {
                    ResetHarvestState(st);
                    llvm::TimeTraceScope reharvestScope("CxxReharvestWalk");
                    CxxExtractionStageTimer reharvest(st.req.verbose,
                                                       "record/sig re-harvest");
                    DeclVisitor refreshed(ctx, st);
                    if (st.req.cxxTypeRequests.empty()
                        && st.req.cxxFunctionWrapperNames.empty())
                        refreshed.TraverseDecl(root);
                    else if (!st.req.cxxTypeRequests.empty()
                             && !refreshed.ProcessTypeRequests(root)) return;
                    else refreshed.ProcessFunctionRequests(root);
                    refreshed.PublishPendingFriendOperators();
                    refreshed.PublishPendingStaticVarDefs();
                }
            }
            st.stillIncompleteSpellings.clear();
            for (const auto& queued : st.incompleteCxxTypes)
                if (queued.type->isIncompleteType())
                    st.stillIncompleteSpellings.push_back(queued.spelling);
            if (st.req.cxxMode && runAbi)
            {
                llvm::TimeTraceScope abiScope("CxxAbiArrange");
                CxxExtractionStageTimer codegen(st.req.verbose,
                                                 "stage-2 CodeGen/companion emission");
                ComputeCxxAbi(st, ctx, root);
            }
        }

        struct ExtractConsumer : public ASTConsumer
        {
            ExtractState& st;
            explicit ExtractConsumer(ExtractState& s) : st(s) {}
            // Record what Sema announces; nothing is emitted here (CodeGen runs later, once, over
            // the finished AST). Only the definition-emission path replays this list.
            bool HandleTopLevelDecl(DeclGroupRef dg) override
            {
                if (st.req.RecordsDefinitionDemand())
                    for (Decl* d : dg)
                    {
                        if (st.req.requireInScope && st.ci != nullptr)
                        {
                            PresumedLoc pl = st.ci->getSourceManager().getPresumedLoc(d->getLocation());
                            if (pl.isInvalid() || !st.InScope(pl.getFilename())) continue;
                        }
                        st.announcedDecls.push_back(d);
                        st.requestDecls.push_back(d);
                    }
                return true;
            }
            void HandleTranslationUnit(ASTContext& ctx) override
            {
                HarvestTranslationUnit(st, ctx, ctx.getTranslationUnitDecl(), true);
            }
        };

        // C++ uuid harvest: walk record decls and emit name + __declspec(uuid) GUID only. The
        // header is parsed as C++ so the MIDL_INTERFACE form (with the uuid attribute) is selected;
        // the C parse never sees it. Everything but the GUID is ignored.
        struct UuidVisitor : public RecursiveASTVisitor<UuidVisitor>
        {
            ExtractState& st;
            explicit UuidVisitor(ExtractState& s) : st(s) {}
            bool VisitRecordDecl(RecordDecl* rd)
            {
                if (!rd->getIdentifier()) return true;
                const auto* u = rd->getAttr<clang::UuidAttr>();
                if (!u) return true;
                RawRecord rec;
                rec.name = rd->getNameAsString();
                rec.uuid = u->getGuid().str();   // canonical hyphenated GUID as written in the attr
                st.out.records.push_back(std::move(rec));
                return true;
            }
        };

        struct UuidConsumer : public ASTConsumer
        {
            ExtractState& st;
            explicit UuidConsumer(ExtractState& s) : st(s) {}
            void HandleTranslationUnit(ASTContext& ctx) override
            {
                UuidVisitor v(st);
                v.TraverseDecl(ctx.getTranslationUnitDecl());
            }
        };

        struct UuidAction : public ASTFrontendAction
        {
            ExtractState& st;
            explicit UuidAction(ExtractState& s) : st(s) {}
            std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&, StringRef) override
            {
                return std::make_unique<UuidConsumer>(st);
            }
        };

        struct ExtractAction : public ASTFrontendAction
        {
            ExtractState& st;
            explicit ExtractAction(ExtractState& s) : st(s) {}
            bool BeginSourceFileAction(CompilerInstance& ci) override
            {
                st.ci = &ci;
                if (st.req.wantIncludes)
                    ci.getPreprocessor().addPPCallbacks(
                        std::make_unique<IncludeCollector>(ci.getPreprocessor(), st));
                return true;
            }
            std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&, StringRef) override
            {
                return std::make_unique<ExtractConsumer>(st);
            }
        };

        // Diagnostic consumer that silently swallows everything (like IgnoringDiagConsumer) but
        // tallies the "unknown type name" errors that originate inside an #included header rather
        // than in our in-memory stub. That pattern is the fingerprint of a non-self-contained
        // header missing a prerequisite include; the header-bind path turns it into a helpful
        // "import them as one group" diagnostic. The intentional macro-probe errors all sit in
        // the main stub file, so isInMainFile() filters them out.

        // Build a CompilerInstance from driver args + an optional in-memory main file, then run
        // `action`. When `source` is non-empty it is remapped onto req.mainFileName; otherwise
        // req.realPath is parsed from disk. When `outPrereqErrors` is non-null it receives the
        // count (and `outFirstPrereqError` the text) of missing-prerequisite errors seen in the
        // parse - used by the header-bind path to detect a header that needs a grouped import.
        bool RunAction(const ExtractRequest& req, const std::string& source,
                       FrontendAction& action, std::string& err,
                       unsigned* outPrereqErrors = nullptr,
                       std::string* outFirstPrereqError = nullptr,
                       ExtractResult* outTargetFacts = nullptr)
        {
            const std::string& inputName = source.empty() ? req.realPath : req.mainFileName;
            if (inputName.empty()) { err = "no input file"; return false; }

            std::vector<const char*> cargs;
            cargs.reserve(req.args.size() + 2);
            cargs.push_back("clang");
            for (const auto& a : req.args) cargs.push_back(a.c_str());
            cargs.push_back(inputName.c_str());

            llvm::IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
            DiagnosticOptions diagOptions;
            llvm::IntrusiveRefCntPtr<DiagnosticsEngine> diagEngine(
                new DiagnosticsEngine(diagIDs, diagOptions, new IgnoringDiagConsumer(), true));

            std::shared_ptr<CompilerInvocation> invocation;
            {
                llvm::TimeTraceScope invScope("CreateInvocation", inputName);
                std::unique_ptr<CompilerInvocation> invocationUP =
                    [&]() {
                        clang::CreateInvocationOptions invOptions;
                        invOptions.Diags = diagEngine;
                        invOptions.RecoverOnError = true;
                        return clang::createInvocation(cargs, invOptions);
                    }();
                if (!invocationUP) { err = "createInvocation failed"; return false; }
                // Header binds only need declarations. Skipping function bodies avoids parsing
                // inline definitions in system headers (e.g. an `&`-taking __inline in math.h
                // trips a clang classifier assert in assertion-enabled builds) and is faster;
                // the FunctionDecl + signature are still produced.
                if (req.skipFunctionBodies)
                    invocationUP->getFrontendOpts().SkipFunctionBodies = true;
                // createInvocation reduces the driver job to a parse, so neither the precompile
                // action nor -o survives. Name both outright rather than through the args.
                if (!req.pchOutputPath.empty())
                {
                    invocationUP->getFrontendOpts().ProgramAction = clang::frontend::GeneratePCH;
                    invocationUP->getFrontendOpts().OutputFile = req.pchOutputPath;
                }
                // The driver puts -disable-free on every cc1 job, so EndSourceFile BURIES the
                // ASTContext / Preprocessor / Sema instead of deleting them: ~60 MB leaked per
                // windows.h parse. The CLI batch and the LSP re-extract on every signature-cache
                // eviction, so this must free (clangd resets the same flag).
                invocationUP->getFrontendOpts().DisableFree = false;
                // A destructor whose body only forwards to its single base is normally folded
                // into that base destructor (-mconstructor-aliases): Clang replaces every use and
                // erases the symbol, so a companion module ends up with NO symbol for a class
                // whose destructor cflat binds by name. Emit the forwarding body instead.
                invocationUP->getCodeGenOpts().CXXCtorDtorAliases = false;
                // Clang derives __OPTIMIZE__ / __NO_INLINE__ from the CodeGen level, so restore the
                // harvest's -O0 macro view by hand: headers branch on them (fortify, extern inlines).
                if (req.emitReferencedInlineDefinitions)
                {
                    if (invocationUP->getCodeGenOpts().OptimizationLevel == 0)
                    {
                        invocationUP->getPreprocessorOpts().addMacroUndef("__OPTIMIZE__");
                        invocationUP->getPreprocessorOpts().addMacroDef("__NO_INLINE__=1");
                    }
                    invocationUP->getCodeGenOpts().OptimizationLevel = 1;
                    invocationUP->getCodeGenOpts().DisableLLVMPasses = true;
                }
                invocation.reset(invocationUP.release());
            }

            auto ci = std::make_unique<clang::CompilerInstance>(std::move(invocation));
            // Own the consumer via the engine, but keep a raw pointer so the prereq tally can be
            // read back after the parse (the engine, hence the consumer, outlives this scope).
            PrereqDiagConsumer* prereqConsumer = new PrereqDiagConsumer();
            ci->createDiagnostics(prereqConsumer, /*own*/ true);

            if (!source.empty())
            {
                std::unique_ptr<llvm::MemoryBuffer> buf =
                    llvm::MemoryBuffer::getMemBufferCopy(source, req.mainFileName);
                ci->getPreprocessorOpts().addRemappedFile(req.mainFileName, buf.release());
            }

            {
                llvm::TimeTraceScope execScope("ExecuteFrontend", inputName);
                if (!ci->ExecuteAction(action)) { err = "ExecuteAction failed"; return false; }
            }
            if (outTargetFacts)
            {
                const clang::TargetInfo& target = ci->getTarget();
                outTargetFacts->longDoubleWidth = target.getLongDoubleWidth();
                outTargetFacts->longDoubleIsIEEEDouble =
                    &target.getLongDoubleFormat() == &llvm::APFloat::IEEEdouble();
                outTargetFacts->targetTriple = target.getTriple().str();
            }
            if (outPrereqErrors) *outPrereqErrors = prereqConsumer->prereqErrors;
            if (outFirstPrereqError) *outFirstPrereqError = prereqConsumer->firstPrereqError;
            if (outTargetFacts) outTargetFacts->firstError = prereqConsumer->firstError;
            return true;
        }

        // Emits C inline bodies and records the gnu_inline functions (by IR name) along the way.
        class CInlineBodyAction : public clang::EmitLLVMOnlyAction
        {
        public:
            using clang::EmitLLVMOnlyAction::EmitLLVMOnlyAction;
            std::set<std::string> gnuInline;

        protected:
            std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& ci,
                                                                  llvm::StringRef file) override
            {
                struct Collector : clang::ASTConsumer
                {
                    std::set<std::string>& names;
                    explicit Collector(std::set<std::string>& n) : names(n) {}
                    bool HandleTopLevelDecl(clang::DeclGroupRef group) override
                    {
                        for (clang::Decl* d : group)
                            if (const auto* fd = llvm::dyn_cast<clang::FunctionDecl>(d))
                                if (fd->hasAttr<clang::GNUInlineAttr>()
                                    || (fd->getASTContext().getLangOpts().GNUInline
                                        && fd->isInlineSpecified()))
                                {
                                    names.insert(fd->getNameAsString());
                                    if (const auto* label = fd->getAttr<clang::AsmLabelAttr>())
                                        names.insert(label->getLabel().str());
                                }
                        return true;
                    }
                };
                std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
                consumers.push_back(std::make_unique<Collector>(gnuInline));
                consumers.push_back(clang::EmitLLVMOnlyAction::CreateASTConsumer(ci, file));
                return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
            }
        };

        std::string SanitizeCInlineKeepName(const std::string& name)
        {
            std::string out = "__cflat_keep_";
            for (unsigned char c : name)
                out += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
            return out;
        }
    } // namespace

    bool EmitCInlineBodies(const std::vector<std::string>& args,
                           const std::vector<std::string>& headers,
                           const std::vector<std::pair<std::string, bool>>& functions,
                           std::string& bitcode, std::string& err)
    {
        bitcode.clear();
        if (headers.empty() || functions.empty()) return true;
        std::string source;
        for (const auto& header : headers)
        {
            std::string path = header;
            std::replace(path.begin(), path.end(), '\\', '/');
            source += "#include \"" + path + "\"\n";
        }
        // No extern redeclaration: a plain inline gets exactly what clang emits for the target
        // (available_externally on Darwin/Linux, linkonce_odr under MSVC C inline semantics).
        for (const auto& [name, isStatic] : functions)
            source += "__attribute__((used)) static void* " + SanitizeCInlineKeepName(name)
                + " = (void*)&" + name + ";\n";

        ExtractRequest req;
        req.mainFileName = "cflat_c_inline_demand.c";
        req.args = args;
        req.args.push_back("-x");
        req.args.push_back("c");
        req.emitReferencedInlineDefinitions = true;
        llvm::LLVMContext context;
        CInlineBodyAction action(&context);
        if (!RunAction(req, source, action, err))
        {
            if (!err.starts_with("clang: ")) err = "clang: " + err;
            return false;
        }
        std::unique_ptr<llvm::Module> module = action.takeModule();
        if (!module) { err = "clang: inline body compilation produced no LLVM module"; return false; }
        for (const auto& [name, isStatic] : functions)
        {
            llvm::Function* function = module->getFunction(name);
            if (isStatic && (function == nullptr || function->isDeclaration()))
            {
                err = "clang: inline body '" + name + "' was not emitted";
                return false;
            }
            if (isStatic) function->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
        /*
         * An available_externally body is an inlining copy of a definition that lives elsewhere.
         * A gnu_inline one (glibc fortify wrappers) or one calling its own symbol (an asm-label
         * redirect to the real function) must stay a declaration: a private copy would recurse.
         * Requested plain inlines keep their linkage for the caller's per-opt-level choice;
         * every other one (reached only from a demanded body) becomes this module's private copy.
         * So does an MSVC C inline (linkonce_odr, discardable): its real name could collide on
         * COFF with a strong or imported definition. weak / weak_odr keep name and linkage.
         * Only direct calls take the private copy: an address use inside a demanded body (the
         * body returns or compares the function) must bind the real symbol so it compares equal
         * across TUs (C11 6.5.9) - the external definition in some other TU, or under MSVC C
         * inline semantics clang-cl's own linkonce_odr comdat body kept under its name.
         */
        std::set<std::string> requested;
        for (const auto& [name, isStatic] : functions) requested.insert(name);
        auto isDirectCall = [](const llvm::Use& use) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(use.getUser());
            return call != nullptr && call->isCallee(&use);
        };
        auto privatize = [&](llvm::Function& function, bool keepComdatBody) {
            bool addressUse = false;
            for (const llvm::Use& use : function.uses())
                if (!isDirectCall(use)) addressUse = true;
            llvm::Function* copy = &function;
            if (addressUse)
            {
                llvm::ValueToValueMapTy valueMap;
                copy = llvm::CloneFunction(&function, valueMap);
                copy->setName(function.getName() + ".cflat_call");
                function.replaceUsesWithIf(copy, isDirectCall);
                if (!keepComdatBody) function.deleteBody();
            }
            copy->setLinkage(llvm::GlobalValue::InternalLinkage);
            copy->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
            copy->setComdat(nullptr);
        };
        std::vector<llvm::Function*> bodies;
        for (llvm::Function& function : module->functions())
            if (!function.isDeclaration()) bodies.push_back(&function);
        for (llvm::Function* function : bodies)
        {
            if (function->hasLinkOnceLinkage() && !requested.contains(function->getName().str()))
            {
                privatize(*function, true);
                continue;
            }
            if (!function->hasAvailableExternallyLinkage()) continue;
            bool selfCall = false;
            for (const llvm::User* user : function->users())
                if (const auto* inst = llvm::dyn_cast<llvm::Instruction>(user))
                    if (inst->getFunction() == function) selfCall = true;
            if (selfCall || action.gnuInline.contains(function->getName().str()))
                function->deleteBody();
            else if (!requested.contains(function->getName().str()))
                privatize(*function, false);
        }
        for (const auto& [name, isStatic] : functions)
            if (llvm::GlobalVariable* keep =
                module->getGlobalVariable(SanitizeCInlineKeepName(name), true))
                {
                    llvm::removeFromUsedLists(*module, [keep](llvm::Constant* value) {
                        return value->stripPointerCasts() == keep;
                    });
                    keep->eraseFromParent();
                }
        // Maintainer ruling 2026-10-02: skip LLVM verification of clang-generated companion code.
        llvm::raw_string_ostream stream(bitcode);
        llvm::WriteBitcodeToFile(*module, stream);
        stream.flush();
        return true;
    }

    std::function<void()> AttachCxxMacroPrepass(clang::Preprocessor& pp,
                                                const ExtractRequest& req,
                                                ExtractResult& out)
    {
        auto active = std::make_shared<bool>(true);
        pp.addPPCallbacks(std::make_unique<IncrementalMacroPrepassAction>(
            pp, req, out, active));
        return [active] { *active = false; };
    }

    bool ExtractCxxIncremental(const ExtractRequest& req, clang::CompilerInstance& ci,
                               clang::TranslationUnitDecl* root,
                               clang::TranslationUnitDecl* headerRoot,
                               const std::vector<clang::TranslationUnitDecl*>& extraRoots,
                               llvm::Module* module,
                               ExtractResult& out, std::string& err, bool checkHeader,
                               clang::TranslationUnitDecl* preludeRoot,
                               const std::vector<clang::Decl*>* announcedDecls)
    {
        if (root == nullptr)
        {
            err = "incremental parse returned no translation-unit part";
            return false;
        }
        (void)module;
        ExtractState st(req, out);
        st.ci = &ci;
        st.contextRoot = headerRoot;
        const clang::TargetInfo& target = ci.getTarget();
        out.longDoubleWidth = target.getLongDoubleWidth();
        out.longDoubleIsIEEEDouble =
            &target.getLongDoubleFormat() == &llvm::APFloat::IEEEdouble();
        out.targetTriple = target.getTriple().str();
        if (req.RecordsDefinitionDemand() && headerRoot != nullptr)
            for (clang::Decl* decl : headerRoot->decls())
            {
                // The full header walk belongs to chunk 0. Requests keep the shared decls
                // available for on-demand helper emission, but do not replay them as announced.
                if (root == headerRoot) st.announcedDecls.push_back(decl);
                st.sharedDecls.push_back(decl);
            }
        if (req.RecordsDefinitionDemand() && root != headerRoot)
            for (clang::Decl* decl : root->decls())
            {
                st.announcedDecls.push_back(decl);
                st.requestDecls.push_back(decl);
            }
        // Includes committed in a separate chunk are shared roots too. Keep their declarations
        // available to CodeGen for helper references, but do not harvest them per request.
        if (req.RecordsDefinitionDemand() && preludeRoot != nullptr
            && preludeRoot != headerRoot)
            for (clang::Decl* decl : preludeRoot->decls())
            {
                st.sharedDecls.push_back(decl);
            }
        // What Sema announced while parsing this chunk: implicit instantiations and the members
        // an explicit instantiation defines, none of which are children of the chunk's root.
        if (req.RecordsDefinitionDemand() && announcedDecls != nullptr)
        {
            st.announcedDecls.insert(st.announcedDecls.end(), announcedDecls->begin(),
                                     announcedDecls->end());
            st.requestDecls.insert(st.requestDecls.end(), announcedDecls->begin(),
                                   announcedDecls->end());
        }
        // Chunk 0 (root == headerRoot) harvests the group and committed-prelude surfaces. A
        // request only harvests its own root and the request's earlier stage-1 root, if present.
        if (root == headerRoot && preludeRoot != nullptr && preludeRoot != headerRoot)
            HarvestTranslationUnit(st, ci.getASTContext(), preludeRoot, false, false);
        for (clang::TranslationUnitDecl* extra : extraRoots)
            if (extra != nullptr && extra != root && extra != headerRoot)
                HarvestTranslationUnit(st, ci.getASTContext(), extra, false, false);
        HarvestTranslationUnit(st, ci.getASTContext(), root,
                               checkHeader && root == headerRoot, true);
        out.incompleteCxxTypeSpellings.insert(out.incompleteCxxTypeSpellings.end(),
                                              st.stillIncompleteSpellings.begin(),
                                              st.stillIncompleteSpellings.end());
        return true;
    }

    bool ExtractCxxMacroPrepass(const ExtractRequest& req, ExtractResult& out, std::string& err)
    {
        ExtractState st(req, out);
        CxxExtractionStageTimer parseStage(req.verbose && req.cxxMode,
                                           "clang parse stage 1");
        PrepassAction prepass(st);
        if (!RunAction(req, req.source, prepass, err)) return false;
        out.macroProbes = std::move(st.probes);
        return true;
    }

    bool ExtractCInterop(const ExtractRequest& req, ExtractResult& out, std::string& err)
    {
        ExtractState st(req, out);

        // Precompile the group's include prologue. One parse now, `-include-pch` later.
        if (!req.pchOutputPath.empty())
        {
            if (req.cxxHeaderParseGuard != nullptr
                && !req.cxxHeaderParseGuard("clang precompile header"))
                return false;
            llvm::TimeTraceScope pchScope("GeneratePch", req.mainFileName);
            CxxExtractionStageTimer pchStage(req.verbose && req.cxxMode, "clang precompile header");
            clang::GeneratePCHAction generate;
            return RunAction(req, req.source, generate, err, nullptr, nullptr, &out);
        }

        // C++ uuid harvest: a single full parse that only collects record __declspec(uuid) GUIDs.
        if (req.uuidHarvestCxx)
        {
            if (req.cxxHeaderParseGuard != nullptr
                && !req.cxxHeaderParseGuard("C++ uuid harvest"))
                return false;
            llvm::TimeTraceScope parseScope("UuidHarvest", req.mainFileName);
            UuidAction harvest(st);
            return RunAction(req, req.source, harvest, err);
        }

        // Stage 1: preprocess-only prepass to discover macro names (header path only).
        std::string fullSource = req.source;
        if (req.wantMacros && !req.source.empty())
        {
            {
                llvm::TimeTraceScope prepassScope("MacroPrepass", req.mainFileName);
                CxxExtractionStageTimer parseStage(req.verbose && req.cxxMode,
                                                   "clang parse stage 1");
                PrepassAction prepass(st);
                if (!RunAction(req, req.source, prepass, err)) return false;
            }

            // Append a value/type probe per discovered object-like macro to the main stub.
            {
                llvm::TimeTraceScope probeScope("BuildMacroProbes", req.mainFileName);
                CxxExtractionStageTimer probeStage(req.verbose && req.cxxMode,
                                                   "macro probes");
                std::string probes;
                probes.reserve(st.probes.size() * 48);
                for (size_t i = 0; i < st.probes.size(); ++i)
                {
                    // No parens around the macro: a macro that expands to a type name (e.g.
                    // math.h `complex` -> `_complex`) would, when parenthesized, parse as a
                    // C-style cast with a missing operand and build a classification-inconsistent
                    // node that trips a clang assert (Expr::ClassifyImpl, "isPRValue()") during
                    // `__auto_type` deduction. Unparenthesized, the same macro is a clean "type
                    // name where an expression was expected" error, leaving no usable initializer
                    // - which the probe reader skips. `=` binds looser than any operator in a
                    // value macro body, so dropping the parens does not change folded values.
                    probes += "static const __auto_type ";
                    probes += kProbePrefix;
                    probes += std::to_string(i);
                    probes += " = ";
                    probes += st.probes[i].name;
                    probes += ";\n";
                }
                fullSource = req.source + probes;
            }
        }

        // Stage 2: the single full parse - harvests decls and reads the probe VarDecls.
        {
            if (req.cxxHeaderParseGuard != nullptr
                && !req.cxxHeaderParseGuard("clang parse stage 2"))
                return false;
            llvm::TimeTraceScope parseScope("FullParse", req.mainFileName.empty() ? req.realPath : req.mainFileName);
            CxxExtractionStageTimer parseStage(req.verbose && req.cxxMode,
                                               "clang parse stage 2");
            ExtractAction extract(st);
            bool ok = RunAction(req, fullSource, extract, err, &out.prereqErrors,
                                &out.firstPrereqError, &out);
            for (auto& record : out.records)
                for (auto& variable : record.staticVars)
                    if (variable.initializerFailure.starts_with("pending:") && !out.firstError.empty())
                        variable.initializerFailure = out.firstError;

            if (ok && req.cxxMode && req.autoInstantiateCxxTypes
                && !st.incompleteCxxTypes.empty())
            {
                // The CompilerInstance is gone; only the spelling snapshot is safe to read.
                const std::vector<std::string>& incompleteSpellings = st.stillIncompleteSpellings;
                if (!incompleteSpellings.empty())
                {
                    ExtractRequest retry = req;
                    retry.autoInstantiateCxxTypes = false;
                    retry.source = req.source;
                    for (const auto& queued : st.incompleteCxxTypes)
                        retry.source += "\ntemplate class " + queued.spelling + ";\n";
                    if (req.verbose)
                        std::cout << std::format(
                            "[verbose]   falling back to a reparse for {} incomplete C++ specializations\n",
                            incompleteSpellings.size());
                    ExtractResult retried;
                    std::string retryError;
                    if (ExtractCInterop(retry, retried, retryError))
                    {
                        bool recovered = true;
                        for (const RawSig& original : out.sigs)
                        {
                            bool namesIncomplete = false;
                            for (const std::string& spelling : incompleteSpellings)
                                if (original.retType == spelling
                                    || std::find(original.paramTypes.begin(), original.paramTypes.end(), spelling)
                                           != original.paramTypes.end())
                                { namesIncomplete = true; break; }
                            if (!namesIncomplete) continue;
                            auto found = std::find_if(retried.sigs.begin(), retried.sigs.end(),
                                [&](const RawSig& candidate) {
                                    return candidate.linkageName == original.linkageName
                                        && candidate.name == original.name;
                                });
                            if (found == retried.sigs.end() || !found->abi.valid)
                            { recovered = false; break; }
                        }
                        if (recovered)
                            for (const RawRecord& original : out.records)
                                for (const RawCxxMember& member : original.members)
                                {
                                    bool namesIncomplete = false;
                                    for (const std::string& spelling : incompleteSpellings)
                                        if (member.retType == spelling
                                            || std::find(member.paramTypes.begin(), member.paramTypes.end(), spelling)
                                                   != member.paramTypes.end())
                                        { namesIncomplete = true; break; }
                                    if (!namesIncomplete) continue;
                                    auto record = std::find_if(retried.records.begin(), retried.records.end(),
                                        [&](const RawRecord& candidate) { return candidate.name == original.name; });
                                    if (record == retried.records.end()) { recovered = false; break; }
                                    auto found = std::find_if(record->members.begin(), record->members.end(),
                                        [&](const RawCxxMember& candidate) {
                                            return candidate.name == member.name
                                                && candidate.retType == member.retType
                                                && candidate.paramTypes == member.paramTypes;
                                        });
                                    if (found == record->members.end() || !found->abi.valid
                                        || found->bindRefusal.size() != 0)
                                    { recovered = false; break; }
                                }
                        if (recovered) out = std::move(retried);
                    }
                }
            }

            // A probe whose injected variable never reached the AST (the body is a type name or
            // an unknown identifier) still reports its alias spelling; the binder decides.
            for (size_t i = 0; i < st.probes.size(); ++i)
            {
                const CxxMacroProbe& mp = st.probes[i];
                if (mp.aliasTarget.empty()) continue;
                if (st.emittedProbes.count((unsigned)i)) continue;
                RawMacro m;
                m.name = mp.name; m.file = mp.file; m.line = mp.line; m.col = mp.col;
                m.aliasTarget = mp.aliasTarget;
                m.kind = RawMacro::Skip;
                out.macros.push_back(std::move(m));
            }
            return ok;
        }
    }

    /*
     * One CodeGenerator over the whole group. Deferrable recorded declarations are handed over
     * as a real TU would hand them (emitted only when referenced); a declaration CodeGen must
     * emit on sight (strong, explicit instantiation, `used` helper) is held back and handed
     * only once something needs its symbol. What is needed is found by CodeGen itself: after a
     * round, any held symbol the module still only declares joins the demand and the round
     * reruns. Rounds are few - an explicit instantiation's out-of-line member reached through
     * an inline body is the typical addition.
     */
    bool EmitCxxDemandCompanion(clang::CompilerInstance& ci, CxxDemandPlan& plan,
                                const std::vector<std::string>& demand,
                                const std::unordered_map<const clang::FunctionDecl*, std::string>* poisoned,
                                bool verbose, std::string& bitcode, CxxDemandStats& stats,
                                std::string& err, llvm::LLVMContext* targetContext,
                                std::unique_ptr<llvm::Module>* moduleOut)
    {
        llvm::TimeTraceScope scope("CxxDemandCompanion");
        if (moduleOut != nullptr) moduleOut->reset();
        (void)poisoned;
        bitcode.clear();
        stats = CxxDemandStats{};
        ASTContext& ctx = ci.getASTContext();
        std::unique_ptr<MangleContext> mangle(ctx.createMangleContext());

        std::vector<Decl*> lazy;
        std::unordered_map<std::string, GlobalDecl> held;
        size_t splitDone = 0;
        // Plan decls split into deferrable (handed wholesale) and must-emit (handed on demand).
        // Re-run as the plan grows: instantiations in the closure below are announced into it.
        auto splitPlan = [&]() {
            llvm::TimeTraceScope splitScope("CxxDemandSplit");
            auto holdName = [&](GlobalDecl gd) { held.emplace(DemandSymbolName(*mangle, gd), gd); };
            for (; splitDone < plan.decls.size(); ++splitDone)
            {
                Decl* d = plan.decls[splitDone];
                if (d == nullptr || d->isInvalidDecl()) continue;
                if (ci.hasSema() && IsNonViableConstrainedMember(ci.getSema(), d)) continue;
                const bool codeDecl = llvm::isa<FunctionDecl>(d) || llvm::isa<VarDecl>(d);
                if (!codeDecl || !ctx.DeclMustBeEmitted(d))
                {
                    lazy.push_back(d);
                    continue;
                }
                if (const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(d))
                {
                    holdName(GlobalDecl(ctor, Ctor_Complete));
                    holdName(GlobalDecl(ctor, Ctor_Base));
                }
                else if (const auto* dtor = llvm::dyn_cast<CXXDestructorDecl>(d))
                {
                    holdName(GlobalDecl(dtor, Dtor_Complete));
                    holdName(GlobalDecl(dtor, Dtor_Base));
                    if (dtor->isVirtual()) holdName(GlobalDecl(dtor, Dtor_Deleting));
                }
                else if (const auto* fd = llvm::dyn_cast<FunctionDecl>(d))
                    holdName(GlobalDecl(fd));
                else
                    holdName(GlobalDecl(llvm::cast<VarDecl>(d)));
            }
        };
        splitPlan();

        std::set<std::string> want;
        std::unordered_map<std::string, std::string> restore;   // emitted -> program name
        for (const std::string& programName : demand)
        {
            std::string name = programName;
            if (auto it = plan.renamed.find(programName); it != plan.renamed.end())
            {
                name = it->second;
                restore[name] = programName;
            }
            if (plan.bound.count(name) != 0 || held.count(name) != 0
                || plan.vtables.count(name) != 0)
                want.insert(name);
        }
        stats.demanded = (unsigned)want.size();
        if (want.empty()) return true;

        clang::DiagnosticsEngine& diagnostics = ci.getDiagnostics();
        if (plan.materializeReachable)
        {
            llvm::TimeTraceScope lateScope("CxxDemandLateBodies");
            std::string failedBody;
            std::string bodyDiagnostic;
            std::vector<const FunctionDecl*> roots;
            for (const std::string& name : want)
            {
                if (auto it = held.find(name); it != held.end())
                    roots.push_back(llvm::dyn_cast<FunctionDecl>(it->second.getDecl()));
                if (auto it = plan.bound.find(name); it != plan.bound.end())
                    roots.push_back(llvm::dyn_cast<FunctionDecl>(
                        GlobalDecl::getFromOpaquePtr(it->second).getDecl()));
            }
            plan.materializeReachable(std::move(roots), failedBody, bodyDiagnostic);
            if (!failedBody.empty())
            {
                err = "clang: failed to compile inline body '" + failedBody
                    + "' required by this program";
                if (llvm::StringRef detail = llvm::StringRef(bodyDiagnostic).trim(); !detail.empty())
                    err += ": " + detail.str();
                return false;
            }
        }
        /*
         * The Microsoft ABI's vftables are handed unconditionally below, and each one emits its
         * deleting destructor. Sema resolves that destructor's operator delete only when it
         * finishes the destructor body, which a skipped inline body never did; CodeGen would
         * dereference the null. Likewise the destructor's exception spec: Sema resolves an
         * unevaluated one only on odr-use, and CodeGen's EH-spec query on it is unreachable.
         */
        for (const auto& [name, record] : plan.vtables)
        {
            if (!name.starts_with("vftable:")) continue;
            auto* dtor = record->getDestructor();
            if (dtor == nullptr || !dtor->isVirtual() || dtor->isDeleted()) continue;
            if (const auto* fpt = dtor->getType()->getAs<clang::FunctionProtoType>();
                fpt != nullptr && clang::isUnresolvedExceptionSpec(fpt->getExceptionSpecType())
                && ci.getSema().ResolveExceptionSpec(dtor->getLocation(), fpt) == nullptr)
            {
                err = "clang could not resolve the exception specification of the virtual "
                      "destructor of '" + record->getQualifiedNameAsString() + "'";
                return false;
            }
            if (dtor->getOperatorDelete() != nullptr) continue;
            clang::Sema& sema = ci.getSema();
            clang::Sema::ContextRAII inDtor(sema, dtor);
            if (sema.CheckDestructor(dtor) || dtor->getOperatorDelete() == nullptr)
            {
                err = "clang could not resolve operator delete for the virtual destructor of '"
                    + record->getQualifiedNameAsString() + "'";
                return false;
            }
        }
        std::unordered_set<const FunctionDecl*> closureTried;
        for (unsigned round = 0; round < 16; ++round)
        {
            splitPlan();
            llvm::TimeTraceScope roundScope("CxxDemandRound", std::to_string(round));
            llvm::LLVMContext localCtx;
            llvm::LLVMContext& llvmCtx = targetContext != nullptr ? *targetContext : localCtx;
            std::unique_ptr<CodeGenerator> cg(
                clang::CreateLLVMCodeGen(ci, "cflat_cxx_demand", llvmCtx));
            if (!cg)
            {
                err = "could not create the C++ code generator";
                return false;
            }
            // Earlier chunks' recovered errors are not CodeGen errors; ModuleBuilder would
            // discard the module over them.
            diagnostics.Reset(/*soft*/ true);
            cg->Initialize(ctx);
            {
                llvm::TimeTraceScope handScope("CxxDemandHand", std::to_string(lazy.size()));
                for (Decl* d : lazy) cg->HandleTopLevelDecl(DeclGroupRef(d));
            }
            for (const std::string& name : want)
            {
                if (auto it = held.find(name); it != held.end())
                {
                    cg->HandleTopLevelDecl(DeclGroupRef(const_cast<Decl*>(it->second.getDecl())));
                    cg->GetAddrOfGlobal(it->second, /*isForDefinition*/ false);
                }
                if (auto it = plan.bound.find(name); it != plan.bound.end())
                    cg->GetAddrOfGlobal(GlobalDecl::getFromOpaquePtr(it->second),
                                        /*isForDefinition*/ false);
                if (auto it = plan.vtables.find(name); it != plan.vtables.end())
                    cg->HandleVTable(const_cast<CXXRecordDecl*>(it->second));
            }
            for (const auto& [name, record] : plan.vtables)
                if (name.starts_with("vftable:"))
                    cg->HandleVTable(const_cast<CXXRecordDecl*>(record));
            {
                llvm::TimeTraceScope flushScope("CxxDemandFlush");
                cg->HandleTranslationUnit(ctx);
            }
            llvm::Module* mod = cg->GetModule();
            if (mod == nullptr)
            {
                err = "clang reported an error while generating the C++ definitions this "
                      "program uses";
                return false;
            }
            // Held symbols the module reaches but only declares: hand them over next round.
            bool grew = false;
            for (const llvm::GlobalValue& gv : mod->global_values())
                if (gv.isDeclaration() && gv.hasName() && held.count(gv.getName().str()) != 0
                    && want.insert(gv.getName().str()).second)
                    grew = true;
            if (grew) continue;
            /*
             * ODR-use closure, as clang++ would finish the TU: a used function the module only
             * declares whose body the AST has (or can instantiate) was never handed. Hand it, or
             * instantiate it first; library symbols (no body, not instantiable) stay external.
             */
            {
                llvm::TimeTraceScope closureScope("CxxDemandClosure");
                std::vector<std::pair<std::string, FunctionDecl*>> pulled;
                for (const llvm::Function& fn : mod->functions())
                {
                    // A demanded symbol has no use in this module; its body may still be skipped.
                    if (!fn.isDeclaration() || fn.isIntrinsic() || !fn.hasName()
                        || (fn.use_empty() && want.count(fn.getName().str()) == 0))
                        continue;
                    const auto* found = llvm::dyn_cast_or_null<FunctionDecl>(
                        cg->GetDeclForMangledName(fn.getName()));
                    if (found == nullptr || !closureTried.insert(found).second) continue;
                    pulled.emplace_back(fn.getName().str(), const_cast<FunctionDecl*>(found));
                }
                clang::Sema& sema = ci.getSema();
                for (auto& [name, fd] : pulled)
                {
                    if (fd->isInvalidDecl() || fd->isDependentContext()) continue;
                    const FunctionDecl* definition = nullptr;
                    if (!fd->isDefined(definition)
                        && fd->getTemplateSpecializationKindForInstantiation()
                               == TSK_ImplicitInstantiation
                        && fd->isImplicitlyInstantiable())
                    {
                        sema.InstantiateFunctionDefinition(fd->getLocation(), fd,
                                                           /*Recursive*/ true,
                                                           /*DefinitionRequired*/ false,
                                                           /*AtEndOfTU*/ true);
                        sema.PerformPendingInstantiations();
                    }
                    if (!fd->isDefined(definition) || definition->isInvalidDecl()) continue;
                    FunctionDecl* body = const_cast<FunctionDecl*>(definition);
                    // The group's header parse deferred this body; parse it now (see LazyBodies).
                    const bool materialized = body->hasSkippedBody();
                    if (materialized
                        && (!plan.materializeBody || !plan.materializeBody(body)))
                        continue;
                    if (plan.declSeen.count(body) == 0 && plan.declSeen.count(fd) == 0)
                    {
                        plan.Add(body);
                        want.insert(name);
                        grew = true;
                    }
                    else if (materialized || splitDone < plan.decls.size())
                        grew = true;
                }
                // Same for storage: a variable a late-parsed body names (libc++'s inline
                // __digits_base_10) is known only once that body exists.
                for (const llvm::GlobalVariable& gv : mod->globals())
                {
                    if (!gv.hasName() || gv.use_empty()) continue;
                    const auto* var = llvm::dyn_cast_or_null<VarDecl>(
                        cg->GetDeclForMangledName(gv.getName()));
                    const VarDecl* definition = var != nullptr ? var->getDefinition() : nullptr;
                    if (definition == nullptr || definition->isInvalidDecl()) continue;
                    // A static reached only from a late-parsed inline body was not visited by
                    // HarvestCxxNamespaceVar; still give its emitted storage this import group's name.
                    if (var->getDeclContext()->isFileContext()
                        && !var->hasExternalFormalLinkage())
                    {
                        const std::string symbol = gv.getName().str();
                        if (std::find(plan.weakPromoteSymbols.begin(),
                                      plan.weakPromoteSymbols.end(), symbol)
                            == plan.weakPromoteSymbols.end())
                            plan.weakPromoteSymbols.push_back(symbol);
                        if (std::find(plan.weakPromotePerGroupSymbols.begin(),
                                      plan.weakPromotePerGroupSymbols.end(), symbol)
                            == plan.weakPromotePerGroupSymbols.end())
                            plan.weakPromotePerGroupSymbols.push_back(symbol);
                    }
                    if (!gv.isDeclaration() || plan.declSeen.count(definition) != 0) continue;
                    plan.Add(const_cast<VarDecl*>(definition));
                    grew = true;
                }
                if (splitDone < plan.decls.size()) grew = true;
            }
            if (grew) continue;

            for (const std::string& sym : plan.weakPromoteSymbols)
            {
                llvm::GlobalVariable* gv = mod->getGlobalVariable(sym, /*AllowInternal*/ true);
                if (gv == nullptr || gv->isDeclaration()) continue;
                const bool perGroup = std::find(plan.weakPromotePerGroupSymbols.begin(),
                              plan.weakPromotePerGroupSymbols.end(), sym)
                    != plan.weakPromotePerGroupSymbols.end();
                if (!gv->hasLocalLinkage() && !perGroup) continue;
                if (perGroup)
                    RenamePerGroupGlobal(gv, StaticCxxGlobalAlias(plan.cxxImportGroupKey, sym));
                if (gv->hasLocalLinkage())
                    gv->setLinkage(llvm::GlobalValue::WeakODRLinkage);
                gv->setVisibility(llvm::GlobalValue::DefaultVisibility);
            }
            for (const llvm::GlobalValue& gv : mod->global_values())
                if (!gv.isDeclaration()) ++stats.definitions;
            for (const std::string& name : want)
                if (const llvm::GlobalValue* gv = mod->getNamedValue(
                        std::find(plan.weakPromotePerGroupSymbols.begin(),
                                  plan.weakPromotePerGroupSymbols.end(), name)
                                != plan.weakPromotePerGroupSymbols.end()
                            ? StaticCxxGlobalAlias(plan.cxxImportGroupKey, name) : name);
                    gv == nullptr || gv->isDeclaration())
                {
                    ++stats.unresolved;
                    if (verbose)
                        std::cout << "[verbose]   C++ demand pass: '" << name
                                  << "' is still only declared\n";
                }
            for (const auto& [emitted, programName] : restore)
                if (llvm::GlobalValue* gv = mod->getNamedValue(emitted);
                    gv != nullptr && mod->getNamedValue(programName) == nullptr)
                {
                    // A static inline stays internal here; the program binds a thunk instead.
                    if (auto* fn = llvm::dyn_cast<llvm::Function>(gv); fn != nullptr
                        && !fn->isDeclaration() && fn->hasLocalLinkage())
                        AddStaticInlineProgramThunk(*mod, fn, programName);
                    else
                        gv->setName(programName);
                }
            {
                llvm::TimeTraceScope serializeScope("CxxDemandSerialize");
                llvm::raw_string_ostream os(bitcode);
                llvm::WriteBitcodeToFile(*mod, os);
                os.flush();
            }
            if (moduleOut != nullptr && targetContext != nullptr)
                *moduleOut = cg->ReleaseModule();
            if (verbose)
                std::cout << std::format("[verbose] C++ demand pass: {} demanded symbol(s), {} "
                                         "round(s), {} definition(s), {} recorded decl(s)\n",
                                         stats.demanded, round + 1, stats.definitions,
                                         plan.decls.size());
            diagnostics.Reset(/*soft*/ true);
            return true;
        }
        err = "the C++ definitions this program uses did not converge";
        return false;
    }
}
