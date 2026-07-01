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

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Token.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/MemoryBuffer.h"

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
            return qt.getCanonicalType().getAsString(ctx.getPrintingPolicy());
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
            int line = 1;
            int col = 0;
        };

        struct ExtractState
        {
            const ExtractRequest& req;
            ExtractResult& out;
            std::vector<MacroProbe> probes;   // index == probe slot
            std::unordered_set<std::string> emittedGlobals;  // dedup global var redeclarations by name
            std::unordered_set<std::string> emittedOpaqueForward;  // dedup opaque forward-decl records by tag
            std::vector<std::string> normDirs; // req.inScopeDirs normalized once (NormPath + trailing-/ stripped)
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

                MacroProbe mp;
                mp.name = name; mp.file = file; mp.line = line; mp.col = col;
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
                if (fd->getStorageClass() == SC_Static) return true;  // not externally linkable
                if (st.req.definitionsOnly && !fd->isThisDeclarationADefinition()) return true;
                std::string file; int line = 1, col = 0;
                if (!LocOf(fd, file, line, col)) return true;

                RawSig sig;
                sig.name = fd->getNameAsString();
                sig.retType = CanonicalSpelling(ctx, fd->getReturnType());
                sig.variadic = fd->isVariadic();
                sig.file = file; sig.line = line; sig.col = col;
                for (const ParmVarDecl* p : fd->parameters())
                {
                    sig.paramTypes.push_back(CanonicalSpelling(ctx, p->getType()));
                    sig.paramNames.push_back(p->getNameAsString());
                }
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
                for (const FieldDecl* f : rd->fields())
                {
                    if (f->getDeclName().isEmpty())
                    {
                        if (f->isBitField())
                        {
                            RawField rf;
                            rf.isBitfield = true;
                            rf.bitWidth = f->getBitWidthValue(ctx);
                            rf.ctype = CanonicalSpelling(ctx, f->getType());
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
                            rec.fields.push_back(std::move(fe));
                        }
                        continue;  // unnamed non-bitfield non-anon: nothing to record
                    }

                    RawField rf;
                    rf.name = f->getNameAsString();

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
                        rf.bitWidth = f->getBitWidthValue(ctx);
                    }
                    rec.fields.push_back(std::move(rf));
                }
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
                rec.isUnion = rd->isUnion();
                rec.file = file; rec.line = line; rec.col = col;
                rec.inScope = !st.req.requireInScope || PathInScope(file, st.normDirs);
                CollectFields(rd, rec.name, rec);
                st.out.records.push_back(std::move(rec));
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
                const Expr* init = vd->getInit();
                if (!init) return true;

                const MacroProbe& mp = st.probes[idx];
                RawMacro m;
                m.name = mp.name; m.file = mp.file; m.line = mp.line; m.col = mp.col;
                m.kind = RawMacro::Skip;

                // EvaluateAsRValue must not be called on an error-recovery or value-dependent
                // initializer (it can assert on the inconsistent node). Probes for non-value
                // macros are built unparenthesized so they fail to parse and leave a null init
                // (handled above), but skip defensively here for any that still produce one.
                if (init->containsErrors() || init->isValueDependent())
                {
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
                st.out.macros.push_back(std::move(m));
                return true;
            }
        };

        struct ExtractConsumer : public ASTConsumer
        {
            ExtractState& st;
            explicit ExtractConsumer(ExtractState& s) : st(s) {}
            void HandleTranslationUnit(ASTContext& ctx) override
            {
                DeclVisitor v(ctx, st);
                v.TraverseDecl(ctx.getTranslationUnitDecl());
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

            llvm::IntrusiveRefCntPtr<DiagnosticOptions> diagOpts(new DiagnosticOptions());
            llvm::IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
            llvm::IntrusiveRefCntPtr<DiagnosticsEngine> diags(
                new DiagnosticsEngine(diagIDs, diagOpts, new IgnoringDiagConsumer(), /*own*/ true));

            std::shared_ptr<CompilerInvocation> invocation;
            {
                llvm::TimeTraceScope invScope("CreateInvocation", inputName);
                CreateInvocationOptions civOpts;
                civOpts.Diags = diags;
                civOpts.RecoverOnError = true;
                std::unique_ptr<CompilerInvocation> invocationUP = createInvocation(cargs, civOpts);
                if (!invocationUP) { err = "createInvocation failed"; return false; }
                // Header binds only need declarations. Skipping function bodies avoids parsing
                // inline definitions in system headers (e.g. an `&`-taking __inline in math.h
                // trips a clang classifier assert in assertion-enabled builds) and is faster;
                // the FunctionDecl + signature are still produced.
                if (req.skipFunctionBodies)
                    invocationUP->getFrontendOpts().SkipFunctionBodies = true;
                invocation.reset(invocationUP.release());
            }

            CompilerInstance ci;
            ci.setInvocation(invocation);
            // Own the consumer via the engine, but keep a raw pointer so the prereq tally can be
            // read back after the parse (the engine, hence the consumer, outlives this scope).
            PrereqDiagConsumer* prereqConsumer = new PrereqDiagConsumer();
            ci.createDiagnostics(prereqConsumer, /*own*/ true);

            if (!source.empty())
            {
                std::unique_ptr<llvm::MemoryBuffer> buf =
                    llvm::MemoryBuffer::getMemBufferCopy(source, req.mainFileName);
                ci.getPreprocessorOpts().addRemappedFile(req.mainFileName, buf.release());
            }

            {
                llvm::TimeTraceScope execScope("ExecuteFrontend", inputName);
                if (!ci.ExecuteAction(action)) { err = "ExecuteAction failed"; return false; }
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
            return RunAction(req, fullSource, extract, err, &out.prereqErrors, &out.firstPrereqError);
        }
    }
}
