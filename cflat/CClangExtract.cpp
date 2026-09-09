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
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/AST/BaseSubobject.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
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
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ADT/SmallString.h"

#include "llvm/Support/TimeProfiler.h"

#include <set>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <unordered_set>
#include <format>

namespace cflat_cinterop
{
    using namespace clang;

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
        const char kProbePrefix[] = "__cflat_macro_";

        std::string CanonicalSpelling(const ASTContext& ctx, QualType qt)
        {
            QualType canonical = qt.getCanonicalType();
            std::string s = canonical.getAsString(ctx.getPrintingPolicy());
            // C++ prints an enum type as a bare name ("cppi::Mode"), where C prints "enum X".
            // The type mapper keys the int decay on the tag, so restore it.
            if (canonical->getAs<EnumType>() != nullptr && s.rfind("enum ", 0) != 0
                && s.rfind("const enum ", 0) != 0)
                s.insert(s.rfind("const ", 0) == 0 ? 6 : 0, "enum ");
            return s;
        }

        std::string CxxQualifiedName(const NamedDecl* d)
        {
            std::string n = d->getQualifiedNameAsString();
            std::replace(n.begin(), n.end(), ':', '.');
            while (n.find("..") != std::string::npos) n.erase(n.find(".."), 1);
            return n;
        }

        std::string CxxForeignIdentity(std::string spelling)
        {
            for (const char* tag : { "class ", "struct ", "union ", "enum " })
                if (spelling.starts_with(tag)) { spelling.erase(0, std::strlen(tag)); break; }
            std::string out;
            for (size_t i = 0; i < spelling.size(); ++i)
            {
                if (spelling[i] == ':' && i + 1 < spelling.size() && spelling[i + 1] == ':')
                { out += '.'; ++i; continue; }
                if (spelling[i] == '<' || spelling[i] == ',') { out += '$'; continue; }
                if (spelling[i] == '>') continue;
                if (std::isspace((unsigned char)spelling[i])) continue;
                if (std::isalnum((unsigned char)spelling[i]) || spelling[i] == '_'
                    || spelling[i] == '.' || spelling[i] == '$')
                    out += spelling[i];
                else if (spelling[i] == '*') out += "ptr";
                else if (spelling[i] == '&') out += "ref";
                else out += '_';
            }
            return out;
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

        RawDefaultArg DefaultArgumentOf(const ParmVarDecl* p, ASTContext& ctx)
        {
            RawDefaultArg result;
            if (p == nullptr || !p->hasDefaultArg()) return result;
            // A template member default is not instantiated until used: never a constant here.
            if (p->hasUninstantiatedDefaultArg()) { result.kind = "nonconst"; return result; }
            const Expr* init = p->getDefaultArg();
            if (init == nullptr || init->containsErrors() || init->isValueDependent())
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

        std::string CxxLinkageName(ASTContext& ctx, const VarDecl* vd)
        {
            auto mangle = std::unique_ptr<MangleContext>(ctx.createMangleContext());
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

        // APSInt -> long long. Signed values sign-extend (they always fit in int64); unsigned
        // values may exceed INT64_MAX (e.g. ~0ULL), so zero-extend and bit-reinterpret rather
        // than call getSExtValue, which asserts isRepresentableByInt64 for those.
        long long ApsIntToLongLong(const llvm::APSInt& v)
        {
            return v.isSigned() ? v.getSExtValue() : static_cast<long long>(v.getZExtValue());
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

        // One object-like macro discovered in the prepass, awaiting value/type deduction by its
        // injected probe. Its index is its probe slot (probe var `__cflat_macro_<index>`).
        struct MacroProbe
        {
            std::string name;
            std::string file;
            std::string aliasTarget;   // body is exactly one identifier token (`#define A B`)
            int line = 1;
            int col = 0;
        };

        class PrereqDiagConsumer : public DiagnosticConsumer
        {
        public:
            unsigned prereqErrors = 0;
            std::string firstPrereqError;
            std::string firstError;
            // Every error with its presumed location, so a record whose definition failed to
            // compile can name the clang diagnostic that broke it (capped: a broken TU cascades).
            struct ErrorNote { std::string message; std::string file; unsigned line = 0; };
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
                    ErrorNote note{ msg.str().str(), std::string(), 0 };
                    if (info.hasSourceManager() && info.getLocation().isValid())
                    {
                        PresumedLoc pl = info.getSourceManager().getPresumedLoc(info.getLocation());
                        if (pl.isValid()) { note.file = pl.getFilename(); note.line = pl.getLine(); }
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
            std::vector<MacroProbe> probes;   // index == probe slot
            std::unordered_set<unsigned> emittedProbes;  // probe slots that produced a RawMacro
            std::unordered_set<std::string> emittedGlobals;  // dedup global var redeclarations by name
            std::unordered_set<std::string> emittedOpaqueForward;  // dedup opaque forward-decl records by tag
            std::unordered_set<const RecordDecl*> emittedDefinedRecords;
            std::unordered_set<std::string> emittedRequestedRecords;
            std::vector<std::string> normDirs; // req.inScopeDirs normalized once (NormPath + trailing-/ stripped)
            // Set in BeginSourceFileAction so the ABI pass can build a CodeGenerator against the
            // very invocation that produced the AST (same triple, same target features).
            CompilerInstance* ci = nullptr;
            // cxxMode only: (index into out.sigs, the decl it came from). Resolved after the
            // traversal so a single CodeGenerator serves every declaration.
            std::vector<std::pair<size_t, const FunctionDecl*>> abiWork;
            std::vector<QualType> functionPointerAbiWork;
            std::unordered_set<std::string> functionPointerAbiSeen;
            std::unordered_set<const FunctionTemplateDecl*> emittedFunctionTemplates;
            // cxxMode only: (index into out.records, index into that record's members, decl).
            struct MemberAbiWork { size_t recordIdx; size_t memberIdx; const CXXMethodDecl* md; };
            std::vector<MemberAbiWork> memberAbiWork;
            // Plain C++ header records whose implicit special members must be materialized before
            // CollectCxxMembers walks the record's methods.
            std::vector<const CXXRecordDecl*> headerSpecialMemberWork;
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
            // emitDefinitions only: polymorphic classes whose vtable/RTTI Clang must emit, and
            // inline / constexpr static data members whose storage lives in the companion module.
            std::vector<const CXXRecordDecl*> vtableWork;
            std::vector<const VarDecl*> varEmitWork;
            std::vector<std::string> incompleteCxxTypes;
            ExtractState(const ExtractRequest& r, ExtractResult& o) : req(r), out(o)
            {
                for (const auto& d : r.inScopeDirs)
                {
                    std::string nd = NormPath(d);
                    while (!nd.empty() && nd.back() == '/') nd.pop_back();
                    normDirs.push_back(std::move(nd));
                }
            }
        };

        void QueueIncompleteCxxType(ExtractState& st, ASTContext& ctx, QualType qt)
        {
            if (!st.req.cxxMode) return;
            // Stage 2 already emits the requested ODR-uses; replaying explicit instantiation for
            // private libc++ helper records can make assertion-enabled Clang see a placeholder.
            if (st.req.emitDefinitions && !st.req.cxxTypeRequests.empty()) return;
            qt = qt.getCanonicalType();
            if (!qt->isRecordType() || !qt->isIncompleteType()) return;
            const auto* cxx = qt->getAsCXXRecordDecl();
            if (cxx == nullptr || !llvm::isa<ClassTemplateSpecializationDecl>(cxx)) return;
            std::string spelling = CanonicalSpelling(ctx, qt);
            if (std::find(st.incompleteCxxTypes.begin(), st.incompleteCxxTypes.end(), spelling)
                    == st.incompleteCxxTypes.end())
                st.incompleteCxxTypes.push_back(std::move(spelling));
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
            ExtractState& st;
            MacroCollector(Preprocessor& p, ExtractState& s) : pp(p), st(s) {}

            void MacroDefined(const Token& nameTok, const MacroDirective* md) override
            {
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
                if (st.req.requireInScope && !PathInScope(file, st.normDirs)) return;

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

                MacroProbe mp;
                mp.name = name; mp.file = file; mp.line = line; mp.col = col;
                mp.aliasTarget = aliasTarget;
                st.probes.push_back(std::move(mp));
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
            return t.getAsString(ctx.getPrintingPolicy());
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

        struct DeclVisitor : public RecursiveASTVisitor<DeclVisitor>
        {
            ASTContext& ctx;
            SourceManager& sm;
            ExtractState& st;

            DeclVisitor(ASTContext& c, ExtractState& s) : ctx(c), sm(c.getSourceManager()), st(s) {}

            void PrepareHeaderSpecialMembers(const CXXRecordDecl* cxx);
            std::string InvalidDefinitionRefusal(const CXXRecordDecl* def) const;

            // Resolve a decl's presumed location WITHOUT applying the in-scope filter. Returns
            // false only on an invalid/unknown location.
            bool LocOfRaw(const Decl* d, std::string& file, int& line, int& col) const
            {
                SourceLocation loc = d->getLocation();
                if (loc.isInvalid()) return false;
                // Macro-generated declarations (for example ATen's Tensor operators) have an
                // expansion location in the including stub; bind their spelling header instead.
                if (loc.isMacroID()) loc = sm.getSpellingLoc(loc);
                PresumedLoc pl = sm.getPresumedLoc(loc);
                if (pl.isInvalid()) return false;
                file = pl.getFilename() ? pl.getFilename() : "";
                line = (int)pl.getLine();
                col = (int)pl.getColumn();
                return true;
            }

            // Location plus the in-scope gate: used by functions/enums/globals where an
            // out-of-scope decl must be dropped outright (not part of the bound API surface).
            bool LocOf(const Decl* d, std::string& file, int& line, int& col) const
            {
                if (!LocOfRaw(d, file, line, col)) return false;
                if (st.req.requireInScope && !PathInScope(file, st.normDirs)) return false;
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
                if (fd == nullptr || !fd->getIdentifier() || fd->isVariadic()
                    || !fd->hasExternalFormalLinkage())
                    return true;
                const auto* md = llvm::dyn_cast<CXXMethodDecl>(fd);
                if (md != nullptr && md->getAccess() != AS_public) return true;
                if (md == nullptr && fd->getStorageClass() == SC_Static) return true;

                unsigned typeParameterCount = 0;
                for (const NamedDecl* tp : *ftd->getTemplateParameters())
                {
                    const auto* typeParam = llvm::dyn_cast<TemplateTypeParmDecl>(tp);
                    if (typeParam != nullptr)
                    {
                        if (typeParam->isParameterPack()) return true;
                        ++typeParameterCount;
                        continue;
                    }
                    // SFINAE helpers such as fmt::to_string's enable_if are non-type
                    // parameters with a default value. They need no explicit CFlat argument.
                    const auto* nonTypeParam = llvm::dyn_cast<NonTypeTemplateParmDecl>(tp);
                    if (nonTypeParam == nullptr || !nonTypeParam->hasDefaultArgument()) return true;
                }

                std::string file;
                int line = 1, col = 0;
                if (!LocOf(fd, file, line, col)) return true;
                RawFunctionTemplate result;
                result.kind = md == nullptr ? RawFunctionTemplate::Free
                    : md->isStatic() ? RawFunctionTemplate::StaticMember
                                     : RawFunctionTemplate::InstanceMember;
                result.name = CxxQualifiedName(fd);
                if (!IsValidDottedName(result.name)) return true;
                if (md != nullptr)
                {
                    result.owner = CxxQualifiedName(md->getParent());
                    result.memberName = fd->getNameAsString();
                }
                result.cxxSpelling = "::" + result.name;
                for (size_t pos = 0; (pos = result.cxxSpelling.find('.', pos)) != std::string::npos; )
                {
                    result.cxxSpelling.replace(pos, 1, "::");
                    pos += 2;
                }
                result.minArity = (unsigned)fd->getNumParams();
                result.maxArity = result.minArity;
                while (result.minArity > 0
                       && fd->getParamDecl(result.minArity - 1)->hasDefaultArg())
                    --result.minArity;
                result.typeParameterCount = typeParameterCount;
                result.isConst = md != nullptr && !md->isStatic() && md->isConst();
                result.isNoexcept = DeclIsNoexcept(fd);
                result.access = md == nullptr ? AccessPublic : MapAccess(md->getAccess());
                result.file = file;
                result.line = line;
                result.col = col;
                st.out.functionTemplates.push_back(std::move(result));
                return true;
            }

            bool VisitFunctionDecl(FunctionDecl* fd)
            {
                // A generated `auto` wrapper whose body failed template deduction keeps an
                // undeduced return type in the recovered AST. It is useful for the diagnostic,
                // but CodeGen's ABI arranger cannot inspect it safely.
                if (fd == nullptr || fd->isInvalidDecl() || fd->getReturnType()->isUndeducedType())
                    return true;
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
                // Not externally linkable. The linkage test also catches the out-of-line
                // `inline` REDECLARATION of a `static inline` header function (simdjson's logger).
                if (fd->getStorageClass() == SC_Static || !fd->hasExternalFormalLinkage()) return true;
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
                sig.isNoexcept = !st.req.cxxMode
                    || fd->getExceptionSpecType() == EST_BasicNoexcept
                    || fd->getExceptionSpecType() == EST_NoexceptTrue
                    || fd->getExceptionSpecType() == EST_NoThrow;
                sig.file = file; sig.line = line; sig.col = col;
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
                if (st.req.cxxMode && !fd->isVariadic())
                    st.abiWork.emplace_back(st.out.sigs.size(), fd);
                st.out.sigs.push_back(std::move(sig));
                return true;
            }

            bool VisitEnumConstantDecl(EnumConstantDecl* ec)
            {
                if (!ec->getIdentifier()) return true;
                std::string file; int line = 1, col = 0;
                if (!LocOf(ec, file, line, col)) return true;

                RawEnum e;
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
                    }
                }
                e.value = ApsIntToLongLong(ec->getInitVal());
                e.file = file; e.line = line; e.col = col;
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
                            // A synthetic anonymous member is never independently in-scope; it is
                            // pulled into the kept set only when its enclosing record is (via the
                            // "struct <tag>__anon<N>" field reference the closure walk follows).
                            nested.inScope = false;
                            std::string nf; int nl = 1, nc = 0;
                            if (LocOfRaw(anon, nf, nl, nc)) { nested.file = nf; nested.line = nl; nested.col = nc; }
                            CollectFields(anon, synTag, nested);
                            st.out.records.push_back(std::move(nested));

                            RawField fe;
                            fe.name = "__anon" + std::to_string(idx);
                            fe.ctype = (isUnion ? "union " : "struct ") + synTag;
                            fe.offsetBytes = layout.getFieldOffset(f->getFieldIndex()) / 8;
                            RecordRawFieldLayout(f->getType(), fe);
                            rec.fields.push_back(std::move(fe));
                        }
                        continue;  // unnamed non-bitfield non-anon: nothing to record
                    }

                    RawField rf;
                    rf.name = f->getNameAsString();
                    rf.access = MapAccess(f->getAccess());
                    rf.offsetBytes = layout.getFieldOffset(f->getFieldIndex()) / 8;
                    RecordRawFieldLayout(f->getType(), rf);
                    if (st.req.cxxMode && f->getType()->isReferenceType())
                    {
                        rec.layoutRefusal = std::format(
                            "field '{}' of '{}' is a C++ reference; reference members are not supported",
                            rf.name, tag);
                    }

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
                        && !nrd->isAnonymousStructOrUnion())
                    {
                        const int idx = anonIdx++;
                        const std::string synTag = tag + "__anon" + std::to_string(idx);
                        const bool isUnion = nrd->isUnion();

                        RawRecord nested;
                        nested.name = synTag;
                        nested.isUnion = isUnion;
                        nested.inScope = false;
                        std::string nf; int nl = 1, nc = 0;
                        if (LocOfRaw(nrd, nf, nl, nc)) { nested.file = nf; nested.line = nl; nested.col = nc; }
                        CollectFields(nrd, synTag, nested);
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
            void CollectCxxMembers(const CXXRecordDecl* cxx, RawRecord& rec,
                                   std::vector<const CXXMethodDecl*>& outDecls)
            {
                rec.isPolymorphic = cxx->isPolymorphic() || cxx->getNumVBases() > 0;
                // A polymorphic class needs a vtable. Clang decides whether this translation unit
                // owns it (all-inline: linkonce_odr here) or whether a key function anchors it in
                // the bound library (external declaration); asking is always safe.
                if (st.req.emitDefinitions && cxx->isPolymorphic() && cxx->getNumVBases() == 0)
                    st.vtableWork.push_back(cxx);
                rec.hasBases = cxx->getNumBases() > 0 || cxx->getNumVBases() > 0;
                rec.hasVirtualBases = cxx->getNumVBases() > 0;
                rec.isAbstract = cxx->isAbstract();
                rec.hasTrivialDefaultCtor = cxx->hasTrivialDefaultConstructor();
                rec.hasTrivialCopyCtor = cxx->hasTrivialCopyConstructor();
                rec.hasTrivialDtor = !cxx->hasNonTrivialDestructor();
                rec.paramDestroyedInCallee = cxx->isParamDestroyedInCallee();
                rec.hasDefaultCtor = cxx->hasDefaultConstructor();
                rec.hasCopyCtor = cxx->hasCopyConstructorWithConstParam()
                               || cxx->needsImplicitCopyConstructor()
                               || cxx->hasUserDeclaredCopyConstructor();
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
                        if (st.req.emitDefinitions)
                        {
                            for (const FunctionDecl* spec : ftd->specializations())
                            {
                                const auto* smd = llvm::dyn_cast<CXXMethodDecl>(spec);
                                if (smd == nullptr || !smd->hasBody()) continue;
                                templateExtras.insert(smd);
                                methodList.push_back(smd);
                            }
                        }
                        if (!st.req.emitDefinitions && (!allDefaulted && !constructorTemplate))
                            continue;
                        if (!st.req.emitDefinitions && dependent)
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
                            if (smd == nullptr) continue;
                            templateExtras.insert(smd);
                            if (listedMethods.insert(smd).second) methodList.push_back(smd);
                            continue;
                        }
                        if (!st.req.emitDefinitions)
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
                    const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md);
                    const auto* dtor = llvm::dyn_cast<CXXDestructorDecl>(md);
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
                        default: break;
                    }
                    /*
                     * Every conversion function is exported, explicit or not: CFlat binds them
                     * ONLY at an explicit cast `(T)obj`, which is what C++ `explicit` already
                     * means, so the two spellings need no distinction here. The registration side
                     * renames the member to "operator <CFlat spelling>" and refuses a target the
                     * type map cannot express.
                     */
                    const auto* conversion = llvm::dyn_cast<CXXConversionDecl>(md);
                    const bool isBindableBoolConversion = conversion != nullptr
                        && conversion->getConversionType().getCanonicalType()->isBooleanType();
                    if (conversion != nullptr) isBindableOperator = true;
                    if (ctor == nullptr && dtor == nullptr && md->getIdentifier() == nullptr
                        && !isAssignSpecial && !isBindableOperator)
                        continue;

                    RawCxxMember m;
                    if (ctor != nullptr)
                    {
                        m.kind = RawCxxMember::Constructor;
                        m.name = "__ctor";
                        m.isDefaultCtor = ctor->isDefaultConstructor();
                        m.isCopyCtor = ctor->isCopyConstructor();
                        m.isMoveCtor = ctor->isMoveConstructor();
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
                        m.name = isBindableBoolConversion ? "operator bool" : md->getNameAsString();
                        m.isCopyAssign = md->isCopyAssignmentOperator();
                        m.isMoveAssign = md->isMoveAssignmentOperator();
                        if (m.name.empty()) m.name = "operator=";
                    }
                    m.isConst = !md->isStatic() && md->isConst();
                    m.isVirtual = md->isVirtual();
                    m.isPureVirtual = md->isPureVirtual();
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
                    m.needsLocalDefinition = md->isImplicit() || md->isDefaulted()
                                          || md->isInlined();
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
                    // Refuse before any arrangement: an incomplete by-value type has no layout.
                    for (const ParmVarDecl* p : md->parameters())
                    {
                        std::string bad = IncompleteByValueRecord(ctx, p->getType());
                        if (bad.empty()) continue;
                        m.bindRefusal = "takes '" + bad + "' by value, whose definition this "
                                        "translation unit does not have (include the header that "
                                        "defines it alongside this one)";
                        break;
                    }
                    if (m.bindRefusal.empty() && md->getReturnType()->isUndeducedType())
                        m.bindRefusal = "has a deduced return type ('auto') that this translation "
                                        "unit never deduced (its body was not instantiated)";
                    if (m.bindRefusal.empty() && ctor == nullptr && dtor == nullptr)
                    {
                        std::string bad = IncompleteByValueRecord(ctx, md->getReturnType());
                        if (!bad.empty())
                            m.bindRefusal = "returns '" + bad + "' by value, whose definition this "
                                            "translation unit does not have (include the header "
                                            "that defines it alongside this one)";
                    }

                    // With definition emission on, an inline / defaulted / implicit member is a
                    // CANDIDATE: the linkage name and the ABI arrangement are produced here, and
                    // the emission pass clears needsLocalDefinition only for the ones Clang really
                    // emitted. Without it the member stays declaration-only, as before.
                    const bool emitCandidate = st.req.emitDefinitions
                                            || st.req.assumeInlineDefinitions;
                    // LSP has no CodeGen module, so assume every header-defined member has a
                    // callable declaration. A real compile still proves the body below.
                    if (st.req.assumeInlineDefinitions && !st.req.emitDefinitions
                        && (md->isInlined() || md->isImplicit() || md->isDefaulted()))
                        m.needsLocalDefinition = false;
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

                for (const Decl* d : cxx->decls())
                {
                    const auto* vd = llvm::dyn_cast<VarDecl>(d);
                    if (vd == nullptr || !vd->isStaticDataMember()) continue;
                    if (vd->getIdentifier() == nullptr) continue;
                    if (vd->isConstexpr() && vd->getInit() != nullptr)
                    {
                        Expr::EvalResult result;
                        if (vd->getInit()->EvaluateAsInt(result, ctx) && result.Val.isInt())
                        {
                            RawCxxStaticVar sv;
                            sv.name = vd->getNameAsString();
                            sv.ctype = CanonicalSpelling(ctx, vd->getType());
                            sv.isCompileTimeConstant = true;
                            sv.constantValue = ApsIntToLongLong(result.Val.getInt());
                            sv.access = MapAccess(vd->getAccess());
                            LocOfRaw(vd, sv.file, sv.line, sv.col);
                            rec.staticVars.push_back(std::move(sv));
                            continue;
                        }
                    }
                    // An inline / constexpr static member is emitted per-TU on demand, so the
                    // bound library need not contain it. Only an out-of-line definition is a
                    // symbol CFlat may read.
                    // With definition emission on the storage is emitted into the companion module
                    // (linkonce_odr, so several importers merge), which makes it a real symbol.
                    const bool emitLocal =
                        (st.req.emitDefinitions || st.req.assumeInlineDefinitions)
                        && (vd->isConstexpr() || vd->isInline())
                        && vd->getDefinition() != nullptr;
                    // Why a static data member was left out is invisible at the use site
                    // ("'count' does not name a value"), so name the reason under -v.
                    auto skipStaticVar = [&](const char* why) {
                        if (st.req.verbose)
                            std::cout << "[verbose]   C++ static member "
                                      << vd->getQualifiedNameAsString()
                                      << " not bound: " << why << "\n";
                    };
                    if (!emitLocal && (vd->isConstexpr() || vd->isInline()))
                    { skipStaticVar("inline or constexpr storage is emitted per TU"); continue; }
                    if (!emitLocal && vd->hasInit())
                    { skipStaticVar("its initializer lives in the header, so it has no library symbol"); continue; }
                    if (emitLocal && st.req.emitDefinitions)
                        st.varEmitWork.push_back(vd->getDefinition());
                    RawCxxStaticVar sv;
                    sv.name = vd->getNameAsString();
                    sv.ctype = CanonicalSpelling(ctx, vd->getType());
                    sv.access = MapAccess(vd->getAccess());
                    sv.linkageName = CxxLinkageName(ctx, vd);
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
                    if (!PathInScope(ofile, st.normDirs)) return true;
                    std::string tag = rd->getNameAsString();
                    if (!st.emittedOpaqueForward.insert(tag).second) return true;
                    RawRecord rec;
                    rec.name = std::move(tag);
                    rec.isUnion = rd->isUnion();
                    rec.file = ofile; rec.line = oline; rec.col = ocol;
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
                    && llvm::isa<CXXRecordDecl>(rd) && !PathInScope(file, st.normDirs)) return;
                if (nameOverride.empty()
                    && !st.emittedDefinedRecords.insert(rd).second)
                    return;
                if (!nameOverride.empty()
                    && !st.emittedRequestedRecords.insert(nameOverride).second)
                    return;

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
                    rec.qualifiedName = CxxQualifiedName(rd);
                    if (!IsValidDottedName(rec.qualifiedName)) return;
                    rec.name = rec.qualifiedName;
                }
                rec.isUnion = rd->isUnion();
                rec.isCxx = st.req.cxxMode;
                rec.file = file; rec.line = line; rec.col = col;
                rec.inScope = forcedBase || !nameOverride.empty() || !st.req.requireInScope
                           || PathInScope(file, st.normDirs);
                if (st.req.cxxMode && llvm::isa<CXXRecordDecl>(rd))
                    rec.canonicalCtype = CanonicalSpelling(ctx, ctx.getCanonicalTagType(rd));
                const ASTRecordLayout& layout = ctx.getASTRecordLayout(rd);
                rec.sizeBytes = layout.getSize().getQuantity();
                rec.alignBytes = layout.getAlignment().getQuantity();
                rec.isPacked = rd->hasAttr<PackedAttr>();
                std::vector<const CXXMethodDecl*> memberDecls;
                bool flattened = false;
                if (const auto* cxx = llvm::dyn_cast<CXXRecordDecl>(rd))
                {
                    rec.isTrivial = cxx->isTrivial();
                    rec.isTriviallyCopyable = cxx->isTriviallyCopyable()
                        && !cxx->hasNonTrivialDestructor() && !cxx->isPolymorphic();
                    if (cxx->hasDefinition())
                    {
                        if (cxx->getDefinition()->isInvalidDecl())
                            rec.layoutRefusal = InvalidDefinitionRefusal(cxx->getDefinition());
                        if (rec.inScope && nameOverride.empty())
                            PrepareHeaderSpecialMembers(cxx);
                        if (rec.inScope || !st.req.requireInScope || !nameOverride.empty())
                            CollectCxxMembers(cxx, rec, memberDecls);
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
                if (!nameOverride.empty() && !pairValue)
                {
                    rec.fields.clear();
                    rec.layoutRefusal.clear();
                    EmitBlobStorage(rec);
                }
                else if (!flattened) CollectFields(rd, rec.name, rec);
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
                                EmitDefinedRecord(def, std::string(), true);
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
            void ProcessTypeRequests()
            {
                for (size_t i = 0; i < st.req.cxxTypeRequests.size(); ++i)
                {
                    const std::string marker = "__cflat_req_" + std::to_string(i);
                    const TypedefNameDecl* td = nullptr;
                    for (Decl* d : ctx.getTranslationUnitDecl()->decls())
                    {
                        auto* cand = llvm::dyn_cast<TypedefNameDecl>(d);
                        if (cand != nullptr && cand->getNameAsString() == marker) { td = cand; break; }
                    }
                    if (td == nullptr) continue;
                    QualType canon = td->getUnderlyingType().getCanonicalType();
                    auto* cxx = canon->getAsCXXRecordDecl();
                    if (cxx == nullptr) continue;
                    CXXRecordDecl* def = cxx->getDefinition();
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
                    std::unordered_set<const FunctionDecl*> seenOperators;
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
                        switch (fd->getOverloadedOperator())
                        {
                            case OO_EqualEqual: case OO_ExclaimEqual:
                            case OO_Less: case OO_Greater:
                            case OO_LessEqual: case OO_GreaterEqual:
                                break;
                            default: return;
                        }
                        bool matches = false;
                        for (const ParmVarDecl* p : fd->parameters())
                            matches = matches || mentionsRequested(p->getType());
                        if (matches) VisitFunctionDecl(const_cast<FunctionDecl*>(fd));
                    };
                    std::function<void(Decl*)> walkOperators;
                    walkOperators = [&](Decl* decl) {
                        collectOperator(decl);
                        if (llvm::isa<FunctionDecl>(decl)) return;
                        if (auto* dc = llvm::dyn_cast<DeclContext>(decl))
                            for (Decl* child : dc->decls()) walkOperators(child);
                    };
                    walkOperators(ctx.getTranslationUnitDecl());
                    for (Decl* d : st.announcedDecls) collectOperator(d);
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
            }

            void ProcessFunctionRequests()
            {
                std::function<void(Decl*)> walk = [&](Decl* decl) {
                    if (decl == nullptr) return;
                    if (llvm::isa<FunctionDecl>(decl))
                    {
                        VisitFunctionDecl(llvm::cast<FunctionDecl>(decl));
                        return;
                    }
                    if (auto* dc = llvm::dyn_cast<DeclContext>(decl))
                        for (Decl* child : dc->decls()) walk(child);
                };
                walk(ctx.getTranslationUnitDecl());
            }

            bool VisitTypedefNameDecl(TypedefNameDecl* td)
            {
                if (!td->getIdentifier()) return true;
                if (auto* alias = llvm::dyn_cast<TypeAliasDecl>(td);
                    alias != nullptr && alias->getDescribedAliasTemplate() != nullptr)
                    return true;
                std::string name = td->getNameAsString();
                QualType u = td->getUnderlyingType();
                std::string sugared = u.getAsString(ctx.getPrintingPolicy());
                std::string canon = u.getCanonicalType().getAsString(ctx.getPrintingPolicy());
                // Prefer the canonical underlying (chases HANDLE -> void *); but for the
                // `typedef enum {} X;` self-referential shape clang canonicalizes to the typedef
                // name itself ("ML_Mode"), so fall back to the sugared spelling ("enum ML_Mode")
                // which the mapper strips to int. Mirrors the old CollectCTypedefsLibclang.
                RawTypedef t;
                t.name = name;
                t.qualifiedName = st.req.cxxMode ? CxxQualifiedName(td) : name;
                LocOfRaw(td, t.file, t.line, t.col);
                if (st.req.requireInScope && !PathInScope(t.file, st.normDirs)) return true;
                if (const RecordType* rt = u->getAs<RecordType>())
                {
                    const RecordDecl* rd = rt->getDecl();
                    t.isAnonymousRecord = rd->isStruct() && !rd->getIdentifier()
                                       && rd->getTypedefNameForAnonDecl() == td;
                }
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
                st.out.typedefs.push_back(std::move(t));
                QueueFunctionPointerAbi(st, ctx, u);
                return true;
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
                if (st.req.requireInScope && !PathInScope(t.file, st.normDirs)) return true;
                t.isCxxAliasTemplate = true;
                t.cxxAliasPattern = alias->getUnderlyingType().getAsString(ctx.getPrintingPolicy());
                t.underlying = t.cxxAliasPattern;
                for (const NamedDecl* param : *atd->getTemplateParameters())
                    if (const auto* typeParam = llvm::dyn_cast<TemplateTypeParmDecl>(param))
                        t.cxxAliasParams.push_back(typeParam->getNameAsString());
                st.out.typedefs.push_back(std::move(t));
                return true;
            }

            // Harvest an externally-linkable file-scope global variable - a header `extern int x;`
            // declaration or a .c-defined `int x = 5;`. Skips statics (internal linkage), locals,
            // and (in definitionsOnly / .c mode) pure declarations with no definition in this TU.
            // Dedups redeclarations by name. Returns true (continue traversal) unconditionally.
            bool HarvestGlobalVar(VarDecl* vd)
            {
                if (!vd->isFileVarDecl()) return true;            // locals, params, members
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
                if (!nm.starts_with(kProbePrefix)) return HarvestGlobalVar(vd);
                unsigned idx = 0;
                if (nm.drop_front(sizeof(kProbePrefix) - 1).getAsInteger(10, idx)) return true;
                if (idx >= st.probes.size()) return true;
                const MacroProbe& mp = st.probes[idx];
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
                                 clang::CodeGen::CodeGenModule& cgm, CodeGenerator& cg);
        void EmitCxxDefinitions(ExtractState& st, ASTContext& ctx, CodeGenerator& cg);

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

        void DeclVisitor::PrepareHeaderSpecialMembers(const CXXRecordDecl* cxx)
        {
            const CXXRecordDecl* def = CompleteNonDependentCxxRecord(cxx);
            if ((!st.req.emitDefinitions && !st.req.assumeInlineDefinitions)
                || !st.req.requireInScope
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
         * An error clang raised inside a header the caller asked to bind poisons everything
         * downstream: Sema marks the offending declarations invalid, every instantiation that
         * touches them comes out holding error expressions, and companion CodeGen would then walk
         * an AST clang's own driver would never have handed it (that walk is what crashes on an
         * STL-using header). Record the first such diagnostic so the bind can be refused with it.
         * Errors in the in-memory stub - the intentional macro probes and default-argument
         * wrapper requests - and in system headers - an ill-formed STL instantiation, which the
         * error-body sweep already contains - are not this case and stay tolerated.
         */
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
                if (e.file.empty() || !PathInScope(e.file, st.normDirs)) continue;
                ++st.out.headerErrors;
                if (st.out.firstHeaderError.empty())
                    st.out.firstHeaderError =
                        std::format("{} at {}:{}", e.message, e.file, e.line);
            }
        }

        void ComputeCxxAbi(ExtractState& st, ASTContext& ctx)
        {
            if (st.ci == nullptr) return;
            DefineHeaderImplicitSpecialMembers(st);
            DefineDefaultedSpecialMembers(st);
            if (st.abiWork.empty() && st.functionPointerAbiWork.empty()
                && st.memberAbiWork.empty() && !st.req.emitDefinitions) return;
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
                                  << t.getAsString(ctx.getPrintingPolicy()) << "'\n";
                    continue;
                }
                if (ProtoHasIncompleteRecord(fptPtr)) continue;
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

            ComputeCxxMemberAbi(st, ctx, cgm, *cg);

            if (st.req.emitDefinitions)
            {
                llvm::TimeTraceScope emitScope("CxxDefinitionEmit");
                EmitCxxDefinitions(st, ctx, *cg);
            }
        }

        // Fill in the per-slot arrangement of every exported class member. The slot info comes
        // from arrangeCXXMethodType (which prepends 'this') for instance methods and structors,
        // and from arrangeFreeFunctionType for static ones. The FUNCTION TYPE is always taken
        // from Clang's own GetAddrOfGlobal declaration - that is the only source that knows a
        // structor returns 'this' on Itanium/Darwin.
        void ComputeCxxMemberAbi(ExtractState& st, ASTContext& ctx,
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
                        if (loc.VBase == nullptr && loc.VFPtrOffset.isZero())
                        {
                            m.vtableIndex = (int)loc.Index;
                            if (dd != nullptr) m.vtableIndexDeleting = (int)loc.Index;
                        }
                    }
                }

                CanQualType canon = md->getType()->getCanonicalTypeUnqualified();
                if (canon->getAs<FunctionProtoType>() == nullptr) continue;
                CanQual<FunctionProtoType> fpt = canon.castAs<FunctionProtoType>();
                if (ProtoHasIncompleteRecord(fpt.getTypePtr())) continue;
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
        void EmitCxxDefinitions(ExtractState& st, ASTContext& ctx, CodeGenerator& cg)
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
                return pl.isValid() && PathInScope(pl.getFilename(), st.normDirs);
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

                // The body of THIS function contains an error expression (as opposed to
                // reaching one through a call).
                bool HasOwnError(const FunctionDecl* fd)
                {
                    if (fd == nullptr) return false;
                    if (fd->isInvalidDecl()) return true;
                    struct OwnErrorVisitor : RecursiveASTVisitor<OwnErrorVisitor>
                    {
                        bool found = false;
                        bool VisitExpr(Expr* expr)
                        {
                            found = found || expr->containsErrors();
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
                            found = found || expr->containsErrors();
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
                    if (def->isInvalidDecl()) { memo[def] = true; return true; }
                    if (def->getBody() != nullptr) visitor.TraverseStmt(def->getBody());
                    bool bad = visitor.found;
                    for (const FunctionDecl* callee : visitor.callees)
                    {
                        if (bad) break;
                        if (callee == nullptr || callee == def) continue;
                        bad = Reaches(callee, depth + 1);
                    }
                    memo[def] = bad;
                    return bad;
                }
            };
            auto errorReach = std::make_shared<ErrorReachScan>();
            errorReach->active = sawParseErrors;
            auto declHasErrors = [errorReach](const Decl* d) {
                if (d == nullptr || d->isInvalidDecl()) return true;
                if (const auto* fd = llvm::dyn_cast<FunctionDecl>(d))
                    return errorReach->active ? errorReach->Reaches(fd)
                                              : errorReach->HasOwnError(fd);
                if (const auto* vd = llvm::dyn_cast<VarDecl>(d))
                    return vd->getInit() != nullptr && vd->getInit()->containsErrors();
                return false;
            };
            auto isDependentCodeGenDecl = [](const Decl* d) {
                if (d == nullptr || d->getDeclContext()->isDependentContext()) return true;
                if (d->isTemplated()) return true;
                const auto* value = llvm::dyn_cast<ValueDecl>(d);
                return value != nullptr && value->getType()->isDependentType();
            };
            auto emitDecl = [&](Decl* d, auto&& emitDeclRef) -> void {
                if (d == nullptr || !inScopeDecl(d)) return;
                if (const auto* linkage = llvm::dyn_cast<LinkageSpecDecl>(d))
                {
                    for (Decl* member : linkage->decls()) emitDeclRef(member, emitDeclRef);
                    return;
                }
                if (declHasErrors(d))
                {
                    rememberDroppedWrapper(llvm::dyn_cast<FunctionDecl>(d));
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
                cg.HandleTopLevelDecl(DeclGroupRef(d));
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
                std::vector<FunctionDecl*> direct;

                explicit ErrorBodySweep(ErrorReachScan& s) : scan(s) {}
                bool shouldVisitTemplateInstantiations() const { return true; }
                bool VisitFunctionDecl(FunctionDecl* fd)
                {
                    if (fd == nullptr || !fd->doesThisDeclarationHaveABody()) return true;
                    // Memoize the whole call graph BEFORE any body is emptied, so a later query
                    // cannot mistake an emptied body for a clean one.
                    if (scan.Reaches(fd) && scan.HasOwnError(fd)) direct.push_back(fd);
                    return true;
                }
            } errorBodies(*errorReach);
            errorBodies.TraverseDecl(ctx.getTranslationUnitDecl());
            for (Decl* d : st.announcedDecls) errorBodies.TraverseDecl(d);
            for (FunctionDecl* fd : errorBodies.direct)
            {
                if (st.req.verbose)
                    std::cout << "[verbose]   C++ body emptied, its instantiation reported an "
                                 "error: " << fd->getQualifiedNameAsString() << "\n";
                fd->setBody(CompoundStmt::CreateEmpty(ctx, /*NumStmts*/ 0, /*HasFPFeatures*/ false));
            }

            // Phase 1: show Clang the whole translation unit. Inline definitions stay deferred.
            for (Decl* d : ctx.getTranslationUnitDecl()->decls())
                emitDecl(d, emitDecl);
            // Plus everything Sema announced that decls() does not contain (see announcedDecls).
            for (Decl* d : st.announcedDecls)
                emitDecl(d, emitDecl);
            /*
             * An inline static data member is not a top-level decl, and unlike a member FUNCTION
             * there is no lexically-in-a-record fallback that finds it later - CodeGen only knows
             * about a variable it was handed. Hand each one over explicitly so the request below
             * has a deferred definition to promote instead of just a declaration.
            */
            // A requested template can instantiate an inline or constexpr static data member
            // transitively (nlohmann::detail::static_const<T>::value is one example). Those
            // specializations are not top-level declarations and are not members of the
            // requested record, but an emitted body can still odr-use them. Queue the used
            // definitions explicitly so CodeGen emits their linkonce_odr storage.
            struct UsedStaticVarVisitor : RecursiveASTVisitor<UsedStaticVarVisitor>
            {
                ExtractState& state;
                std::unordered_set<const VarDecl*> seen;

                explicit UsedStaticVarVisitor(ExtractState& s) : state(s)
                {
                    for (const VarDecl* vd : state.varEmitWork) seen.insert(vd);
                }

                bool shouldVisitTemplateInstantiations() const { return true; }

                bool VisitVarDecl(VarDecl* vd)
                {
                    if (!vd->isStaticDataMember() || !vd->isUsed()
                        || (!vd->isConstexpr() && !vd->isInline()))
                        return true;
                    const VarDecl* definition = vd->getDefinition();
                    if (definition == nullptr) definition = vd;
                    if (seen.insert(definition).second)
                        state.varEmitWork.push_back(definition);
                    return true;
                }
            } usedStaticVars(st);
            usedStaticVars.TraverseDecl(ctx.getTranslationUnitDecl());

            for (const VarDecl* vd : st.varEmitWork)
                if (vd != nullptr && !declHasErrors(vd))
                {
                    if (st.req.emitDefinitions && isDependentCodeGenDecl(vd))
                    {
                        if (st.req.verbose)
                            std::cout << "[verbose]   skipped dependent C++ static variable "
                                      << vd->getQualifiedNameAsString() << "\n";
                        continue;
                    }
                    cg.HandleTopLevelDecl(DeclGroupRef(const_cast<VarDecl*>(vd)));
                }

            // Phase 2: reference what cflat binds so the deferred bodies become emission work.
            auto request = [&](GlobalDecl gd) { cg.GetAddrOfGlobal(gd, /*isForDefinition*/ false); };
            // hasBody(), not getDefinition(): a defaulted or deleted member is already "a
            // definition" in the AST, and only a real body is something CodeGen can emit.
            for (const auto& [idx, fd] : st.abiWork)
                if (fd != nullptr && !declHasErrors(fd) && !fd->getType()->isDependentType()
                    && fd->hasBody())
                    request(GlobalDecl(fd));
            for (const auto& w : st.memberAbiWork)
                if (w.md != nullptr && !declHasErrors(w.md) && w.md->hasBody()
                    && !isDependentCodeGenDecl(w.md))
                    request(MemberGlobalDecl(w.md));
            for (const VarDecl* vd : st.varEmitWork)
                if (vd != nullptr && !declHasErrors(vd) && !isDependentCodeGenDecl(vd))
                    request(GlobalDecl(vd));
            // Promote concrete free-function helpers that a requested inline body uses. Clang
            // defers these internal inline definitions independently of their caller.
            std::vector<const FunctionDecl*> usedFunctionWork;
            struct UsedFunctionVisitor : RecursiveASTVisitor<UsedFunctionVisitor>
            {
                std::vector<const FunctionDecl*>& work;
                std::unordered_set<const FunctionDecl*> seen;

                explicit UsedFunctionVisitor(std::vector<const FunctionDecl*>& w) : work(w) {}
                bool shouldVisitTemplateInstantiations() const { return true; }
                bool VisitFunctionDecl(FunctionDecl* fd)
                {
                    // Anything whose definition this module would have to provide itself: an
                    // internal-linkage helper (libc++'s _LIBCPP_HIDE_FROM_ABI), an inline body, or
                    // a template instantiation. A plain external non-inline function is NOT
                    // promoted - the bound library exports that strong symbol already.
                    const bool ownDefinitionNeeded = fd != nullptr
                        && (fd->hasAttr<AlwaysInlineAttr>() || fd->hasAttr<InternalLinkageAttr>()
                            || fd->isInlined()
                            || fd->getTemplateSpecializationKind() == TSK_ImplicitInstantiation
                            || fd->getFormalLinkage() == Linkage::Internal);
                    if (fd == nullptr || llvm::isa<CXXMethodDecl>(fd) || !fd->hasBody()
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
            usedFunctions.TraverseDecl(ctx.getTranslationUnitDecl());
            for (const FunctionDecl* fd : usedFunctionWork)
            {
                cg.HandleTopLevelDecl(DeclGroupRef(const_cast<FunctionDecl*>(fd)));
                cg.GetAddrOfGlobal(GlobalDecl(const_cast<FunctionDecl*>(fd)),
                                   /*isForDefinition*/ true);
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
                cg.HandleVTable(const_cast<CXXRecordDecl*>(def));
            }

            // Phase 3: flush. This emits the deferred definitions and finalizes the module.
            if (st.req.verbose)
                std::cout << std::format(
                    "[verbose]   extraction codegen flush: {} clang error(s) so far\n",
                    ctx.getDiagnostics().getClient()->getNumErrors());
            cg.HandleTranslationUnit(ctx);

            llvm::Module* mod = cg.GetModule();
            if (mod == nullptr) return;   // CodeGen error: nothing is bound to a local definition

            unsigned defs = 0;
            for (const llvm::Function& f : mod->functions())
                if (!f.isDeclaration()) ++defs;
            for (const llvm::GlobalVariable& g : mod->globals())
                if (!g.isDeclaration()) ++defs;
            if (defs == 0) return;

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
                        m.bindRefusal = "cannot be instantiated for these template arguments "
                                        "(clang reported an error inside the body it generated)";
                    m.linkageName.clear();
                    m.abi = RawAbi{};
                    continue;
                }
                if (!m.needsLocalDefinition || m.linkageName.empty()) continue;
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
                /*
                 * An EXPLICIT INSTANTIATION DECLARATION (`extern template class basic_string<char>;`
                 * in libc++) says the bound library owns this specialization's symbols: Clang
                 * deliberately emits a reference rather than a body, and a real C++ translation unit
                 * links against the library's copy. Trust it the same way - but only when the
                 * library really does export it. isExternallyVisible() is about LINKAGE, so a
                 * _LIBCPP_HIDE_FROM_ABI member (hidden visibility, excluded from the explicit
                 * instantiation, deliberately absent from libc++.dylib) satisfies it while having no
                 * symbol anywhere. Trusting that turns a compile-time refusal into a link-time
                 * "undefined symbol", so require default visibility and no exclusion attribute.
                 */
                else if (w.md->getTemplateSpecializationKind()
                             == clang::TSK_ExplicitInstantiationDeclaration
                         && w.md->isExternallyVisible()
                         && w.md->getVisibility() == clang::DefaultVisibility
                         && !w.md->hasAttr<clang::ExcludeFromExplicitInstantiationAttr>())
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
                llvm::raw_string_ostream os(st.out.bitcode);
                CxxExtractionStageTimer serialize(st.req.verbose && st.req.cxxMode,
                                                  "bitcode serialization");
                llvm::WriteBitcodeToFile(*mod, os);
                os.flush();
            }
            st.out.emittedDefinitions = defs;
        }

        struct ExtractConsumer : public ASTConsumer
        {
            ExtractState& st;
            explicit ExtractConsumer(ExtractState& s) : st(s) {}
            // Record what Sema announces; nothing is emitted here (CodeGen runs later, once, over
            // the finished AST). Only the definition-emission path replays this list.
            bool HandleTopLevelDecl(DeclGroupRef dg) override
            {
                if (st.req.emitDefinitions)
                    for (Decl* d : dg)
                    {
                        if (st.req.requireInScope && st.ci != nullptr)
                        {
                            PresumedLoc pl = st.ci->getSourceManager().getPresumedLoc(d->getLocation());
                            if (pl.isInvalid() || !PathInScope(pl.getFilename(), st.normDirs)) continue;
                        }
                        st.announcedDecls.push_back(d);
                    }
                return true;
            }
            void HandleTranslationUnit(ASTContext& ctx) override
            {
                // A bound header clang rejected leaves invalid declarations behind, and neither
                // the harvest nor the ABI/CodeGen pass below is safe to run over them - both walk
                // into Clang machinery a real driver would never reach after an error. The caller
                // refuses the bind with the diagnostic instead.
                RecordInScopeHeaderErrors(st);
                if (st.out.headerErrors > 0)
                {
                    if (st.req.verbose)
                        std::cout << std::format(
                            "[verbose]   bound header does not compile: {}\n",
                            st.out.firstHeaderError);
                    return;
                }
                DeclVisitor v(ctx, st);
                {
                    CxxExtractionStageTimer harvest(st.req.verbose && st.req.cxxMode,
                                                     "record/sig harvest");
                    if (st.req.cxxTypeRequests.empty() && st.req.cxxFunctionWrapperNames.empty())
                        v.TraverseDecl(ctx.getTranslationUnitDecl());
                    else if (!st.req.cxxTypeRequests.empty()) v.ProcessTypeRequests();
                    else v.ProcessFunctionRequests();
                }
                if (st.req.cxxMode)
                {
                    llvm::TimeTraceScope abiScope("CxxAbiArrange");
                    CxxExtractionStageTimer codegen(st.req.verbose,
                                                     "stage-2 CodeGen/companion emission");
                    ComputeCxxAbi(st, ctx);
                }
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
    } // namespace

    bool ExtractCInterop(const ExtractRequest& req, ExtractResult& out, std::string& err)
    {
        ExtractState st(req, out);

        // Precompile the group's include prologue. One parse now, `-include-pch` later.
        if (!req.pchOutputPath.empty())
        {
            llvm::TimeTraceScope pchScope("GeneratePch", req.mainFileName);
            CxxExtractionStageTimer pchStage(req.verbose && req.cxxMode, "clang precompile header");
            clang::GeneratePCHAction generate;
            return RunAction(req, req.source, generate, err, nullptr, nullptr, &out);
        }

        // C++ uuid harvest: a single full parse that only collects record __declspec(uuid) GUIDs.
        if (req.uuidHarvestCxx)
        {
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
            llvm::TimeTraceScope parseScope("FullParse", req.mainFileName.empty() ? req.realPath : req.mainFileName);
            CxxExtractionStageTimer parseStage(req.verbose && req.cxxMode,
                                               "clang parse stage 2");
            ExtractAction extract(st);
            bool ok = RunAction(req, fullSource, extract, err, &out.prereqErrors,
                                &out.firstPrereqError, &out);

            if (ok && req.cxxMode && req.autoInstantiateCxxTypes
                && !st.incompleteCxxTypes.empty())
            {
                ExtractRequest retry = req;
                retry.autoInstantiateCxxTypes = false;
                retry.source = req.source;
                for (const std::string& spelling : st.incompleteCxxTypes)
                    retry.source += "\ntemplate class " + spelling + ";\n";
                ExtractResult retried;
                std::string retryError;
                if (ExtractCInterop(retry, retried, retryError))
                {
                    bool recovered = true;
                    for (const RawSig& original : out.sigs)
                    {
                        bool namesIncomplete = false;
                        for (const std::string& spelling : st.incompleteCxxTypes)
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
                                for (const std::string& spelling : st.incompleteCxxTypes)
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

            // A probe whose injected variable never reached the AST (the body is a type name or
            // an unknown identifier) still reports its alias spelling; the binder decides.
            for (size_t i = 0; i < st.probes.size(); ++i)
            {
                const MacroProbe& mp = st.probes[i];
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
}
