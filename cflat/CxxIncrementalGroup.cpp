#include "CxxIncrementalGroup.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclContextInternals.h"
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
#include "clang/Lex/Token.h"
#include "clang/Options/Options.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Sema.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/IR/Module.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    std::string BodyVerdictKey(const clang::FunctionDecl* function)
    {
        std::string key = function->getQualifiedNameAsString() + "|"
            + function->getType().getCanonicalType().getAsString();
        if (const auto* args = function->getTemplateSpecializationArgs())
        {
            llvm::raw_string_ostream out(key);
            const auto& policy = function->getASTContext().getPrintingPolicy();
            for (const auto& arg : args->asArray())
            {
                out << '|';
                arg.print(policy, out, true);
            }
        }
        return key;
    }

    /*
     * A failed Parse erases every TU lookup entry whose visible declaration the failed chunk
     * made. A chunk that reopens a namespace replaces the earlier redeclaration there, so the
     * whole namespace (and every type an earlier chunk declared in it) would drop out of name
     * lookup. Snapshot the visible namespaces first; restore the ones the failure erased.
     */
    std::vector<clang::NamespaceDecl*> VisibleTopLevelNamespaces(clang::ASTContext& context)
    {
        std::vector<clang::NamespaceDecl*> out;
        clang::DeclContext* tu = context.getTranslationUnitDecl()->getPrimaryContext();
        if (clang::StoredDeclsMap* map = tu->getLookupPtr())
            for (auto& entry : *map)
                for (clang::NamedDecl* decl : entry.second.getLookupResult())
                    if (auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl))
                        out.push_back(ns);
        return out;
    }

    void RestoreTopLevelNamespaces(clang::ASTContext& context,
                                   const std::vector<clang::NamespaceDecl*>& saved)
    {
        clang::DeclContext* tu = context.getTranslationUnitDecl()->getPrimaryContext();
        for (clang::NamespaceDecl* ns : saved)
            if (tu->lookup(ns->getDeclName()).empty()) tu->makeDeclVisibleInContext(ns);
    }

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
        std::string firstErrorLocation;
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
                if (firstError.empty())
                {
                    firstError = std::move(message);
                    if (info.getLocation().isValid() && info.hasSourceManager())
                    {
                        const clang::PresumedLoc presumed = info.getSourceManager().getPresumedLoc(
                            info.getSourceManager().getExpansionLoc(info.getLocation()));
                        if (presumed.isValid())
                            firstErrorLocation = std::format("{}:{}: {}", presumed.getFilename(),
                                                             presumed.getLine(), firstError);
                    }
                }
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
        std::vector<clang::FileID>& fileIds;
        IncludeGraph& graph;

        IncludeCollector(clang::Preprocessor& p, std::vector<std::string>& f,
                         std::vector<clang::FileID>& ids, IncludeGraph& g)
            : pp(p), files(f), fileIds(ids), graph(g) {}

        void FileChanged(clang::SourceLocation loc, FileChangeReason reason,
                         clang::SrcMgr::CharacteristicKind, clang::FileID) override
        {
            if (reason != EnterFile) return;
            clang::SourceManager& sm = pp.getSourceManager();
            llvm::StringRef file = sm.getFilename(loc);
            if (file.empty()) return;
            files.push_back(file.str());
            fileIds.push_back(sm.getFileID(loc));
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

    /*
     * Private Parser members the lazy-body scheme needs. An explicit instantiation may name a
     * private member (access is not checked there); the friend hands the pointer out.
     */
    struct ParserSkipBodiesTag { using type = bool clang::Parser::*; };
    struct ParserLateParseTag { using type = void (*)(void*, clang::LateParsedTemplate&); };
    template <typename Tag, typename Tag::type Member>
    struct PrivateMember
    {
        friend typename Tag::type AccessPrivate(Tag) { return Member; }
    };
    ParserSkipBodiesTag::type AccessPrivate(ParserSkipBodiesTag);
    ParserLateParseTag::type AccessPrivate(ParserLateParseTag);
    template struct PrivateMember<ParserSkipBodiesTag, &clang::Parser::SkipFunctionBodies>;
    template struct PrivateMember<ParserLateParseTag, &clang::Parser::LateTemplateParserCallback>;

    /*
     * Bodies of inline non-template functions skipped by the group's header parse, kept as the
     * preprocessed tokens the parser saw so the demand pass can parse only the bodies it emits.
     * The parser skips a body by lexing it fresh, and the token watcher reports each fresh token
     * once in stream order, so a skipped body is the balanced '{' ... '}' run after its '{'.
     */
    struct LazyBodies
    {
        struct Body
        {
            std::vector<clang::Token> tokens;
            clang::FPOptions fpo;
        };
        std::unordered_map<std::string, std::string> replayedPoison;
        bool active = false;                 // only during the group's header Parse
        clang::Parser* parser = nullptr;
        clang::Sema* sema = nullptr;
        std::deque<clang::Token> recent;     // locates the '{' the parser already consumed
        Body* capturing = nullptr;
        unsigned depth = 0;
        std::unordered_map<clang::FunctionDecl*, Body> bodies;
        cflat_cinterop::CxxDemandPlan* plan = nullptr;   // the group's, for the demand pass
        const std::unordered_map<const clang::FunctionDecl*, std::string>* poisoned = nullptr;
        std::string poisonDiagnostic;
        std::unordered_set<const clang::FunctionDecl*> walked;
        std::unordered_set<const clang::CXXRecordDecl*> vtableRecords;

        void Observe(const clang::Token& token)
        {
            if (capturing != nullptr)
            {
                capturing->tokens.push_back(token);
                if (token.is(clang::tok::l_brace)) ++depth;
                else if (token.is(clang::tok::r_brace) && --depth == 0) capturing = nullptr;
                return;
            }
            recent.push_back(token);
            if (recent.size() > 64) recent.pop_front();
        }

        bool Skip(clang::Decl* decl)
        {
            if (!active || capturing != nullptr || parser == nullptr) return false;
            auto* fd = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if (fd == nullptr) return false;
            if (fd->isTemplated() || !fd->isInlined() || fd->getReturnType()->isUndeducedType()
                || fd->getTemplateSpecializationKind() != clang::TSK_Undeclared
                || fd->getParentFunctionOrMethod() != nullptr || fd->hasAttr<clang::UsedAttr>()
                || fd->isMultiVersion())
                return false;
            // A ctor-initializer or function-try-block is not a plain brace run; parse those.
            const clang::Token& open = parser->getCurToken();
            if (!open.is(clang::tok::l_brace)) return false;
            size_t at = recent.size();
            while (at > 0 && !(recent[at - 1].is(clang::tok::l_brace)
                               && recent[at - 1].getLocation() == open.getLocation()))
                --at;
            if (at == 0) return false;
            // hasInlineBody() (the key-function choice, so vtable linkage) must not change.
            fd->setWillHaveBody(true);
            Body& body = bodies[fd];
            body.tokens.assign(recent.begin() + (at - 1), recent.end());
            body.fpo = sema->getCurFPFeatures();
            recent.clear();
            depth = 0;
            for (const clang::Token& token : body.tokens)
            {
                if (token.is(clang::tok::l_brace)) ++depth;
                else if (token.is(clang::tok::r_brace)) --depth;
            }
            if (depth != 0) capturing = &body;
            return true;
        }

        // A still-skipped function's lazy body, or null.
        Body* Find(const clang::FunctionDecl* fd)
        {
            const clang::FunctionDecl* definition = nullptr;
            if (fd == nullptr || !fd->isDefined(definition)) return nullptr;
            auto it = bodies.find(const_cast<clang::FunctionDecl*>(definition));
            return it == bodies.end() ? nullptr : &it->second;
        }

        /*
         * For the demand pass: CodeGen treats a skipped body as a declaration, but would emit a
         * late-parsed one with no body. Outside it every predicate must see "defined", as it did.
         */
        void MarkPending(bool forCodeGen)
        {
            for (auto& [fd, body] : bodies)
            {
                fd->setLateTemplateParsed(!forCodeGen);
                fd->setHasSkippedBody(forCodeGen);
                fd->setWillHaveBody(true);
            }
        }

        // Callees a parsed body names directly; their skipped bodies are parsed with it.
        struct CalleeWalk : clang::RecursiveASTVisitor<CalleeWalk>
        {
            std::vector<const clang::FunctionDecl*> found;
            bool shouldVisitImplicitCode() const { return true; }
            void Note(const clang::FunctionDecl* fd) { if (fd != nullptr) found.push_back(fd); }
            std::vector<clang::VarDecl*> storage;
            bool VisitDeclRefExpr(clang::DeclRefExpr* e)
            {
                Note(llvm::dyn_cast<clang::FunctionDecl>(e->getDecl()));
                if (auto* var = llvm::dyn_cast<clang::VarDecl>(e->getDecl());
                    var != nullptr && var->hasGlobalStorage() && !var->isStaticLocal())
                    storage.push_back(var);
                return true;
            }
            bool VisitMemberExpr(clang::MemberExpr* e)
            {
                Note(llvm::dyn_cast<clang::FunctionDecl>(e->getMemberDecl()));
                return true;
            }
            std::vector<const clang::CXXRecordDecl*> constructed;
            bool VisitCXXConstructExpr(clang::CXXConstructExpr* e)
            {
                Note(e->getConstructor());
                if (e->getConstructor() != nullptr)
                    constructed.push_back(e->getConstructor()->getParent());
                return true;
            }
            bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr* e)
            {
                Note(e->getTemporary()->getDestructor());
                return true;
            }
            bool recovery = false;
            bool VisitRecoveryExpr(clang::RecoveryExpr*) { recovery = true; return true; }
            bool VisitVarDecl(clang::VarDecl* var)
            {
                if (const auto* record = var->getType()->getAsCXXRecordDecl())
                    if (record->hasDefinition()) Note(record->getDestructor());
                return true;
            }
        };

        /*
         * clang's late-parse entry resets CurContext to ASTContext's TU, which in the Interpreter
         * is the NEWEST partial TU, then re-pushes the body's lexical scopes outermost first. A
         * scope opened in an older chunk hangs off that chunk's TU, so the first push is out of
         * order. For the parse, the outermost scope is re-homed to the current TU; both TUs share
         * one primary context, and lookup walks semantic parents, so only the push check sees it.
         */
        struct LexicalTuRehome
        {
            clang::Decl* outer = nullptr;
            clang::DeclContext* writtenTU = nullptr;
            bool wasSemantic = false;
            clang::Decl::ModuleOwnershipKind ownership{};

            LexicalTuRehome(clang::FunctionDecl* fd, clang::DeclContext* currentTU)
            {
                clang::Decl* top = nullptr;
                for (clang::DeclContext* dc = fd->getLexicalDeclContext();
                     dc != nullptr && !dc->isTranslationUnit(); dc = dc->getLexicalParent())
                    top = clang::Decl::castFromDeclContext(dc);
                if (top == nullptr || top->getLexicalDeclContext() == currentTU) return;
                outer = top;
                writtenTU = top->getLexicalDeclContext();
                wasSemantic = top->getDeclContext() == writtenTU;
                ownership = top->getModuleOwnershipKind();
                outer->setLexicalDeclContext(currentTU);
            }
            ~LexicalTuRehome()
            {
                if (outer == nullptr) return;
                // setDeclContext drops the lexical/semantic split the re-home allocated.
                if (wasSemantic) outer->setDeclContext(writtenTU);
                else outer->setLexicalDeclContext(writtenTU);
                if (outer->getModuleOwnershipKind() != ownership)
                    outer->setModuleOwnershipKind(ownership);
            }
        };

        // Parse one skipped body where it was written (clang's MS late-parse entry re-enters
        // its lexical scopes). A body that does not compile leaves `fd` declared only.
        bool ParseOne(clang::FunctionDecl* fd, std::string& failure)
        {
            auto it = bodies.find(fd);
            if (it == bodies.end()) return false;
            Body body = std::move(it->second);
            bodies.erase(it);
            clang::LateParsedTemplate late;
            late.D = fd;
            late.FPO = body.fpo;
            late.Toks.append(body.tokens.begin(), body.tokens.end());
            /*
             * A namespace-scope definition is skipped only after ActOnStartOfFunctionDef added its
             * named parameters to fd's decls; the late parse adds them again. Re-adding a listed
             * decl links the chain into a cycle (Release has no assert), and any decls() walk of
             * fd - MS ABI CodeGen's dllimport inlining check - then never ends.
             */
            for (clang::ParmVarDecl* param : fd->parameters())
                if (param->getIdentifier() != nullptr && fd->containsDecl(param))
                    fd->removeDecl(param);
            clang::DiagnosticErrorTrap trap(sema->getDiagnostics());
            fd->setHasSkippedBody(false);
            fd->setLateTemplateParsed(true);
            // A namespace-scope body was skipped after ActOnStartOfFunctionDef added the params to
            // fd; the late parse re-adds them, so take them out first (else the decl list cycles).
            for (clang::ParmVarDecl* param : fd->parameters())
                if (fd->containsDecl(param)) fd->removeDecl(param);
            {
                LexicalTuRehome rehome(fd, sema->getASTContext().getTranslationUnitDecl());
                AccessPrivate(ParserLateParseTag{})(parser, late);
            }
            /*
             * The late parse leaves its token stream (pointing into `late`) as the current token
             * lexer; the next Interpreter::Parse would enter its file inside it. Drain to the
             * input end the way IncrementalParser does after a DelayedTemplateParsing parse.
             */
            if (!active)
            {
                clang::Token drained;
                do parser->getPreprocessor().Lex(drained);
                while (drained.isNot(clang::tok::annot_repl_input_end)
                       && drained.isNot(clang::tok::eof));
            }
            sema->PerformPendingInstantiations();
            fd->setLateTemplateParsed(false);
            if (!trap.hasErrorOccurred() && fd->getBody() != nullptr) return true;
            fd->setBody(nullptr);
            fd->setHasSkippedBody(true);
            fd->setInvalidDecl();
            if (failure.empty()) failure = fd->getQualifiedNameAsString();
            return false;
        }

        /*
         * Parse every skipped body CodeGen can reach from `work`: walk each reached body (skipped
         * or not) for callees, destructors a destructor runs implicitly, and the virtual members
         * of a record whose vtable this module defines. The demand pass's ODR-use closure
         * still catches anything this misses, one round later. Returns bodies parsed.
         */
        unsigned MaterializeReachable(std::vector<const clang::FunctionDecl*> work,
                                      std::string& failure)
        {
            unsigned parsed = 0;
            poisonDiagnostic.clear();
            clang::Scope tuScope(nullptr, clang::Scope::DeclScope, sema->getDiagnostics());
            const bool lendScope = sema->TUScope == nullptr;
            if (lendScope) sema->TUScope = &tuScope;
            struct ScopeReset
            {
                clang::Sema& sema; bool active;
                ~ScopeReset() { if (active) sema.TUScope = nullptr; }
            } scopeReset{*sema, lendScope};
            clang::ASTContext& ctx = sema->getASTContext();
            auto addRecord = [&](const clang::CXXRecordDecl* record, bool forVTable) {
                if (record == nullptr || !record->hasDefinition()) return;
                record = record->getDefinition();
                if (!forVTable)
                {
                    work.push_back(record->getDestructor());
                    return;
                }
                if (!record->isDynamicClass() || !vtableRecords.insert(record).second) return;
                const clang::CXXMethodDecl* key = ctx.getCurrentKeyFunction(record);
                const clang::FunctionDecl* keyDef = nullptr;
                if (key != nullptr && !key->isDefined(keyDef)) return;   // the library's vtable
                for (const clang::CXXMethodDecl* method : record->methods())
                    if (method->isVirtual()) work.push_back(method);
            };
            while (!work.empty())
            {
                const clang::FunctionDecl* next = work.back();
                work.pop_back();
                const clang::FunctionDecl* definition = nullptr;
                if (next == nullptr) continue;
                if (auto bad = replayedPoison.find(BodyVerdictKey(next)); bad != replayedPoison.end())
                {
                    if (failure.empty()) failure = next->getQualifiedNameAsString();
                    if (poisonDiagnostic.empty()) poisonDiagnostic = bad->second;
                    continue;
                }
                if (poisoned != nullptr)
                    if (auto bad = poisoned->find(next); bad != poisoned->end())
                    {
                        if (failure.empty()) failure = next->getQualifiedNameAsString();
                        if (poisonDiagnostic.empty()) poisonDiagnostic = bad->second;
                        continue;
                    }
                clang::DiagnosticErrorTrap trap(sema->getDiagnostics());
                sema->MarkFunctionReferenced(next->getLocation(),
                                             const_cast<clang::FunctionDecl*>(next));
                sema->PerformPendingInstantiations();
                if (trap.hasErrorOccurred())
                {
                    if (failure.empty()) failure = next->getQualifiedNameAsString();
                    continue;
                }
                // An instantiation that failed in an earlier check stays invalid; its trap fired then.
                if (next->isInvalidDecl())
                {
                    if (failure.empty()) failure = next->getQualifiedNameAsString();
                    continue;
                }
                if (!next->isDefined(definition)) continue;
                auto* target = const_cast<clang::FunctionDecl*>(definition);
                if (bodies.count(target) != 0 && ParseOne(target, failure)) ++parsed;
                if (target->getBody() == nullptr || !walked.insert(target).second) continue;
                CalleeWalk walk;
                walk.TraverseStmt(target->getBody());
                // Error nodes mean clang diagnosed this body earlier, where no check listened.
                if (walk.recovery)
                {
                    if (failure.empty()) failure = target->getQualifiedNameAsString();
                    continue;
                }
                work.insert(work.end(), walk.found.begin(), walk.found.end());
                // A variable that body names must be shown to CodeGen, or it stays external.
                for (clang::VarDecl* var : walk.storage)
                    if (clang::VarDecl* def = var->getDefinition(); def != nullptr && plan != nullptr)
                        plan->Add(def);
                for (const clang::CXXRecordDecl* record : walk.constructed)
                    addRecord(record, true);
                if (const auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(target))
                {
                    for (const clang::CXXCtorInitializer* init : ctor->inits())
                    {
                        CalleeWalk inits;
                        inits.TraverseStmt(init->getInit());
                        work.insert(work.end(), inits.found.begin(), inits.found.end());
                    }
                    addRecord(ctor->getParent(), true);
                }
                if (const auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(target))
                {
                    const clang::CXXRecordDecl* record = dtor->getParent();
                    addRecord(record, true);
                    for (const clang::CXXBaseSpecifier& base : record->bases())
                        addRecord(base.getType()->getAsCXXRecordDecl(), false);
                    for (const clang::FieldDecl* field : record->fields())
                        addRecord(ctx.getBaseElementType(field->getType())->getAsCXXRecordDecl(),
                                  false);
                }
            }
            return parsed;
        }
    };

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
        // Always on: every announced decl, whoever triggered it (a chunk or the extractor's own
        // instantiations), is a candidate for the group's demand pass.
        cflat_cinterop::CxxDemandPlan* plan = nullptr;
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
                // Functions and variables only: a namespace or linkage block would hand every
                // strong definition inside it; chunks hand those through their own harvest.
                if (plan != nullptr
                    && (llvm::isa<clang::FunctionDecl>(decl) || llvm::isa<clang::VarDecl>(decl)))
                    plan->Add(decl);
            }
            return true;
        }

        LazyBodies* lazyBodies = nullptr;
        bool shouldSkipFunctionBody(clang::Decl* decl) override
        {
            return lazyBodies != nullptr && lazyBodies->Skip(decl);
        }

        // An implicitly instantiated static data member never reaches HandleTopLevelDecl; the
        // demand pass still needs it to emit the storage a request ODR-uses.
        void HandleCXXStaticMemberVarInstantiation(clang::VarDecl* var) override
        {
            if (sink != nullptr) sink->push_back(var);
            if (plan != nullptr) plan->Add(var);
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

    /*
     * A cc1-only flag that reaches the driver without -Xclang is unknown to it and dropped - but
     * the driver first ranks every option spelling for a "did you mean" hint, ~0.3 ms per flag.
     * Drop them here with the driver's own table and visibility. That error also skipped the
     * default config-file search, so --no-default-config keeps the driver's input identical.
     */
    void DropDriverUnknownArgs(std::vector<std::string>& args)
    {
        std::vector<const char*> argv;
        argv.reserve(args.size());
        for (const std::string& arg : args) argv.push_back(arg.c_str());
        unsigned missingIndex = 0, missingCount = 0;
        const llvm::opt::InputArgList parsed = clang::getDriverOptTable().ParseArgs(
            argv, missingIndex, missingCount, llvm::opt::Visibility(clang::options::ClangOption));
        std::vector<char> unknown(args.size(), 0);
        bool any = false;
        for (const llvm::opt::Arg* arg : parsed.filtered(clang::options::OPT_UNKNOWN))
            if (arg->getIndex() < unknown.size()) { unknown[arg->getIndex()] = 1; any = true; }
        if (!any) return;
        std::vector<std::string> kept;
        kept.reserve(args.size() + 1);
        for (size_t i = 0; i < args.size(); ++i)
            if (unknown[i] == 0) kept.push_back(std::move(args[i]));
        kept.push_back("--no-default-config");
        args = std::move(kept);
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
    std::vector<clang::FileID> includedFileIds;   // parallel to includedFiles
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
    // Everything a definitions harvest would have handed CodeGen, for the one demand pass.
    cflat_cinterop::CxxDemandPlan plan;
    std::vector<std::string> demandChunkSources;
    bool demandHeaderHarvested = false;
    // Free-operator candidates per header TU root (ExtractRequest::operatorIndex).
    std::unordered_map<const clang::Decl*, std::vector<clang::Decl*>> operatorIndex;
    // Header records whose special members the harvest deferred (ExtractRequest).
    std::unordered_map<std::string, const clang::CXXRecordDecl*> specialMemberRecords;
    LazyBodies lazy;

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
    bool verbose, std::string& error, bool tolerateDiagnostics,
    const cflat_cinterop::ExtractRequest* macroReq,
    cflat_cinterop::ExtractResult* macroOut)
{
    std::optional<llvm::TimeTraceScope> setupScope;
    setupScope.emplace("CxxGroupSetup");
    std::vector<std::string> storage = InterpreterArgs(args);
    DropDriverUnknownArgs(storage);
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
    // Same reason as the extractor invocation: a forwarding destructor must stay a symbol
    // of its own (see CXXCtorDtorAliases in CClangExtract.cpp).
    (*compiler)->getCodeGenOpts().CXXCtorDtorAliases = false;
    /*
     * Parse only: the Interpreter would code-generate (and run its backend pipeline over) every
     * chunk, and cflat never uses those modules. Companion code comes from the group's one
     * demand pass (EmitCxxDemandCompanion) instead.
     */
    (*compiler)->getFrontendOpts().ProgramAction = clang::frontend::ParseSyntaxOnly;
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
        recorder->plan = &impl->plan;
        recorder->lazyBodies = &impl->lazy;
        impl->announcer = recorder.get();
        auto& consumers = MultiplexAccess::ListOf(*multiplex);
        auto guarded = std::make_unique<StaticMemberGuard>(std::move(consumers));
        consumers.clear();
        consumers.push_back(std::move(recorder));
        consumers.push_back(std::move(guarded));
    }
    setupScope.reset();
    llvm::TimeTraceScope finalizeScope("CxxGroupHeaderFinalize");
    {
        DiagnosticScope diagnostics(impl->interpreter->getCompilerInstance()->getDiagnostics());
        impl->interpreter->getCompilerInstance()->getPreprocessor().addPPCallbacks(
            std::make_unique<IncludeCollector>(
                impl->interpreter->getCompilerInstance()->getPreprocessor(),
                impl->includedFiles, impl->includedFileIds, impl->includeGraph));
        /*
         * Skip the bodies of inline non-template functions, keeping their tokens: the demand
         * pass parses only the ones the program reaches (LazyBodies). Request chunks parse all.
         */
        clang::CompilerInstance& ci = *impl->interpreter->getCompilerInstance();
        clang::Preprocessor& pp = ci.getPreprocessor();
        LazyBodies& lazy = impl->lazy;
        const bool trackHeaderScope = macroReq != nullptr && macroOut != nullptr
            && macroReq->cxxMode;
        std::function<void()> stopMacroCollector;
        if (macroReq != nullptr && macroOut != nullptr)
            stopMacroCollector = cflat_cinterop::AttachCxxMacroPrepass(pp, *macroReq, *macroOut);
        lazy.parser = static_cast<clang::Parser*>(pp.getCodeCompletionHandler());
        lazy.sema = &ci.getSema();
        lazy.poisoned = &impl->poisoned;
        // CFLAT_CXX_EAGER_BODIES=1 parses every body up front (A/B and bisecting a late body).
        if (lazy.parser != nullptr && !cflat_cinterop::CxxEagerBodies())
        {
            lazy.parser->*AccessPrivate(ParserSkipBodiesTag{}) = true;
            lazy.active = true;
        }
        int headerBraceDepth = 0;
        const bool watchTokens = lazy.active || trackHeaderScope;
        if (watchTokens)
            pp.setTokenWatcher([&lazy, &pp, &headerBraceDepth, macroOut, trackHeaderScope,
                                injectedHeaderClosers = false]
                               (const clang::Token& token) mutable {
                if (lazy.active) lazy.Observe(token);
                if (!trackHeaderScope) return;
                if (token.is(clang::tok::l_brace)) ++headerBraceDepth;
                else if (token.is(clang::tok::r_brace)) --headerBraceDepth;
                else if (!injectedHeaderClosers && headerBraceDepth != 0
                         && token.is(clang::tok::identifier)
                         && token.getIdentifierInfo() != nullptr
                         && token.getIdentifierInfo()->getName()
                            == "__cflat_header_scope_sentinel")
                {
                    macroOut->headerScopeOpen = true;
                    injectedHeaderClosers = true;
                    // A stray extra `}` (depth < 0) needs no closers: clang recovers from it.
                    if (headerBraceDepth < 0) return;
                    const unsigned count = static_cast<unsigned>(headerBraceDepth);
                    auto closers = std::make_unique<clang::Token[]>(count);
                    for (unsigned i = 0; i < count; ++i)
                    {
                        closers[i].startToken();
                        closers[i].setKind(clang::tok::r_brace);
                        closers[i].setLength(1);
                        closers[i].setLocation(token.getLocation());
                    }
                    pp.EnterTokenStream(std::move(closers), count,
                                       /*DisableMacroExpansion*/ true,
                                       /*IsReinject*/ false);
                }
            });
        auto ptu = [&] {
            llvm::TimeTraceScope parseScope("CxxGroupHeaderParse");
            return impl->interpreter->Parse(headerSource);
        }();
        if (stopMacroCollector) stopMacroCollector();
        if (watchTokens) pp.setTokenWatcher(nullptr);
        if (lazy.active)
        {
            lazy.active = false;
            lazy.parser->*AccessPrivate(ParserSkipBodiesTag{}) = false;
            lazy.recent.clear();
            // An unterminated capture cannot be replayed; that function stays declared only.
            if (lazy.capturing != nullptr)
                for (auto it = lazy.bodies.begin(); it != lazy.bodies.end(); ++it)
                    if (&it->second == lazy.capturing)
                    {
                        lazy.bodies.erase(it);
                        break;
                    }
            lazy.capturing = nullptr;
            lazy.MarkPending(/*forCodeGen*/ false);
            if (verbose)
                std::cout << std::format("[verbose] C++ header parse: {} inline bod(ies) "
                                         "deferred\n", lazy.bodies.size());
        }
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
    cflat_cinterop::ExtractRequest effective = req;
    if (effective.RecordsDefinitionDemand()) effective.demandPlan = &impl_->plan;
    effective.operatorIndex = &impl_->operatorIndex;
    effective.specialMemberRecords = &impl_->specialMemberRecords;
    const bool harvested = cflat_cinterop::ExtractCxxIncremental(
        effective, *impl_->interpreter->getCompilerInstance(), impl_->headerRoot,
        impl_->headerRoot, {}, impl_->headerModule, out, error, true);
    if (harvested && effective.demandPlan != nullptr) out.demandRecorded = true;
    if (harvested && out.demandRecorded) impl_->demandHeaderHarvested = true;
    out.includedFiles = impl_->includedFiles;
    return harvested;
}

std::vector<std::string_view> CxxIncrementalGroup::IncludedFileBuffers() const
{
    std::vector<std::string_view> buffers;
    if (impl_ == nullptr) return buffers;
    const clang::SourceManager& sm = impl_->interpreter->getCompilerInstance()->getSourceManager();
    buffers.reserve(impl_->includedFileIds.size());
    for (clang::FileID id : impl_->includedFileIds)
    {
        std::optional<llvm::StringRef> data = sm.getBufferDataOrNone(id);
        buffers.push_back(data ? std::string_view(data->data(), data->size()) : std::string_view());
    }
    return buffers;
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

bool CxxIncrementalGroup::HasDemandChunkSource(const std::string& source) const
{
    return std::find(impl_->demandChunkSources.begin(), impl_->demandChunkSources.end(), source)
        != impl_->demandChunkSources.end();
}

bool CxxIncrementalGroup::HasDemandHeaderHarvest() const
{
    return impl_->demandHeaderHarvested;
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
    // Bodies already poisoned before this parse; the replay entry records only this parse's own.
    std::unordered_set<const clang::FunctionDecl*> poisonedBefore;
    for (const auto& [function, verdict] : impl_->poisoned) poisonedBefore.insert(function);
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
    // Macro probes are one independent line each: a body that is no expression drops only itself.
    const bool recoverDeclarations = (!typeKey.empty() && !wrapperBatch) || !req.cxxMacroProbes.empty();
    std::string chunk = source;
    /*
     * An earlier batch chunk may already define this wrapper (a default-argument wrapper whose
     * member was refused at the time, or a std::function bridge ctor). Parse under a fresh name,
     * then restore the requested name in the result; both definitions are weak and identical.
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
    // A std::function bridge ctor is the same kind of weak helper: a second request for the
    // spelling (its first came from a cache hit that a demand replay re-parsed) re-defines it.
    for (const char* helperPrefix : { "__cflat_dflt_", "__cflat_std_function_ctor_" })
        for (size_t pos = 0; (pos = chunk.find(helperPrefix, pos)) != std::string::npos;)
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
        clang::ASTContext& astContext = impl_->interpreter->getCompilerInstance()->getASTContext();
        std::vector<clang::NamespaceDecl*> namespaces;
        if (chunk.find("namespace ") != std::string::npos)
            namespaces = VisibleTopLevelNamespaces(astContext);
        if (impl_->announcer != nullptr) impl_->announcer->sink = &announced;
        auto ptu = impl_->interpreter->Parse(chunk);
        if (!ptu && !namespaces.empty()) RestoreTopLevelNamespaces(astContext, namespaces);
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
        {
            // A later chunk's use raises no error again; the demand check must still see it.
            for (clang::FunctionDecl* function : diagnostics.consumer.failedFunctions)
            {
                auto cause = impl_->causes.find(function);
                impl_->poisoned.emplace(function, cause != impl_->causes.end()
                                                      ? cause->second : error);
            }
            return false;
        }
        emptyFailedFunctions();
        if (recoveredError.empty()) recoveredError = diagnostics.consumer.incompleteRecordError;
        if (impl_->verbose && req.cxxMacroProbes.empty())
            std::cout << std::format("[verbose] incremental request dropped declarations after "
                                     "'{}':\n{}", error, dropped);
        else if (impl_->verbose)
            std::cout << std::format("[verbose] macro probe batch dropped {} probe(s) that are no "
                                     "expression\n", std::count(dropped.begin(), dropped.end(), '\n'));
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
    if (effective.emitDefinitions || wrapperBatch) effective.demandPlan = &impl_->plan;
    effective.operatorIndex = &impl_->operatorIndex;
    effective.specialMemberRecords = &impl_->specialMemberRecords;
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
    if (harvested && effective.emitDefinitions && effective.demandPlan != nullptr)
        out.demandRecorded = true;
    if (harvested && out.demandRecorded)
    {
        auto& replay = out.demandReplayChunk;
        replay.order = impl_->demandChunkSources.size() + 1;
        replay.source = source;
        replay.prefixSource = req.demandPrefixSource;
        // Store the whole prefix, not this group's unseen part of it: a replay re-strips it
        // against its own group, which may not have parsed this compile's earlier chunks.
        const size_t prefixAt = req.demandPrefixOffset;
        if (!req.demandPrefixSource.empty() && prefixAt != std::string::npos
            && prefixAt <= source.size())
        {
            const std::string unseen = UnseenPrefixSource(req.demandPrefixSource);
            if (source.compare(prefixAt, unseen.size(), unseen) == 0)
            {
                replay.source = source.substr(0, prefixAt) + req.demandPrefixSource
                    + source.substr(prefixAt + unseen.size());
                replay.prefixOffset = prefixAt;
            }
        }
        replay.typeRequests = req.cxxTypeRequests;
        replay.markerPrefix = req.cxxRequestMarkerPrefix;
        replay.thunkSuffix = req.cxxThunkSuffix;
        replay.wrapperNames = req.cxxFunctionWrapperNames;
        replay.wrapperBatch = req.cxxWrapperBatch;
        replay.autoInstantiate = req.autoInstantiateCxxTypes;
        replay.completeSpecialMembers = req.completeCxxSpecialMembers;
        /*
         * Only bodies that failed in THIS chunk's parse: a verdict from an earlier chunk or a
         * demand check belongs to another request (possibly another program's [cpp] source).
         */
        for (const auto& [function, verdict] : impl_->poisoned)
            if (!poisonedBefore.contains(function))
                replay.poisonedBodies.emplace_back(BodyVerdictKey(function), verdict);
        std::sort(replay.poisonedBodies.begin(), replay.poisonedBodies.end());
        impl_->demandChunkSources.push_back(replay.source);
    }
    if (harvested && out.firstError.empty()) out.firstError = recoveredError;
    if (harvested && !renamedWrappers.empty())
    {
        if (out.demandRecorded)
            for (const auto& [renamed, original] : renamedWrappers)
                impl_->plan.renamed[original] = renamed;
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
            merged.demandRecorded = out.demandRecorded;
            merged.demandReplayChunk = std::move(out.demandReplayChunk);
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

unsigned CxxIncrementalGroup::DemandChunks() const
{
    return impl_->plan.recordedChunks;
}

void CxxIncrementalGroup::RestorePoisonedBodies(
    const std::vector<std::pair<std::string, std::string>>& verdicts)
{
    for (const auto& [key, verdict] : verdicts) impl_->lazy.replayedPoison.emplace(key, verdict);
}

int CxxIncrementalGroup::CheckDemand(const std::string& symbol, std::string& error)
{
    std::string name = symbol;
    if (auto it = impl_->plan.renamed.find(name); it != impl_->plan.renamed.end())
        name = it->second;
    auto it = impl_->plan.bound.find(name);
    if (it == impl_->plan.bound.end()) return 0;
    auto* fd = llvm::dyn_cast<clang::FunctionDecl>(
        clang::GlobalDecl::getFromOpaquePtr(it->second).getDecl());
    if (fd == nullptr) return 1;
    if (auto poisoned = impl_->poisoned.find(fd); poisoned != impl_->poisoned.end())
    {
        error = "clang: " + poisoned->second;
        return -1;
    }
    DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
    diagnostics.consumer.causes = &impl_->causes;
    llvm::TimeTraceScope scope("CxxDemandCheck", name);
    std::string failure;
    impl_->lazy.plan = &impl_->plan;
    impl_->lazy.MaterializeReachable({fd}, failure);
    if (failure.empty() && diagnostics.consumer.firstError.empty()) return 1;
    // Sema can instantiate a helper before the callee walk reaches it. Keep its cold verdict.
    if (impl_->lazy.poisonDiagnostic.empty())
        for (const auto* failed : diagnostics.consumer.failedFunctions)
            if (auto original = impl_->lazy.replayedPoison.find(BodyVerdictKey(failed));
                original != impl_->lazy.replayedPoison.end())
            {
                impl_->lazy.poisonDiagnostic = original->second;
                break;
            }
    error = "clang: " + (!impl_->lazy.poisonDiagnostic.empty() ? impl_->lazy.poisonDiagnostic
        : diagnostics.consumer.allText.empty()
        ? (impl_->lazy.poisonDiagnostic.empty() ? "failed to instantiate '" + failure + "'"
                                               : impl_->lazy.poisonDiagnostic)
        : diagnostics.consumer.allText);
    for (const auto* failed : diagnostics.consumer.failedFunctions)
    {
        auto cause = impl_->causes.find(failed);
        impl_->poisoned.emplace(failed, cause == impl_->causes.end() ? error.substr(7)
                                                                  : cause->second);
    }
    impl_->poisoned.emplace(fd, error.substr(7));
    return -1;
}

bool CxxIncrementalGroup::EmitDemandCompanion(const std::vector<std::string>& demand,
                                              std::string& bitcode,
                                              cflat_cinterop::CxxDemandStats& stats,
                                              std::string& error)
{
    DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
    LazyBodies& lazy = impl_->lazy;
    lazy.plan = &impl_->plan;
    unsigned parsed = 0;
    std::string failure;
    impl_->plan.materializeBody = [&](clang::FunctionDecl* fd) {
        if (lazy.Find(fd) == nullptr) return false;
        parsed += lazy.MaterializeReachable({fd}, failure);
        return lazy.Find(fd) == nullptr && !fd->isInvalidDecl();
    };
    impl_->plan.materializeReachable = [&](std::vector<const clang::FunctionDecl*> roots,
                                           std::string& bodyFailure,
                                           std::string& bodyDiagnostic) {
        parsed += lazy.MaterializeReachable(std::move(roots), failure);
        bodyFailure = failure;
        if (!failure.empty())
            bodyDiagnostic = diagnostics.consumer.firstErrorLocation.empty()
                ? diagnostics.consumer.firstError : diagnostics.consumer.firstErrorLocation;
    };
    lazy.MarkPending(/*forCodeGen*/ true);
    const auto companionStart = std::chrono::steady_clock::now();
    const bool ok = cflat_cinterop::EmitCxxDemandCompanion(
        *impl_->interpreter->getCompilerInstance(), impl_->plan, demand, &impl_->poisoned,
        impl_->verbose, bitcode, stats, error);
    lazy.MarkPending(/*forCodeGen*/ false);
    impl_->plan.materializeBody = nullptr;
    impl_->plan.materializeReachable = nullptr;
    if (impl_->verbose)
    {
        const double companionMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - companionStart).count();
        std::cout << std::format("[verbose] C++ demand companion elapsed: {:.3f} ms\n",
                                 companionMs);
        std::cout << std::format("[verbose] C++ demand pass: {} deferred inline bod(ies) parsed, "
                                 "{} still deferred{}\n", parsed, lazy.bodies.size(),
                                 failure.empty() ? "" : ", '" + failure + "' did not compile");
    }
    if (!ok && error.empty()) error = diagnostics.consumer.firstError;
    else if (!ok && !diagnostics.consumer.firstError.empty()
             && error.find(diagnostics.consumer.firstError) == std::string::npos)
        error += ": " + diagnostics.consumer.firstError;
    return ok;
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
