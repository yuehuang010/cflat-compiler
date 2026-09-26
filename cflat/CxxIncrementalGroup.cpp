#include "CxxIncrementalGroup.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Module.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    /*
     * The interpreter is used only to parse and emit bitcode; nothing it produces is ever run.
     * Clang's default executor is an in-process LLJIT for the TU's triple, which under a cross
     * `-p` target JITs foreign-architecture code (the runtime prelude and the LLJIT at-exit thunk)
     * and calls into it from interpreter teardown. This executor accepts every module and runs
     * nothing, so no triple ever reaches ORC.
     */
    class ParseOnlyExecutor : public clang::IncrementalExecutor
    {
    public:
        llvm::Error addModule(clang::PartialTranslationUnit&) override { return llvm::Error::success(); }
        llvm::Error removeModule(clang::PartialTranslationUnit&) override { return llvm::Error::success(); }
        llvm::Error runCtors() const override { return llvm::Error::success(); }
        llvm::Error cleanUp() override { return llvm::Error::success(); }
        llvm::Expected<llvm::orc::ExecutorAddr> getSymbolAddress(llvm::StringRef name,
                                                                 SymbolNameKind) const override
        {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                "cflat C++ interpreter does not execute code: no address for '" + name.str() + "'");
        }
        llvm::Error LoadDynamicLibrary(const char* name) override
        {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                "cflat C++ interpreter does not execute code: cannot load '" + std::string(name) + "'");
        }
    };

    const clang::CXXRecordDecl* IncompleteCflatRecordIn(
        llvm::ArrayRef<clang::TemplateArgument> args, unsigned depth);

    bool InCflatUserNamespace(const clang::Decl* decl)
    {
        for (const clang::DeclContext* scope = decl->getDeclContext(); scope != nullptr;
             scope = scope->getParent())
            if (const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope))
                if (ns->getName() == "__cflat_user") return true;
        return false;
    }

    clang::QualType StripIndirection(clang::QualType type)
    {
        while (!type.isNull())
        {
            type = type.getCanonicalType();
            if (type->isPointerType() || type->isReferenceType()
                || type->isMemberPointerType())
                type = type->getPointeeType();
            else if (const clang::ArrayType* array = type->getAsArrayTypeUnsafe())
                type = array->getElementType();
            else
                break;
        }
        return type;
    }

    // A not-yet-defined CFlat record named by TYPE, directly or through template arguments.
    const clang::CXXRecordDecl* IncompleteCflatRecord(clang::QualType type, unsigned depth)
    {
        type = StripIndirection(type);
        if (type.isNull() || depth > 8) return nullptr;
        const clang::CXXRecordDecl* record = type->getAsCXXRecordDecl();
        if (record == nullptr) return nullptr;
        if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record))
            if (const clang::CXXRecordDecl* found =
                    IncompleteCflatRecordIn(spec->getTemplateArgs().asArray(), depth + 1))
                return found;
        return !record->hasDefinition() && InCflatUserNamespace(record) ? record : nullptr;
    }

    const clang::CXXRecordDecl* IncompleteCflatRecordIn(
        llvm::ArrayRef<clang::TemplateArgument> args, unsigned depth)
    {
        for (const clang::TemplateArgument& arg : args)
        {
            const clang::CXXRecordDecl* found = nullptr;
            if (arg.getKind() == clang::TemplateArgument::Type)
                found = IncompleteCflatRecord(arg.getAsType(), depth);
            else if (arg.getKind() == clang::TemplateArgument::Pack)
                found = IncompleteCflatRecordIn(arg.pack_elements(), depth);
            if (found != nullptr) return found;
        }
        return nullptr;
    }

    // The incomplete CFlat record under TYPE when TYPE's record was left invalid by it.
    const clang::CXXRecordDecl* InvalidOverIncompleteCflatRecord(clang::QualType type)
    {
        type = StripIndirection(type);
        if (type.isNull()) return nullptr;
        const clang::CXXRecordDecl* record = type->getAsCXXRecordDecl();
        if (record == nullptr || !record->isInvalidDecl()) return nullptr;
        return IncompleteCflatRecord(type, 0);
    }

    class CountingDiagnosticConsumer : public clang::DiagnosticConsumer
    {
    public:
        unsigned errors = 0;
        std::string firstError;
        // First error naming a not-yet-defined CFlat record: no retry can complete that record.
        std::string incompleteRecordError;
        // Some error names a type built over a not-yet-defined CFlat record.
        bool incompleteRecordInvolved = false;
        // Lines of the newest interpreter input buffer that an error or its notes point at.
        std::string blamedBuffer;
        std::set<unsigned> blamedLines;

        void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                              const clang::Diagnostic& info) override
        {
            if (level == clang::DiagnosticsEngine::Note)
            {
                if (!inErrorGroup) return;
                AppendGroupLine(info);
                // This note names the incomplete type's declaration, not the use that failed.
                // Dropping that declaration only turns the retry into an undeclared name.
                if (info.getID() != clang::diag::note_forward_declaration) Blame(info);
                RecordFailedInstantiation(info);
                return;
            }
            inErrorGroup = level >= clang::DiagnosticsEngine::Error;
            if (!inErrorGroup) return;
            groupFunctions.clear();
            groupText.clear();
            AppendGroupLine(info);
            ++errors;
            Blame(info);
            for (unsigned i = 0; i < info.getNumArgs() && !incompleteRecordInvolved; ++i)
                if (info.getArgKind(i) == clang::DiagnosticsEngine::ak_qualtype)
                    incompleteRecordInvolved = InvalidOverIncompleteCflatRecord(
                        clang::QualType::getFromOpaquePtr(
                            reinterpret_cast<void*>(info.getRawArg(i)))) != nullptr;
            if (firstError.empty() || incompleteRecordError.empty())
            {
                llvm::SmallString<256> text;
                info.FormatDiagnostic(text);
                std::string message = text.str().str();
                if (incompleteRecordError.empty()
                    && message.find("incomplete type '__cflat_user::") != std::string::npos)
                    incompleteRecordError = message;
                if (firstError.empty()) firstError = std::move(message);
            }
        }

        // Unqualified names of members whose instantiation failed. Clang keeps such a member
        // as an invalid decl across the rollback, and a later use of it reaches CodeGen.
        std::set<std::string> failedMembers;
        std::vector<clang::FunctionDecl*> failedFunctions;
        // Every error with its notes, and per failed instantiation the lines of its own error
        // group, so a refusal can later be matched against the diagnostic that caused it.
        std::string allText;
        std::unordered_map<const clang::FunctionDecl*, std::string>* causes = nullptr;

    private:
        bool inErrorGroup = false;
        std::string groupText;
        std::vector<const clang::FunctionDecl*> groupFunctions;

        void AppendGroupLine(const clang::Diagnostic& info)
        {
            llvm::SmallString<256> text;
            info.FormatDiagnostic(text);
            const std::string line = text.str().str();
            groupText += (groupText.empty() ? "" : "\n") + line;
            if (allText.size() < 65536) allText += (allText.empty() ? "" : "\n") + line;
            if (causes != nullptr)
                for (const clang::FunctionDecl* function : groupFunctions)
                    (*causes)[function] += "\n" + line;
        }

        void RecordFailedInstantiation(const clang::Diagnostic& info)
        {
            llvm::SmallString<256> text;
            info.FormatDiagnostic(text);
            const std::string note = text.str().str();
            for (const char* lead : { "in instantiation of member function '",
                                      "in instantiation of function template specialization '" })
            {
                if (!note.starts_with(lead)) continue;
                // Only the instantiation chain failed; a "'g' declared here" note names a
                // callee that may be valid and must not be emptied.
                if (info.getNumArgs() > 0
                    && info.getArgKind(0) == clang::DiagnosticsEngine::ak_nameddecl)
                    if (auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(
                            reinterpret_cast<clang::NamedDecl*>(info.getRawArg(0))))
                    {
                        failedFunctions.push_back(function);
                        groupFunctions.push_back(function);
                        if (causes != nullptr)
                        {
                            std::string& cause = (*causes)[function];
                            cause += (cause.empty() ? "" : "\n") + groupText;
                        }
                    }
                const size_t begin = std::char_traits<char>::length(lead);
                const size_t end = note.find('\'', begin);
                if (end == std::string::npos) return;
                std::string name = note.substr(begin, end - begin);
                // Strip trailing template arguments, then take the last scope component.
                int depth = 0;
                size_t split = std::string::npos;
                for (size_t i = 0; i < name.size(); ++i)
                {
                    if (name[i] == '<' && !(i > 0 && name.compare(0, i, "operator") == 0)) ++depth;
                    else if (name[i] == '>' && depth > 0) --depth;
                    else if (depth == 0 && name.compare(i, 2, "::") == 0) split = i + 2;
                }
                if (split != std::string::npos) name = name.substr(split);
                const size_t args = name.find('<');
                if (args != std::string::npos && !name.starts_with("operator")) name.resize(args);
                if (!name.empty()) failedMembers.insert(name);
                return;
            }
        }

        void Blame(const clang::Diagnostic& info)
        {
            if (!info.getLocation().isValid() || !info.hasSourceManager()) return;
            const clang::SourceManager& sm = info.getSourceManager();
            const clang::SourceLocation loc = sm.getExpansionLoc(info.getLocation());
            const clang::PresumedLoc presumed = sm.getPresumedLoc(loc);
            if (presumed.isInvalid()) return;
            const std::string buffer = presumed.getFilename();
            if (!buffer.starts_with("input_line_")) return;
            if (buffer != blamedBuffer)
            {
                // Interpreter buffers are numbered; only the newest one is the failing chunk.
                if (!blamedBuffer.empty()
                    && std::strtoul(buffer.c_str() + 11, nullptr, 10)
                           < std::strtoul(blamedBuffer.c_str() + 11, nullptr, 10))
                    return;
                blamedBuffer = buffer;
                blamedLines.clear();
            }
            blamedLines.insert(presumed.getLine());
        }
    };

    struct DiagnosticScope
    {
        clang::DiagnosticsEngine& diagnostics;
        clang::DiagnosticConsumer* previous;
        // setClient(..., false) deletes an owned client, so hold it for the restore.
        std::unique_ptr<clang::DiagnosticConsumer> ownedPrevious;
        CountingDiagnosticConsumer consumer;

        explicit DiagnosticScope(clang::DiagnosticsEngine& d)
            : diagnostics(d), previous(d.getClient()), ownedPrevious(d.takeClient())
        {
            diagnostics.Reset(/*soft*/ true);
            diagnostics.setSuppressAllDiagnostics(false);
            diagnostics.setClient(&consumer, /*ShouldOwnClient*/ false);
        }

        ~DiagnosticScope()
        {
            if (ownedPrevious != nullptr)
                diagnostics.setClient(ownedPrevious.release(), /*ShouldOwnClient*/ true);
            else
                diagnostics.setClient(previous, /*ShouldOwnClient*/ false);
            diagnostics.setSuppressAllDiagnostics(true);
            diagnostics.Reset(/*soft*/ true);
        }
    };

    // Every header an #include named, and who named it. A header an include guard skips
    // still gets its edge, so a later chunk's includes reach what an earlier chunk parsed.
    struct IncludeGraph
    {
        std::unordered_map<const clang::FileEntry*, std::vector<const clang::FileEntry*>> edges;
        std::unordered_set<const clang::FileEntry*> targets;
        // `<name>` -> the header the first `#include <name>` found; resolves a root such as the
        // request prologue's `<new>`.
        std::unordered_map<std::string, const clang::FileEntry*> angled;
    };

    class IncludeCollector : public clang::PPCallbacks
    {
    public:
        clang::Preprocessor& pp;
        std::vector<std::string>& files;
        IncludeGraph& graph;

        IncludeCollector(clang::Preprocessor& p, std::vector<std::string>& f, IncludeGraph& g)
            : pp(p), files(f), graph(g) {}

        void FileChanged(clang::SourceLocation loc, FileChangeReason reason,
                         clang::SrcMgr::CharacteristicKind, clang::FileID) override
        {
            if (reason != EnterFile) return;
            llvm::StringRef file = pp.getSourceManager().getFilename(loc);
            if (!file.empty()) files.push_back(file.str());
        }

        void InclusionDirective(clang::SourceLocation hashLoc, const clang::Token&,
                                llvm::StringRef spelled, bool angled, clang::CharSourceRange,
                                clang::OptionalFileEntryRef file, llvm::StringRef,
                                llvm::StringRef, const clang::Module*, bool,
                                clang::SrcMgr::CharacteristicKind) override
        {
            if (!file) return;
            const clang::FileEntry* target = &file->getFileEntry();
            graph.targets.insert(target);
            clang::SourceManager& sm = pp.getSourceManager();
            if (auto includer = sm.getFileEntryRefForID(sm.getFileID(sm.getExpansionLoc(hashLoc))))
                graph.edges[&includer->getFileEntry()].push_back(target);
            if (angled) graph.angled.emplace("<" + spelled.str() + ">", target);
        }
    };

    /*
     * A request chunk is one Interpreter::Parse, and a PTU with any error is rolled back whole.
     * A request TU instead error-recovers each top-level ODR-use on its own. Emulate that: split
     * the chunk into top-level declarations by brace depth, drop the ones an error blamed, and
     * let the caller re-parse. Marker typedefs and preprocessor lines are never dropped - an
     * error there is a real request failure. False when nothing droppable was blamed.
     */
    bool DropBlamedDeclarations(const std::string& source, const std::set<unsigned>& blamed,
                                const std::set<std::string>& failedMembers,
                                std::string& kept, std::string& dropped)
    {
        kept.clear();
        dropped.clear();
        bool droppedAny = false;
        std::string segment;
        bool segmentBlamed = false;
        int depth = 0;
        unsigned line = 0;
        size_t pos = 0;
        while (pos < source.size())
        {
            size_t end = source.find('\n', pos);
            if (end == std::string::npos) end = source.size(); else ++end;
            const std::string text = source.substr(pos, end - pos);
            pos = end;
            ++line;
            for (char c : text)
            {
                if (c == '{') ++depth;
                else if (c == '}') --depth;
            }
            segment += text;
            if (blamed.count(line) != 0) segmentBlamed = true;
            if (depth > 0) continue;
            depth = 0;
            const size_t first = segment.find_first_not_of(" \t\r\n");
            const bool protectedSegment = first == std::string::npos || segment[first] == '#'
                || segment.find("typedef ") != std::string::npos;
            if (!protectedSegment && !segmentBlamed)
                for (const std::string& member : failedMembers)
                    if (segment.find("::" + member + ")") != std::string::npos
                        || segment.find("->" + member + "(") != std::string::npos
                        || segment.find("." + member + "(") != std::string::npos)
                    {
                        segmentBlamed = true;
                        break;
                    }
            if (segmentBlamed && !protectedSegment)
            {
                dropped += segment;
                droppedAny = true;
            }
            else
            {
                if (segmentBlamed) return false;
                kept += segment;
            }
            segment.clear();
            segmentBlamed = false;
        }
        kept += segment;
        return droppedAny;
    }

    /*
     * A failed Parse does not roll back everything: an explicit instantiation stays done, and
     * CodeGen keeps the definitions it already emitted. So a retry drops every explicit
     * instantiation (it ran already) and renames the ODR-use helpers, which exist only to force
     * instantiation, so they cannot collide with their emitted first copies.
     */
    std::string PrepareRetryChunk(const std::string& source, unsigned attempt)
    {
        std::string result;
        size_t pos = 0;
        while (pos < source.size())
        {
            size_t end = source.find('\n', pos);
            end = end == std::string::npos ? source.size() : end + 1;
            if (source.compare(pos, 15, "template class ") != 0)
                result.append(source, pos, end - pos);
            pos = end;
        }
        const std::string prefix = "__cflat_inc_";
        const std::string retryTag = "r" + std::to_string(attempt) + "_";
        pos = 0;
        while ((pos = result.find(prefix, pos)) != std::string::npos)
        {
            size_t cursor = pos + prefix.size();
            while (cursor < result.size() && std::isdigit((unsigned char)result[cursor])) ++cursor;
            if (cursor > pos + prefix.size() && cursor < result.size() && result[cursor] == '_')
            {
                // Replace an earlier retry's tag ("r1_") so a second retry gets fresh names too.
                size_t tagEnd = cursor + 1;
                if (tagEnd < result.size() && result[tagEnd] == 'r')
                {
                    size_t digits = tagEnd + 1;
                    while (digits < result.size() && std::isdigit((unsigned char)result[digits]))
                        ++digits;
                    if (digits > tagEnd + 1 && digits < result.size() && result[digits] == '_')
                        tagEnd = digits + 1;
                }
                // "use" helpers and "thk" thunk-name suffixes (ExtractRequest::cxxThunkSuffix).
                if (result.compare(tagEnd, 3, "use") == 0 || result.compare(tagEnd, 3, "thk") == 0)
                    result.replace(cursor + 1, tagEnd - (cursor + 1), retryTag);
            }
            pos = cursor;
        }
        return result;
    }

    struct ContainsErrors : clang::RecursiveASTVisitor<ContainsErrors>
    {
        bool found = false;
        bool VisitExpr(clang::Expr* expr)
        {
            if (expr->containsErrors()) found = true;
            return !found;
        }
    };

    /*
     * First consumer in the Interpreter's chain, so it runs before CodeGen. It records every decl
     * Sema announces while a sink is attached, and it neutralizes a generated request helper
     * (a "__cflat_" ODR-use) that holds error nodes without an error: a member whose body failed
     * to instantiate in an earlier chunk stays invalid, and a later use of it is silent but
     * cannot be lowered. The helper only exists to force instantiation, so dropping its
     * initializer or body loses nothing.
     */
    class ChunkConsumer : public clang::ASTConsumer
    {
    public:
        std::vector<clang::Decl*>* sink = nullptr;
        clang::ASTContext* context = nullptr;
        std::vector<std::string> neutralized;
        std::unordered_map<const clang::FunctionDecl*, std::string>* poisoned = nullptr;
        const std::unordered_map<const clang::FunctionDecl*, std::string>* causes = nullptr;

        bool HandleTopLevelDecl(clang::DeclGroupRef group) override
        {
            for (clang::Decl* decl : group)
            {
                Neutralize(decl);
                if (sink != nullptr) sink->push_back(decl);
            }
            return true;
        }

    private:
        // Error nodes in a statement tree. An Expr's containsErrors() already covers its
        // subexpressions, so only statements are descended.
        static bool StmtHasErrors(const clang::Stmt* stmt)
        {
            if (stmt == nullptr) return false;
            if (const auto* expr = llvm::dyn_cast<clang::Expr>(stmt)) return expr->containsErrors();
            for (const clang::Stmt* child : stmt->children())
                if (StmtHasErrors(child)) return true;
            return false;
        }

        /*
         * An instantiated body can come out with error nodes and NO error diagnostic (clang
         * instantiated it under a SFINAE trap, or an earlier chunk's rollback kept it). CodeGen
         * then raises "cannot compile this l-value expression yet", drops the PTU's module, and
         * the Interpreter dereferences the null module. Empty the body before CodeGen sees it and
         * poison it, so every later body that reaches it is refused instead of calling nothing.
         */
        void EmptyErroneousInstantiation(clang::Decl* decl)
        {
            auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if (function == nullptr || context == nullptr
                || !function->doesThisDeclarationHaveABody()
                || function->getTemplateInstantiationPattern() == nullptr
                || !StmtHasErrors(function->getBody()))
                return;
            function->setBody(clang::CompoundStmt::CreateEmpty(*context, /*NumStmts*/ 0,
                                                               /*HasFPFeatures*/ false));
            if (poisoned != nullptr)
            {
                std::string reason = "clang reported an error inside the body it generated for '"
                    + function->getQualifiedNameAsString() + "'";
                if (causes != nullptr)
                    if (auto cause = causes->find(function); cause != causes->end())
                        reason += "\n" + cause->second;
                poisoned->emplace(function, reason);
            }
            neutralized.push_back(function->getQualifiedNameAsString());
        }

        // Only a body over an incomplete CFlat record (or a generated `__cflat_` helper, always
        // scanned) can reach one of its broken instantiations; other bodies skip the scan.
        static bool MentionsIncompleteCflatRecord(const clang::FunctionDecl* function)
        {
            if (const clang::TemplateArgumentList* args =
                    function->getTemplateSpecializationArgs())
                if (IncompleteCflatRecordIn(args->asArray(), 0) != nullptr) return true;
            for (const clang::DeclContext* scope = function->getDeclContext(); scope != nullptr;
                 scope = scope->getParent())
                if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(scope))
                    if (IncompleteCflatRecordIn(spec->getTemplateArgs().asArray(), 0) != nullptr)
                        return true;
            return false;
        }

        // First record, among the types a body names, that is invalid over an incomplete CFlat record.
        struct InvalidRecordUse : clang::RecursiveASTVisitor<InvalidRecordUse>
        {
            const clang::CXXRecordDecl* invalid = nullptr;
            const clang::CXXRecordDecl* incomplete = nullptr;
            void Check(clang::QualType type)
            {
                if (invalid != nullptr) return;
                incomplete = InvalidOverIncompleteCflatRecord(type);
                if (incomplete != nullptr)
                    invalid = StripIndirection(type)->getAsCXXRecordDecl();
            }
            bool VisitExpr(clang::Expr* expr)
            {
                Check(expr->getType());
                return invalid == nullptr;
            }
            bool VisitValueDecl(clang::ValueDecl* value)
            {
                Check(value->getType());
                return invalid == nullptr;
            }
        };

        /*
         * Under a SFINAE trap, instantiating a node or value record over an incomplete CFlat
         * record (std::list's __list_node<Leaf>) marks it invalid with no error counted. A body
         * using it then reaches CodeGen, whose layout query asserts or crashes. Empty and poison
         * that body, and report the incomplete record so this chunk fails before CodeGen runs.
         */
        void RefuseInvalidRecordUse(clang::Decl* decl)
        {
            auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if (function == nullptr || context == nullptr
                || !function->doesThisDeclarationHaveABody())
                return;
            const clang::IdentifierInfo* id = function->getIdentifier();
            const bool helper = id != nullptr && id->getName().starts_with("__cflat_");
            if (!helper && !MentionsIncompleteCflatRecord(function)) return;
            InvalidRecordUse scan;
            for (const clang::ParmVarDecl* param : function->parameters())
                scan.Check(param->getType());
            scan.Check(function->getReturnType());
            if (scan.invalid == nullptr) scan.TraverseStmt(function->getBody());
            if (scan.invalid == nullptr) return;
            const std::string name = function->getQualifiedNameAsString();
            const std::string incomplete = scan.incomplete->getQualifiedNameAsString();
            function->setBody(clang::CompoundStmt::CreateEmpty(*context, /*NumStmts*/ 0,
                                                               /*HasFPFeatures*/ false));
            if (poisoned != nullptr)
                poisoned->emplace(function, std::format(
                    "the body clang generated for '{}' uses '{}', which is invalid because "
                    "'{}' is incomplete", name, scan.invalid->getQualifiedNameAsString(),
                    incomplete));
            neutralized.push_back(name);
            clang::DiagnosticsEngine& diagnostics = context->getDiagnostics();
            const unsigned diagId = diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                "incomplete type '%0' used in the definition of '%1'");
            clang::SourceLocation where = function->getPointOfInstantiation();
            if (where.isInvalid()) where = function->getLocation();
            diagnostics.Report(where, diagId) << incomplete << name;
        }

        void Neutralize(clang::Decl* decl)
        {
            // First, so a body with error nodes over an invalid record still reports it.
            RefuseInvalidRecordUse(decl);
            EmptyErroneousInstantiation(decl);
            auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
            const clang::IdentifierInfo* id = named != nullptr ? named->getIdentifier() : nullptr;
            if (id == nullptr || context == nullptr || !id->getName().starts_with("__cflat_"))
                return;
            if (auto* var = llvm::dyn_cast<clang::VarDecl>(decl))
            {
                if (var->getInit() != nullptr && var->getInit()->containsErrors())
                {
                    var->setInit(nullptr);
                    neutralized.push_back(var->getNameAsString());
                }
                return;
            }
            auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if (function == nullptr || !function->doesThisDeclarationHaveABody()) return;
            ContainsErrors scan;
            scan.TraverseStmt(function->getBody());
            if (!scan.found) return;
            function->setBody(clang::CompoundStmt::CreateEmpty(*context, /*NumStmts*/ 0,
                                                               /*HasFPFeatures*/ false));
            neutralized.push_back(function->getNameAsString());
        }
    };

    // The Interpreter's consumer is a MultiplexConsumer with no public way to add one; its
    // list is protected, so reach it through a derived accessor.
    struct MultiplexAccess : clang::MultiplexConsumer
    {
        static std::vector<std::unique_ptr<clang::ASTConsumer>>& ListOf(
            clang::MultiplexConsumer& consumer)
        {
            return static_cast<MultiplexAccess&>(consumer).Consumers;
        }
    };

    /*
     * Sema hands CodeGen an instantiated static data member the moment it instantiates it, with
     * no error check in between. An explicit instantiation over an incomplete CFlat record (e.g.
     * a deque's `_Block_size = sizeof(T) ...`) yields a broken one, and emitting it crashes.
     * The Interpreter's consumers sit behind this guard, which drops such members.
     */
    class StaticMemberGuard : public clang::MultiplexConsumer
    {
    public:
        using clang::MultiplexConsumer::MultiplexConsumer;

        void HandleCXXStaticMemberVarInstantiation(clang::VarDecl* var) override
        {
            const bool brokenInit = var->getInit() != nullptr && var->getInit()->containsErrors();
            if (!var->isInvalidDecl() && !brokenInit)
            {
                clang::MultiplexConsumer::HandleCXXStaticMemberVarInstantiation(var);
                return;
            }
            // A surviving use still takes its address. Without an initializer that is a plain
            // external declaration, not a constant CodeGen cannot evaluate.
            if (var->getInit() != nullptr) var->setInit(nullptr);
        }
    };

    std::string ErrorText(llvm::Error error)
    {
        return llvm::toString(std::move(error));
    }

    std::vector<std::string> InterpreterArgs(const std::vector<std::string>& args)
    {
        std::vector<std::string> result;
        result.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == "-fsyntax-only") continue;
            if (args[i] == "-x" && i + 1 < args.size())
            {
                ++i;
                continue;
            }
            if (args[i] == "-Xclang" && i + 1 < args.size())
            {
                result.push_back(args[++i]);
                continue;
            }
            result.push_back(args[i]);
        }
        return result;
    }

    bool MergeBitcode(const std::string& first, const std::string& second,
                      std::string& merged)
    {
        llvm::LLVMContext context;
        auto left = llvm::parseBitcodeFile(llvm::MemoryBufferRef(first, "cflat-cxx-left"),
                                           context);
        auto right = llvm::parseBitcodeFile(llvm::MemoryBufferRef(second, "cflat-cxx-right"),
                                            context);
        if (!left || !right) return false;
        llvm::Linker linker(*left.get());
        if (linker.linkInModule(std::move(*right), llvm::Linker::OverrideFromSrc)) return false;
        llvm::raw_string_ostream stream(merged);
        llvm::WriteBitcodeToFile(*left.get(), stream);
        stream.flush();
        return true;
    }

    std::string SerializeModule(llvm::Module& module)
    {
        std::string result;
        llvm::raw_string_ostream stream(result);
        llvm::WriteBitcodeToFile(module, stream);
        stream.flush();
        return result;
    }
}

struct CxxIncrementalGroup::Impl
{
    std::unique_ptr<clang::Interpreter> interpreter;
    clang::TranslationUnitDecl* headerRoot = nullptr;
    llvm::Module* headerModule = nullptr;
    std::vector<std::string> includedFiles;
    IncludeGraph includeGraph;
    std::unordered_set<std::string> prefixSources;
    bool verbose = false;
    std::unordered_map<std::string, cflat_cinterop::ExtractResult> wrapperResults;
    std::unordered_map<std::string, cflat_cinterop::ExtractResult> typeResults;
    std::unordered_map<std::string, clang::TranslationUnitDecl*> typeRoots;
    // Include preludes already committed as their own chunk, with the root they produced.
    std::unordered_map<std::string, clang::TranslationUnitDecl*> preludeRoots;
    ChunkConsumer* announcer = nullptr;   // owned by the Interpreter's consumer chain
    unsigned wrapperRenames = 0;
    // Specializations whose bodies were emptied after a failed instantiation, with the reason.
    std::unordered_map<const clang::FunctionDecl*, std::string> poisoned;
    // Per failed instantiation, the clang error group (error plus notes) that named it.
    std::unordered_map<const clang::FunctionDecl*, std::string> causes;
    // Every error and note the newest ParseRequest reported, across its recovery attempts.
    std::string lastDiagnostics;
    bool headerHadDiagnostics = false;

    ~Impl()
    {
        if (headerHadDiagnostics && interpreter != nullptr)
        {
            // Clang CodeGeneratorImpl asserts on deferred inline members after a failed header
            // parse; DiagnosticScope has reset the error state, so only this path must leak it.
            (void)interpreter.release();
        }
    }
};

CxxIncrementalGroup::CxxIncrementalGroup(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

CxxIncrementalGroup::~CxxIncrementalGroup() = default;

std::unique_ptr<CxxIncrementalGroup> CxxIncrementalGroup::Create(
    const std::vector<std::string>& args, const std::string& headerSource,
    bool verbose, std::string& error, bool tolerateDiagnostics)
{
    std::vector<std::string> storage = InterpreterArgs(args);
    std::vector<const char*> cargs;
    cargs.reserve(storage.size());
    for (const std::string& arg : storage) cargs.push_back(arg.c_str());

    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();

    clang::IncrementalCompilerBuilder builder;
    builder.SetCompilerArgs(cargs);
    auto compiler = builder.CreateCpp();
    if (!compiler)
    {
        error = ErrorText(compiler.takeError());
        return nullptr;
    }
    auto executorBuilder = std::make_unique<clang::IncrementalExecutorBuilder>();
    executorBuilder->IE = std::make_unique<ParseOnlyExecutor>();
    auto interpreter = clang::Interpreter::create(std::move(*compiler), std::move(executorBuilder));
    if (!interpreter)
    {
        error = ErrorText(interpreter.takeError());
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->interpreter = std::move(*interpreter);
    impl->verbose = verbose;
    if (auto* multiplex = dynamic_cast<clang::MultiplexConsumer*>(
            &impl->interpreter->getCompilerInstance()->getASTConsumer()))
    {
        auto recorder = std::make_unique<ChunkConsumer>();
        recorder->context = &impl->interpreter->getCompilerInstance()->getASTContext();
        recorder->poisoned = &impl->poisoned;
        recorder->causes = &impl->causes;
        impl->announcer = recorder.get();
        auto& consumers = MultiplexAccess::ListOf(*multiplex);
        auto guarded = std::make_unique<StaticMemberGuard>(std::move(consumers));
        consumers.clear();
        consumers.push_back(std::move(recorder));
        consumers.push_back(std::move(guarded));
    }
    {
        DiagnosticScope diagnostics(impl->interpreter->getCompilerInstance()->getDiagnostics());
        impl->interpreter->getCompilerInstance()->getPreprocessor().addPPCallbacks(
            std::make_unique<IncludeCollector>(
                impl->interpreter->getCompilerInstance()->getPreprocessor(),
                impl->includedFiles, impl->includeGraph));
        auto ptu = impl->interpreter->Parse(headerSource);
        impl->headerHadDiagnostics = diagnostics.consumer.errors != 0;
        if (!ptu)
        {
            error = ErrorText(ptu.takeError());
            if (error.empty()) error = diagnostics.consumer.firstError;
            return nullptr;
        }
        if (!tolerateDiagnostics && diagnostics.consumer.errors != 0)
        {
            error = diagnostics.consumer.firstError;
            return nullptr;
        }
        impl->headerRoot = (*ptu).TUPart;
        impl->headerModule = (*ptu).TheModule.get();
    }
    return std::unique_ptr<CxxIncrementalGroup>(
        new CxxIncrementalGroup(std::move(impl)));
}

bool CxxIncrementalGroup::HarvestHeader(const cflat_cinterop::ExtractRequest& req,
                                         cflat_cinterop::ExtractResult& out,
                                         std::string& error)
{
    if (impl_ == nullptr || impl_->headerRoot == nullptr)
    {
        error = "incremental header parse returned no translation-unit part";
        return false;
    }
    DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
    diagnostics.consumer.causes = &impl_->causes;
    const bool harvested = cflat_cinterop::ExtractCxxIncremental(
        req, *impl_->interpreter->getCompilerInstance(), impl_->headerRoot,
        impl_->headerRoot, {}, impl_->headerModule, out, error, true);
    out.includedFiles = impl_->includedFiles;
    if (harvested && impl_->headerModule != nullptr)
    {
        std::string headerBitcode = SerializeModule(*impl_->headerModule);
        if (!headerBitcode.empty() && !out.bitcode.empty())
        {
            std::string merged;
            if (MergeBitcode(headerBitcode, out.bitcode, merged)) out.bitcode = std::move(merged);
        }
        else if (!headerBitcode.empty()) out.bitcode = std::move(headerBitcode);
    }
    return harvested;
}

bool CxxIncrementalGroup::PrecheckSpelling(const std::string& spelling, std::string& error)
{
    static unsigned nextId = 0;
    const std::string typedefName = "__cflat_precheck_" + std::to_string(nextId++);
    DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
    auto ptu = impl_->interpreter->Parse(
        "typedef " + spelling + " " + typedefName + ";\n");
    if (!ptu)
    {
        // Consume the Expected either way; an unchecked one aborts under LLVM assertions.
        const std::string parseError = ErrorText(ptu.takeError());
        error = diagnostics.consumer.firstError;
        if (error.empty()) error = parseError;
        return false;
    }

    clang::TypedefNameDecl* typedefDecl = nullptr;
    for (clang::Decl* decl : (*ptu).TUPart->decls())
    {
        auto* candidate = llvm::dyn_cast<clang::TypedefNameDecl>(decl);
        if (candidate != nullptr && candidate->getNameAsString() == typedefName)
        {
            typedefDecl = candidate;
            break;
        }
    }
    if (typedefDecl == nullptr)
    {
        error = std::format("C++ type '{}' could not be prechecked", spelling);
        return false;
    }

    return diagnostics.consumer.errors == 0;
}

bool CxxIncrementalGroup::HasPrefixSource(const std::string& source) const
{
    return !source.empty() && impl_->prefixSources.count(source) != 0;
}

std::string CxxIncrementalGroup::UnseenPrefixSource(const std::string& source) const
{
    std::string result = source;
    std::vector<std::string> known(impl_->prefixSources.begin(), impl_->prefixSources.end());
    std::sort(known.begin(), known.end(), [](const auto& a, const auto& b) {
        return a.size() > b.size();
    });
    for (const std::string& prefix : known)
    {
        size_t pos = 0;
        while (!prefix.empty() && (pos = result.find(prefix, pos)) != std::string::npos)
            result.erase(pos, prefix.size());
    }
    return result;
}

void CxxIncrementalGroup::RememberPrefixSource(const std::string& source)
{
    if (!source.empty()) impl_->prefixSources.insert(source);
}

bool CxxIncrementalGroup::ParseRequest(const cflat_cinterop::ExtractRequest& req,
                                       const std::string& source,
                                       cflat_cinterop::ExtractResult& out,
                                       std::string& error)
{
    impl_->lastDiagnostics.clear();
    std::string typeKey;
    for (const auto& request : req.cxxTypeRequests)
        typeKey += request.cflatName + "\n";
    const std::string wrapperName = req.cxxFunctionWrapperNames.size() == 1
        ? req.cxxFunctionWrapperNames.front() : std::string{};
    const bool wrapperBatch = req.cxxWrapperBatch || !wrapperName.empty();
    if (!wrapperName.empty())
    {
        auto found = impl_->wrapperResults.find(wrapperName);
        if (found != impl_->wrapperResults.end())
        {
            out = found->second;
            return true;
        }
    }
    // Type requests recover per declaration like a request TU; a wrapper request is all or nothing.
    const bool recoverDeclarations = !typeKey.empty() && !wrapperBatch;
    std::string chunk = source;
    /*
     * An earlier batch chunk may already define this wrapper (a default-argument wrapper whose
     * member was refused at the time). Parse under a fresh name, then restore the requested name
     * in the result; both definitions are weak and identical.
     */
    // Wrapper names re-spelled in this chunk, fresh name -> requested name.
    std::vector<std::pair<std::string, std::string>> renamedWrappers;
    auto identChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    // Rewrites NAME and its NAME_cpp helper, never a longer identifier that starts with NAME.
    auto rewrite = [&](const std::string& name, const std::string& fresh) {
        bool changed = false;
        for (size_t pos = 0; (pos = chunk.find(name, pos)) != std::string::npos;)
        {
            size_t after = pos + name.size();
            if (chunk.compare(after, 4, "_cpp") == 0) after += 4;
            if ((pos != 0 && identChar(chunk[pos - 1]))
                || (after < chunk.size() && identChar(chunk[after])))
            {
                pos += name.size();
                continue;
            }
            chunk.replace(pos, name.size(), fresh);
            pos += fresh.size();
            changed = true;
        }
        return changed;
    };
    /*
     * A failed attempt's definitions stay emitted in the Interpreter's CodeGen module, so every
     * retry re-spells all earlier renames too, not just the new one.
     */
    auto renameWrapper = [&](const std::string& name) {
        auto freshName = [&](const std::string& original) {
            return std::format("{}__cflat_again{}", original, impl_->wrapperRenames++);
        };
        const std::string fresh = freshName(name);
        if (!rewrite(name, fresh)) return false;
        for (auto& [renamed, original] : renamedWrappers)
        {
            const std::string next = freshName(original);
            rewrite(renamed, next);
            renamed = next;
        }
        renamedWrappers.emplace_back(fresh, name);
        return true;
    };
    clang::ASTContext& context = impl_->interpreter->getCompilerInstance()->getASTContext();
    std::set<std::string> chunkWrappers;
    for (size_t pos = 0; (pos = chunk.find("__cflat_dflt_", pos)) != std::string::npos;)
    {
        size_t end = pos;
        while (end < chunk.size() && identChar(chunk[end])) ++end;
        std::string name = chunk.substr(pos, end - pos);
        if (name.ends_with("_cpp")) name.resize(name.size() - 4);
        chunkWrappers.insert(std::move(name));
        pos = end;
    }
    for (const std::string& name : chunkWrappers)
        if (!context.getTranslationUnitDecl()->lookup(
                clang::DeclarationName(&context.Idents.get(name))).empty())
            renameWrapper(name);
    clang::TranslationUnitDecl* preludeRoot = nullptr;
    {
        /*
         * Commit the leading #include lines first, in their own chunk. A failed chunk that is the
         * first to include a header rolls back the header's decls, but its include guard stays
         * set, so every later chunk would see the header as empty ("undeclared identifier 'std'").
         */
        size_t bodyStart = 0;
        while (bodyStart < chunk.size() && (chunk[bodyStart] == '#' || chunk[bodyStart] == '\n'))
        {
            const size_t end = chunk.find('\n', bodyStart);
            bodyStart = end == std::string::npos ? chunk.size() : end + 1;
        }
        const std::string prelude = chunk.substr(0, bodyStart);
        if (!prelude.empty() && bodyStart < chunk.size())
        {
            auto known = impl_->preludeRoots.find(prelude);
            if (known == impl_->preludeRoots.end())
            {
                DiagnosticScope diagnostics(
                    impl_->interpreter->getCompilerInstance()->getDiagnostics());
                auto ptu = impl_->interpreter->Parse(prelude);
                if (!ptu)
                {
                    const std::string parseError = ErrorText(ptu.takeError());
                    error = diagnostics.consumer.firstError;
                    if (error.empty()) error = parseError;
                    return false;
                }
                known = impl_->preludeRoots.emplace(prelude, (*ptu).TUPart).first;
            }
            preludeRoot = known->second;
            chunk.erase(0, bodyStart);
        }
    }
    clang::PartialTranslationUnit* parsed = nullptr;
    std::vector<clang::Decl*> announced;
    unsigned parsedAttempt = 0;
    // A drop over a not-yet-defined CFlat record: the request reports that error as firstError.
    // Any other drop is an ordinary recovery.
    std::string recoveredError;
    for (unsigned attempt = 0;; ++attempt)
    {
        DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
        diagnostics.consumer.causes = &impl_->causes;
        // A retry chunk has its explicit instantiations removed (PrepareRetryChunk).
        const bool explicitInstantiation = chunk.starts_with("template class ")
            || chunk.find("\ntemplate class ") != std::string::npos;
        announced.clear();
        if (impl_->announcer != nullptr) impl_->announcer->sink = &announced;
        auto ptu = impl_->interpreter->Parse(chunk);
        if (impl_->announcer != nullptr) impl_->announcer->sink = nullptr;
        if (impl_->verbose && impl_->announcer != nullptr)
            for (const std::string& name : impl_->announcer->neutralized)
                std::cout << "[verbose] incremental request helper " << name
                          << " uses a member that failed to instantiate earlier; emptied\n";
        if (impl_->announcer != nullptr) impl_->announcer->neutralized.clear();
        if (ptu)
        {
            // An error raised while instantiating at the end of the chunk does not fail Parse.
            // One over an incomplete CFlat record leaves nothing sound to harvest.
            if (explicitInstantiation && !diagnostics.consumer.incompleteRecordError.empty())
            {
                error = diagnostics.consumer.incompleteRecordError;
                impl_->lastDiagnostics += (impl_->lastDiagnostics.empty() ? "" : "\n")
                    + diagnostics.consumer.allText;
                return false;
            }
            parsed = &*ptu;
            parsedAttempt = attempt;
            break;
        }
        // Consume the Expected either way; an unchecked one aborts under LLVM assertions.
        const std::string parseError = ErrorText(ptu.takeError());
        error = diagnostics.consumer.firstError;
        if (error.empty()) error = parseError;
        impl_->lastDiagnostics += (impl_->lastDiagnostics.empty() ? "" : "\n")
            + diagnostics.consumer.allText;
        /*
         * An earlier chunk already defines a generated wrapper this chunk repeats (a batch that
         * emitted it while its member was refused). Parse the repeat under a fresh name.
         */
        static const std::string kRedefinition = "redefinition of '__cflat_";
        if (!wrapperBatch && error.starts_with(kRedefinition)) return false;
        if (wrapperBatch && attempt < 16 && error.starts_with(kRedefinition))
        {
            const size_t start = kRedefinition.size() - std::string("__cflat_").size();
            const size_t end = error.find('\'', start);
            std::string name = error.substr(start, end - start);
            if (name.ends_with("_cpp")) name.resize(name.size() - 4);
            if (renameWrapper(name)) continue;
        }
        // The failed instantiations keep bodies with error nodes, CodeGen has no guard for
        // them, and it may already have them queued. Empty them, as the request-TU sweep does.
        auto emptyFailedFunctions = [&] {
            clang::ASTContext& context =
                impl_->interpreter->getCompilerInstance()->getASTContext();
            for (clang::FunctionDecl* function : diagnostics.consumer.failedFunctions)
                if (function->doesThisDeclarationHaveABody())
                {
                    function->setBody(clang::CompoundStmt::CreateEmpty(
                        context, /*NumStmts*/ 0, /*HasFPFeatures*/ false));
                    auto cause = impl_->causes.find(function);
                    impl_->poisoned.emplace(function, cause != impl_->causes.end()
                                                          ? cause->second : error);
                }
        };
        // The explicit instantiation left records invalid over an incomplete CFlat record; a
        // retry would harvest, and emit, bodies built on them. Refuse the request instead.
        if (explicitInstantiation && (!diagnostics.consumer.incompleteRecordError.empty()
                                      || diagnostics.consumer.incompleteRecordInvolved))
        {
            emptyFailedFunctions();
            if (!diagnostics.consumer.incompleteRecordError.empty())
                error = diagnostics.consumer.incompleteRecordError;
            return false;
        }
        std::string kept, dropped;
        if (!recoverDeclarations || attempt >= 3
            || !DropBlamedDeclarations(chunk, diagnostics.consumer.blamedLines,
                                       diagnostics.consumer.failedMembers, kept, dropped))
            return false;
        emptyFailedFunctions();
        if (recoveredError.empty()) recoveredError = diagnostics.consumer.incompleteRecordError;
        if (impl_->verbose)
            std::cout << std::format("[verbose] incremental request dropped declarations after "
                                     "'{}':\n{}", error, dropped);
        chunk = PrepareRetryChunk(kept, attempt + 1);
        // The failed attempt's default-argument wrappers stay emitted in CodeGen's module too;
        // a kept one re-defined under its old name fails CodeGen and crashes GenModule.
        for (const std::string& name : chunkWrappers)
            if (std::none_of(renamedWrappers.begin(), renamedWrappers.end(),
                             [&](const auto& entry) { return entry.second == name; }))
                renameWrapper(name);
        for (auto& [renamed, original] : renamedWrappers)
        {
            const std::string next =
                std::format("{}__cflat_again{}", original, impl_->wrapperRenames++);
            rewrite(renamed, next);
            renamed = next;
        }
    }
    error.clear();
    cflat_cinterop::ExtractRequest effective = req;
    effective.poisonedFunctions = &impl_->poisoned;
    effective.errorCauses = &impl_->causes;
    // A retry renamed the thunks with its tag; the extractor looks them up by that name.
    if (parsedAttempt > 0 && !effective.cxxThunkSuffix.empty())
        effective.cxxThunkSuffix = PrepareRetryChunk(effective.cxxThunkSuffix, parsedAttempt);
    for (auto& name : effective.cxxFunctionWrapperNames)
        for (const auto& [renamed, original] : renamedWrappers)
            if (name == original) name = renamed;
    if (wrapperBatch)
    {
        effective.requireInScope = false;
        effective.checkHeaderScope = false;
    }
    if (!wrapperName.empty())
    {
        effective.emitDefinitions = true;
        effective.assumeInlineDefinitions = false;
    }
    std::vector<clang::TranslationUnitDecl*> extraRoots;
    if (req.emitDefinitions && !typeKey.empty())
    {
        auto found = impl_->typeRoots.find(typeKey);
        if (found != impl_->typeRoots.end()) extraRoots.push_back(found->second);
    }
    const bool harvested = cflat_cinterop::ExtractCxxIncremental(
        effective, *impl_->interpreter->getCompilerInstance(), parsed->TUPart,
        wrapperBatch ? nullptr : impl_->headerRoot, extraRoots,
        parsed->TheModule.get(), out, error, false, wrapperBatch ? nullptr : preludeRoot,
        &announced);
    if (harvested && out.firstError.empty()) out.firstError = recoveredError;
    if (harvested && !renamedWrappers.empty())
    {
        llvm::Module* module = parsed->TheModule.get();
        for (const auto& [renamed, original] : renamedWrappers)
        {
            for (auto& sig : out.sigs)
            {
                if (sig.name == renamed) sig.name = original;
                if (sig.linkageName == renamed) sig.linkageName = original;
            }
            for (auto& name : out.droppedCxxDefaultWrappers)
                if (name == renamed) name = original;
            if (module != nullptr)
                if (llvm::GlobalValue* value = module->getNamedValue(renamed))
                    value->setName(original);
        }
        // The harvested bitcode also holds what the extractor emitted itself beyond the chunk's
        // PTU module, so rename inside it rather than re-serializing the PTU module.
        if (!out.bitcode.empty())
        {
            llvm::LLVMContext bitcodeContext;
            auto parsedBitcode = llvm::parseBitcodeFile(
                llvm::MemoryBufferRef(out.bitcode, "cflat-incremental-request"), bitcodeContext);
            if (!parsedBitcode)
            {
                error = "renaming default-argument wrappers: "
                    + ErrorText(parsedBitcode.takeError());
                return false;
            }
            for (const auto& [renamed, original] : renamedWrappers)
                if (llvm::GlobalValue* value = (*parsedBitcode)->getNamedValue(renamed))
                    value->setName(original);
            out.bitcode.clear();
            llvm::raw_string_ostream os(out.bitcode);
            llvm::WriteBitcodeToFile(**parsedBitcode, os);
            os.flush();
        }
    }
    if (harvested && !typeKey.empty() && !req.emitDefinitions)
    {
        impl_->typeResults[typeKey] = out;
        impl_->typeRoots[typeKey] = parsed->TUPart;
    }
    if (harvested && !typeKey.empty() && req.emitDefinitions)
    {
        auto prior = impl_->typeResults.find(typeKey);
        if (prior != impl_->typeResults.end())
        {
            cflat_cinterop::ExtractResult merged = prior->second;
            auto appendRecords = [&](const auto& source) {
                for (const auto& record : source)
                {
                    const bool exists = std::any_of(merged.records.begin(), merged.records.end(),
                        [&](const auto& old) {
                            return (!record.name.empty() && old.name == record.name)
                                || (!record.canonicalCtype.empty()
                                    && old.canonicalCtype == record.canonicalCtype);
                        });
                    if (exists)
                    {
                        for (auto& old : merged.records)
                            if ((!record.name.empty() && old.name == record.name)
                                || (!record.canonicalCtype.empty()
                                    && old.canonicalCtype == record.canonicalCtype))
                            {
                                old = record;
                                break;
                            }
                    }
                    else merged.records.push_back(record);
                }
            };
            auto appendSigs = [&](const auto& source) {
                for (const auto& sig : source)
                {
                    const bool exists = std::any_of(merged.sigs.begin(), merged.sigs.end(),
                        [&](const auto& old) {
                            return old.name == sig.name && old.linkageName == sig.linkageName;
                        });
                    if (!exists) merged.sigs.push_back(sig);
                }
            };
            auto appendTemplates = [&](const auto& source) {
                for (const auto& templ : source)
                {
                    const bool exists = std::any_of(merged.functionTemplates.begin(),
                                                    merged.functionTemplates.end(),
                        [&](const auto& old) {
                            return old.name == templ.name
                                && old.cxxSpelling == templ.cxxSpelling;
                        });
                    if (!exists) merged.functionTemplates.push_back(templ);
                }
            };
            appendRecords(out.records);
            appendSigs(out.sigs);
            appendTemplates(out.functionTemplates);
            merged.bitcode = std::move(out.bitcode);
            merged.emittedDefinitions = out.emittedDefinitions;
            if (!out.firstError.empty()) merged.firstError = std::move(out.firstError);
            if (!out.invalidCxxTypeRequestError.empty())
                merged.invalidCxxTypeRequestError = std::move(out.invalidCxxTypeRequestError);
            merged.droppedCxxDefaultWrappers.insert(
                merged.droppedCxxDefaultWrappers.end(),
                out.droppedCxxDefaultWrappers.begin(), out.droppedCxxDefaultWrappers.end());
            merged.weakPromoteSymbols.insert(merged.weakPromoteSymbols.end(),
                                             out.weakPromoteSymbols.begin(),
                                             out.weakPromoteSymbols.end());
            out = std::move(merged);
        }
    }
    if (harvested && !wrapperName.empty()) impl_->wrapperResults.emplace(wrapperName, out);
    return harvested;
}

const std::string& CxxIncrementalGroup::LastRequestDiagnostics() const
{
    return impl_->lastDiagnostics;
}

namespace
{
    // Every file `roots` reach in `graph`; false when a root is not a header of the TU.
    bool ReachFromRoots(const IncludeGraph& graph, clang::FileManager& fm,
                        const std::vector<std::string>& roots,
                        std::unordered_set<const clang::FileEntry*>& reached)
    {
        std::vector<const clang::FileEntry*> work;
        for (const std::string& root : roots)
        {
            const clang::FileEntry* entry = nullptr;
            if (root.starts_with("<"))
            {
                auto found = graph.angled.find(root);
                if (found != graph.angled.end()) entry = found->second;
            }
            else if (auto ref = fm.getOptionalFileRef(root))
                entry = &ref->getFileEntry();
            if (entry == nullptr || graph.targets.count(entry) == 0) return false;
            if (reached.insert(entry).second) work.push_back(entry);
        }
        while (!work.empty())
        {
            const clang::FileEntry* current = work.back();
            work.pop_back();
            auto found = graph.edges.find(current);
            if (found == graph.edges.end()) continue;
            for (const clang::FileEntry* child : found->second)
                if (reached.insert(child).second) work.push_back(child);
        }
        return true;
    }
}

std::unordered_set<std::string> CxxIncrementalGroup::UnreachableFiles(
    const std::vector<std::string>& roots, const std::vector<std::string>& files) const
{
    std::unordered_set<std::string> result;
    clang::FileManager& fm = impl_->interpreter->getCompilerInstance()->getFileManager();
    std::unordered_set<const clang::FileEntry*> reached;
    if (!ReachFromRoots(impl_->includeGraph, fm, roots, reached)) return result;
    for (const std::string& file : files)
    {
        auto ref = fm.getOptionalFileRef(file);
        if (ref && impl_->includeGraph.targets.count(&ref->getFileEntry()) != 0
            && reached.count(&ref->getFileEntry()) == 0)
            result.insert(file);
    }
    return result;
}
