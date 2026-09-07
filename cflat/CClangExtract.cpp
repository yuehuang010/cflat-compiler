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
#include "clang/AST/Expr.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/AST/BaseSubobject.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Token.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ADT/SmallString.h"

#include "llvm/Support/TimeProfiler.h"

#include <algorithm>
#include <unordered_set>

namespace cflat_cinterop
{
    using namespace clang;

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

        // The complete-object GlobalDecl for a member: structors need their variant, everything
        // else is the plain decl.
        GlobalDecl MemberGlobalDecl(const CXXMethodDecl* md)
        {
            if (const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md))
                return GlobalDecl(ctor, Ctor_Complete);
            if (const auto* dtor = llvm::dyn_cast<CXXDestructorDecl>(md))
                return GlobalDecl(dtor, Dtor_Complete);
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

        struct ExtractState
        {
            const ExtractRequest& req;
            ExtractResult& out;
            std::vector<MacroProbe> probes;   // index == probe slot
            std::unordered_set<unsigned> emittedProbes;  // probe slots that produced a RawMacro
            std::unordered_set<std::string> emittedGlobals;  // dedup global var redeclarations by name
            std::unordered_set<std::string> emittedOpaqueForward;  // dedup opaque forward-decl records by tag
            std::vector<std::string> normDirs; // req.inScopeDirs normalized once (NormPath + trailing-/ stripped)
            // Set in BeginSourceFileAction so the ABI pass can build a CodeGenerator against the
            // very invocation that produced the AST (same triple, same target features).
            CompilerInstance* ci = nullptr;
            // cxxMode only: (index into out.sigs, the decl it came from). Resolved after the
            // traversal so a single CodeGenerator serves every declaration.
            std::vector<std::pair<size_t, const FunctionDecl*>> abiWork;
            // cxxMode only: (index into out.records, index into that record's members, decl).
            struct MemberAbiWork { size_t recordIdx; size_t memberIdx; const CXXMethodDecl* md; };
            std::vector<MemberAbiWork> memberAbiWork;
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

        struct DeclVisitor : public RecursiveASTVisitor<DeclVisitor>
        {
            ASTContext& ctx;
            SourceManager& sm;
            ExtractState& st;

            DeclVisitor(ASTContext& c, ExtractState& s) : ctx(c), sm(c.getSourceManager()), st(s) {}

            // Resolve a decl's presumed location WITHOUT applying the in-scope filter. Returns
            // false only on an invalid/unknown location.
            bool LocOfRaw(const Decl* d, std::string& file, int& line, int& col) const
            {
                SourceLocation loc = d->getLocation();
                if (loc.isInvalid()) return false;
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

            bool VisitFunctionDecl(FunctionDecl* fd)
            {
                if (!fd->getIdentifier()) return true;
                // M0-M3 expose free functions. Methods, constructors and operators need the
                // class ABI/lifetime machinery from M4; keeping them out avoids publishing a
                // callable declaration with the wrong implicit object parameter.
                if (fd->getDeclContext()->isRecord()) return true;
                if (fd->getStorageClass() == SC_Static) return true;  // not externally linkable
                if (st.req.definitionsOnly && !fd->isThisDeclarationADefinition()) return true;
                std::string file; int line = 1, col = 0;
                if (!LocOf(fd, file, line, col)) return true;

                RawSig sig;
                sig.name = st.req.cxxMode ? CxxQualifiedName(fd) : fd->getNameAsString();
                if (st.req.cxxMode && (fd->isInAnonymousNamespace() || !IsValidDottedName(sig.name)))
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
                for (const ParmVarDecl* p : fd->parameters())
                {
                    sig.paramTypes.push_back(CanonicalSpelling(ctx, p->getType()));
                    sig.paramNames.push_back(p->getNameAsString());
                }
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
                unsigned fieldIndex = 0;
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
                            rf.offsetBytes = layout.getFieldOffset(fieldIndex) / 8;
                            rec.fields.push_back(std::move(rf));
                            ++fieldIndex;
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
                            fe.offsetBytes = layout.getFieldOffset(fieldIndex) / 8;
                            rec.fields.push_back(std::move(fe));
                            ++fieldIndex;
                        }
                        ++fieldIndex;
                        continue;  // unnamed non-bitfield non-anon: nothing to record
                    }

                    RawField rf;
                    rf.name = f->getNameAsString();
                    rf.access = MapAccess(f->getAccess());
                    rf.offsetBytes = layout.getFieldOffset(fieldIndex) / 8;

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
                    }
                    rec.fields.push_back(std::move(rf));
                    ++fieldIndex;
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
                rec.hasBases = cxx->getNumBases() > 0 || cxx->getNumVBases() > 0;
                rec.hasVirtualBases = cxx->getNumVBases() > 0;
                rec.isAbstract = cxx->isAbstract();
                rec.hasTrivialDefaultCtor = cxx->hasTrivialDefaultConstructor();
                rec.hasTrivialCopyCtor = cxx->hasTrivialCopyConstructor();
                rec.hasTrivialDtor = !cxx->hasNonTrivialDestructor();
                rec.hasDefaultCtor = cxx->hasDefaultConstructor();
                rec.hasCopyCtor = cxx->hasCopyConstructorWithConstParam()
                               || cxx->needsImplicitCopyConstructor()
                               || cxx->hasUserDeclaredCopyConstructor();
                rec.isAggregate = cxx->isAggregate();

                for (const CXXMethodDecl* md : cxx->methods())
                {
                    // Templates and their specializations need Sema instantiation (M5).
                    if (md->getDescribedFunctionTemplate() != nullptr) continue;
                    if (md->getPrimaryTemplate() != nullptr) continue;
                    const auto* ctor = llvm::dyn_cast<CXXConstructorDecl>(md);
                    const auto* dtor = llvm::dyn_cast<CXXDestructorDecl>(md);
                    // Operators and conversion functions are M5; they have no plain identifier.
                    // Copy and move ASSIGNMENT are the exception: they are special members that
                    // nontrivial-class lifetime needs (M4b), so they are exported under the name
                    // "operator=" and consumed by the class's assignment table, never as a
                    // callable member.
                    const bool isAssignSpecial = md->isCopyAssignmentOperator()
                                              || md->isMoveAssignmentOperator();
                    if (ctor == nullptr && dtor == nullptr && md->getIdentifier() == nullptr
                        && !isAssignSpecial)
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
                        m.name = md->getNameAsString();
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
                            const ASTRecordLayout& rl =
                                ctx.getASTRecordLayout(mineRd->getDefinition());
                            if (!mineRd->getDefinition()->isDerivedFrom(theirsRd->getDefinition())
                                || rl.getBaseClassOffset(theirsRd->getDefinition())
                                       .getQuantity() != 0)
                                m.covariantReturnNeedsAdjust = true;
                        }
                    m.isDeleted = md->isDeleted();
                    m.isDefaulted = md->isDefaulted();
                    m.isImplicit = md->isImplicit();
                    m.isNoexcept = DeclIsNoexcept(md);
                    m.access = MapAccess(md->getAccess());
                    m.variadic = md->isVariadic();
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
                        m.retType = CanonicalSpelling(ctx, md->getReturnType());

                    if (m.kind == RawCxxMember::Instance || m.kind == RawCxxMember::Constructor
                        || m.kind == RawCxxMember::Destructor)
                    {
                        // 'this' first, spelled as a plain pointer to the record (const is
                        // dropped by CFlat, so the const overload differs only in `isConst`).
                        m.paramTypes.push_back(CanonicalSpelling(ctx,
                            ctx.getPointerType(ctx.getCanonicalTagType(cxx))));
                        m.paramNames.push_back("this");
                    }
                    for (const ParmVarDecl* p : md->parameters())
                    {
                        m.paramTypes.push_back(CanonicalSpelling(ctx, p->getType()));
                        m.paramNames.push_back(p->getNameAsString());
                    }
                    if (!m.isDeleted && !m.needsLocalDefinition)
                        m.linkageName = CxxLinkageName(ctx, MemberGlobalDecl(md));
                    LocOfRaw(md, m.file, m.line, m.col);

                    if (m.isDeleted && ctor != nullptr && ctor->isDefaultConstructor())
                        rec.hasDeletedDefaultCtor = true;
                    if (m.isDeleted && m.isCopyCtor) rec.hasDeletedCopyCtor = true;

                    // Only members with a real symbol get an ABI arrangement; the rest are
                    // exported for diagnostics only.
                    outDecls.push_back((!m.isDeleted && !m.needsLocalDefinition) ? md : nullptr);
                    rec.members.push_back(std::move(m));
                }

                for (const Decl* d : cxx->decls())
                {
                    const auto* vd = llvm::dyn_cast<VarDecl>(d);
                    if (vd == nullptr || !vd->isStaticDataMember()) continue;
                    if (vd->getIdentifier() == nullptr) continue;
                    // An inline / constexpr static member is emitted per-TU on demand, so the
                    // bound library need not contain it. Only an out-of-line definition is a
                    // symbol CFlat may read.
                    if (vd->isConstexpr() || vd->isInline()) continue;
                    if (vd->hasInit()) continue;
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
                        + (uint64_t)layout.getBaseClassOffset(brd).getQuantity();
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

                std::string file; int line = 1, col = 0;
                // Collect records regardless of scope (LocOfRaw, not LocOf): an in-scope struct
                // may reference an out-of-scope struct by value (e.g. MSG.pt is a POINT defined
                // in the SDK shared/ dir). The backend keeps the transitive closure of in-scope
                // records and drops the rest, so the dependency is available without registering
                // every unrelated SDK struct.
                if (!LocOfRaw(rd, file, line, col)) return true;

                RawRecord rec;
                rec.name = rd->getNameAsString();
                // An unnamed record keeps an empty name - the caller synthesizes its tag, and a
                // qualified spelling would hand it the "(unnamed struct at ...)" placeholder.
                rec.qualifiedName = rec.name;
                if (st.req.cxxMode && !rec.name.empty())
                {
                    if (rd->isInAnonymousNamespace()) return true;
                    rec.qualifiedName = CxxQualifiedName(rd);
                    if (!IsValidDottedName(rec.qualifiedName)) return true;
                    rec.name = rec.qualifiedName;
                }
                rec.isUnion = rd->isUnion();
                rec.isCxx = st.req.cxxMode;
                rec.file = file; rec.line = line; rec.col = col;
                rec.inScope = !st.req.requireInScope || PathInScope(file, st.normDirs);
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
                        CollectCxxMembers(cxx, rec, memberDecls);
                        for (const CXXBaseSpecifier& b : cxx->bases())
                        {
                            const auto* brd = b.getType()->getAsCXXRecordDecl();
                            RawCxxBase rb;
                            rb.name = brd != nullptr ? CxxQualifiedName(brd) : std::string();
                            rb.access = MapAccess(b.getAccessSpecifier());
                            rb.isVirtual = b.isVirtual();
                            if (brd != nullptr && brd->getDefinition() != nullptr)
                                rb.offsetBytes = (uint64_t)ctx.getASTRecordLayout(cxx)
                                    .getBaseClassOffset(brd->getDefinition()).getQuantity();
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
                if (!flattened) CollectFields(rd, rec.name, rec);
                st.out.records.push_back(std::move(rec));
                if (!memberDecls.empty())
                {
                    const size_t recIdx = st.out.records.size() - 1;
                    for (size_t i = 0; i < memberDecls.size(); ++i)
                        if (memberDecls[i] != nullptr)
                            st.memberAbiWork.push_back({ recIdx, i, memberDecls[i] });
                }
                return true;
            }

            bool VisitTypedefNameDecl(TypedefNameDecl* td)
            {
                if (!td->getIdentifier()) return true;
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
                LocOfRaw(td, t.file, t.line, t.col);
                if (const RecordType* rt = u->getAs<RecordType>())
                {
                    const RecordDecl* rd = rt->getDecl();
                    t.isAnonymousRecord = rd->isStruct() && !rd->getIdentifier()
                                       && rd->getTypedefNameForAnonDecl() == td;
                }
                if (!canon.empty() && canon != name) t.underlying = canon;
                else if (!sugared.empty())           t.underlying = sugared;
                else                                  t.underlying = canon;
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
            s.inReg = ai.getInReg();
            if (ai.isDirect() || ai.isExtend())
            {
                s.coerceType  = LlvmTypeText(ai.getCoerceToType());
                s.paddingType = LlvmTypeText(ai.getPaddingType());
                s.directOffset = (uint64_t)ai.getDirectOffset();
                s.canBeFlattened = ai.getCanBeFlattened();
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

        /*
         * cxxMode: ask Clang for the calling convention of every collected C++ free function and
         * serialize it onto the RawSig. The CodeGenerator (and its LLVMContext / module) is
         * transient - everything the backend needs is plain data by the time this returns, which
         * is what lets the recipe survive the on-disk signature cache.
         */
        void ComputeCxxMemberAbi(ExtractState& st, ASTContext& ctx,
                                 clang::CodeGen::CodeGenModule& cgm, CodeGenerator& cg);

        void ComputeCxxAbi(ExtractState& st, ASTContext& ctx)
        {
            if (st.abiWork.empty() || st.ci == nullptr) return;
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
                const CGFunctionInfo& fi = arrangeFreeFunctionType(cgm, fpt);

                RawAbi abi;
                abi.valid = true;
                abi.callingConv = fi.getEffectiveCallingConvention();
                abi.fnTypeText = LlvmTypeText(convertFreeFunctionType(cgm, fd));
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
                st.out.sigs[idx].abi = std::move(abi);
            }

            ComputeCxxMemberAbi(st, ctx, cgm, *cg);
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
                if (md->isVariadic()) continue;

                /*
                 * M6 - the vtable slot of a virtual member, straight from Clang's Itanium vtable
                 * layout. The index is relative to the address point of the vtable of the class
                 * that DECLARES the member, which is exactly the subobject the call site will have
                 * adjusted `this` to. A virtual destructor gets both of its slots: D1 (complete
                 * object) and D0 (deleting, which also releases the storage).
                 */
                if (md->isVirtual())
                {
                    if (auto* itanium = llvm::dyn_cast<clang::ItaniumVTableContext>(
                            ctx.getVTableContext()))
                    {
                        if (const auto* dd = llvm::dyn_cast<CXXDestructorDecl>(md))
                        {
                            m.vtableIndex = (int)itanium->getMethodVTableIndex(
                                GlobalDecl(dd, Dtor_Complete));
                            m.vtableIndexDeleting = (int)itanium->getMethodVTableIndex(
                                GlobalDecl(dd, Dtor_Deleting));
                        }
                        else
                            m.vtableIndex = (int)itanium->getMethodVTableIndex(GlobalDecl(md));
                    }
                }

                CanQualType canon = md->getType()->getCanonicalTypeUnqualified();
                if (canon->getAs<FunctionProtoType>() == nullptr) continue;
                CanQual<FunctionProtoType> fpt = canon.castAs<FunctionProtoType>();

                const CGFunctionInfo* fi = nullptr;
                if (m.kind == RawCxxMember::StaticMethod)
                    fi = &arrangeFreeFunctionType(cgm, fpt);
                else
                    fi = &arrangeCXXMethodType(cgm, md->getParent(), fpt.getTypePtr(), md);
                if (fi == nullptr) continue;

                RawAbi abi;
                abi.valid = true;
                abi.callingConv = fi->getEffectiveCallingConvention();
                abi.ret = DescribeAbiSlot(fi->getReturnInfo());
                unsigned next = (abi.ret.kind == RawAbiSlot::Indirect
                              || abi.ret.kind == RawAbiSlot::IndirectAliased) ? 1u : 0u;
                for (const auto& a : fi->arguments())
                {
                    RawAbiSlot s = DescribeAbiSlot(a.info);
                    s.llvmArgIndex = next;
                    next += s.llvmArgCount;
                    abi.params.push_back(std::move(s));
                }
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

        struct ExtractConsumer : public ASTConsumer
        {
            ExtractState& st;
            explicit ExtractConsumer(ExtractState& s) : st(s) {}
            void HandleTranslationUnit(ASTContext& ctx) override
            {
                DeclVisitor v(ctx, st);
                v.TraverseDecl(ctx.getTranslationUnitDecl());
                if (st.req.cxxMode)
                {
                    llvm::TimeTraceScope abiScope("CxxAbiArrange");
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
        class PrereqDiagConsumer : public DiagnosticConsumer
        {
        public:
            unsigned prereqErrors = 0;
            std::string firstPrereqError;

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
                if (msg.str().find("unknown type name") == llvm::StringRef::npos) return;

                // Errors in the in-memory stub (the macro probes) are not header prerequisites.
                if (info.hasSourceManager())
                {
                    const SourceManager& sm = info.getSourceManager();
                    if (info.getLocation().isValid() && sm.isInMainFile(info.getLocation()))
                        return;
                }

                ++prereqErrors;
                if (firstPrereqError.empty()) firstPrereqError = msg.str().str();
            }
        };

        // Build a CompilerInstance from driver args + an optional in-memory main file, then run
        // `action`. When `source` is non-empty it is remapped onto req.mainFileName; otherwise
        // req.realPath is parsed from disk. When `outPrereqErrors` is non-null it receives the
        // count (and `outFirstPrereqError` the text) of missing-prerequisite errors seen in the
        // parse - used by the header-bind path to detect a header that needs a grouped import.
        bool RunAction(const ExtractRequest& req, const std::string& source,
                       FrontendAction& action, std::string& err,
                       unsigned* outPrereqErrors = nullptr,
                       std::string* outFirstPrereqError = nullptr)
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
            if (outPrereqErrors) *outPrereqErrors = prereqConsumer->prereqErrors;
            if (outFirstPrereqError) *outFirstPrereqError = prereqConsumer->firstPrereqError;
            return true;
        }
    } // namespace

    bool ExtractCInterop(const ExtractRequest& req, ExtractResult& out, std::string& err)
    {
        ExtractState st(req, out);

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
                PrepassAction prepass(st);
                if (!RunAction(req, req.source, prepass, err)) return false;
            }

            // Append a value/type probe per discovered object-like macro to the main stub.
            {
                llvm::TimeTraceScope probeScope("BuildMacroProbes", req.mainFileName);
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
            ExtractAction extract(st);
            bool ok = RunAction(req, fullSource, extract, err, &out.prereqErrors, &out.firstPrereqError);

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
