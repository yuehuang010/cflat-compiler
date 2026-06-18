#include <llvm\Support\CrashRecoveryContext.h>

#include "LspServer.h"
#include "LspTypes.h"
#include "LspSymbolIndex.h"
#include "JsonRpcLoop.h"
#include "LLVMBackend.h"
#include "MainListener.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <deque>
#include <windows.h>
#include <nlohmann/json.hpp>

namespace {

constexpr std::string_view kFileUriPrefix = "file:///";

// URI <-> file path conversion
std::string uriToFilePath(const std::string& uri)
{
    if (!uri.starts_with(kFileUriPrefix))
        return uri;
    std::string path = uri.substr(kFileUriPrefix.size());
    std::string result;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '%' && i + 2 < path.size())
        {
            char hex[3] = { path[i+1], path[i+2], '\0' };
            result += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        }
        else if (path[i] == '/')
            result += '\\';
        else
            result += path[i];
    }
    return result;
}

std::string filePathToUri(const std::string& path)
{
    std::string uri(kFileUriPrefix);
    for (char c : path)
    {
        if (c == '\\') uri += '/';
        else if (c == ':') uri += "%3A";
        else uri += c;
    }
    return uri;
}

static bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Return the text of a single line (0-based) from document text.
static std::string getLineText(const std::string& text, int line)
{
    int cur = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            if (cur == line)
                return text.substr(lineStart, i - lineStart);
            cur++;
            lineStart = i + 1;
        }
    }
    return cur == line ? text.substr(lineStart) : std::string{};
}

// If the given line is an import statement, return the quoted filename; else return "".
static std::string extractImportFilename(const std::string& text, int line)
{
    std::string ln = getLineText(text, line);
    size_t i = 0;
    while (i < ln.size() && std::isspace((unsigned char)ln[i])) i++;
    if (ln.compare(i, 6, "import") != 0) return {};
    i += 6;
    while (i < ln.size() && std::isspace((unsigned char)ln[i])) i++;
    if (i >= ln.size() || ln[i] != '"') return {};
    i++;
    size_t start = i;
    while (i < ln.size() && ln[i] != '"') i++;
    if (i >= ln.size()) return {};
    return ln.substr(start, i - start);
}

// Resolve an import filename using the same search order as CompileImportedFile.
static std::string resolveImportPath(const std::string& sourceFilePath,
                                     const std::string& importFilename,
                                     const std::string& importSearchDir,
                                     const std::string& runtimeDir)
{
    auto tryCanonical = [](std::filesystem::path p) -> std::string {
        std::error_code ec;
        auto c = std::filesystem::canonical(p.lexically_normal(), ec);
        return ec ? std::string{} : c.string();
    };
    if (!sourceFilePath.empty())
    {
        auto r = tryCanonical(std::filesystem::path(sourceFilePath).parent_path() / importFilename);
        if (!r.empty()) return r;
    }
    if (!importSearchDir.empty())
    {
        auto r = tryCanonical(std::filesystem::path(importSearchDir) / importFilename);
        if (!r.empty()) return r;
    }
    if (!runtimeDir.empty())
    {
        auto r = tryCanonical(std::filesystem::path(runtimeDir) / "core" / importFilename);
        if (!r.empty()) return r;
    }
    return {};
}

// Count identifier occurrences across the document, skipping comments and string/char
// literals so a name mentioned only in a comment never counts as a "use". This is the
// use-set for the unused-code check: a declared name whose count is <= 1 (only the
// declaration itself) is unreferenced. Same-named declarations in sibling scopes inflate
// the count, which can only suppress a hint - never produce a false positive.
std::unordered_map<std::string, int> scanIdentifierOccurrences(const std::string& text)
{
    std::unordered_map<std::string, int> counts;
    auto isWordStart = [](char c) { return std::isalpha((unsigned char)c) || c == '_'; };

    size_t i = 0, n = text.size();
    while (i < n)
    {
        char c = text[i];
        // Comments
        if (c == '/' && i + 1 < n && text[i + 1] == '/')
        {
            i += 2;
            while (i < n && text[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        // String / char literals
        if (c == '"' || c == '\'')
        {
            char quote = c;
            i++;
            while (i < n && text[i] != quote)
            {
                if (text[i] == '\\') i++;  // skip escaped char
                i++;
            }
            i++;  // closing quote
            continue;
        }
        // Identifier
        if (isWordStart(c))
        {
            size_t start = i;
            while (i < n && isWordChar(text[i])) i++;
            counts[text.substr(start, i - start)]++;
            continue;
        }
        i++;
    }
    return counts;
}

// Extract the word (identifier) at a given 0-based line/character position in text.
std::string extractWordAt(const std::string& text, int line, int character)
{
    std::string lineText = getLineText(text, line);
    size_t col = static_cast<size_t>(character);
    if (col >= lineText.size()) return {};
    size_t start = col;
    while (start > 0 && isWordChar(lineText[start - 1])) start--;
    size_t end = col;
    while (end < lineText.size() && isWordChar(lineText[end])) end++;
    return lineText.substr(start, end - start);
}

// Extract the identifier before a '.' at the given position, plus any partial identifier after it.
// Returns {"", ""} if the character before the cursor (or before any partial word) is not '.'.
std::pair<std::string, std::string> extractReceiverAt(const std::string& text, int line, int character)
{
    std::string lineText = getLineText(text, line);
    size_t col = static_cast<size_t>(character) < lineText.size() ? static_cast<size_t>(character) : lineText.size();

    // Scan backward over any partial identifier typed after the dot
    size_t partialStart = col;
    while (partialStart > 0 && isWordChar(lineText[partialStart - 1]))
        partialStart--;
    std::string partial = lineText.substr(partialStart, col - partialStart);

    // Expect a '.' immediately before the partial
    if (partialStart == 0 || lineText[partialStart - 1] != '.')
        return {"", ""};

    // Scan backward over the receiver identifier
    size_t dotPos = partialStart - 1;
    size_t recvEnd = dotPos;
    size_t recvStart = dotPos;
    while (recvStart > 0 && isWordChar(lineText[recvStart - 1]))
        recvStart--;
    if (recvStart == recvEnd) return {"", ""};

    return {lineText.substr(recvStart, recvEnd - recvStart), partial};
}

struct OpenDocument
{
    std::string uri;
    std::string filePath;
    std::string text;
};

class LspServer
{
public:
    LspServer(int protocolFd, const std::string& runtimeDir, const std::string& importDir, bool verbose,
              unsigned int poolSizeOverride = 0)
        : loop_(protocolFd, verbose)
        , runtimeDir_(runtimeDir)
        , importSearchDir_(importDir)
        , verbose_(verbose)
        , currentIndex_(std::make_shared<LspSymbolIndex>())
    {
        // Backend pool sized to hardware_concurrency. Each backend owns its own
        // LLVMContext/Module so multiple analyses can run in parallel.
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        poolSize_ = hw;
        // CFLAT_LSP_POOL_SIZE overrides for diagnostics/testing.
        size_t envLen = 0;
        char envBuf[16] = {};
        if (getenv_s(&envLen, envBuf, sizeof(envBuf), "CFLAT_LSP_POOL_SIZE") == 0 && envLen > 0)
        {
            int v = std::atoi(envBuf);
            if (v > 0) poolSize_ = (unsigned int)v;
        }
        // The --lsp-pool-size switch takes precedence over the env var.
        if (poolSizeOverride > 0)
            poolSize_ = poolSizeOverride;
        backendPool_.reserve(poolSize_);
        backendAnalyzed_.resize(poolSize_, false);
        for (unsigned int i = 0; i < poolSize_; ++i)
        {
            auto b = std::make_unique<LLVMBackend>();
            b->SetRuntimeDir(runtimeDir_);
            b->SetVerbose(verbose_);
            backendPool_.push_back(std::move(b));
            freeBackends_.push_back(i);
        }

        // Worker pool - one thread per backend slot.
        workers_.reserve(poolSize_);
        for (unsigned int i = 0; i < poolSize_; ++i)
            workers_.emplace_back([this] { WorkerLoop(); });

        if (verbose_)
            std::cerr << std::format("[lsp] backend pool size: {}\n", poolSize_);
    }

    ~LspServer()
    {
        {
            std::unique_lock<std::mutex> lock(debounceMutex_);
            stopDebounce_ = true;
            debounceCV_.notify_all();
        }
        if (debounceThread_.joinable())
            debounceThread_.join();

        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            stopWorkers_ = true;
            jobCV_.notify_all();
        }
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    void Run()
    {
        loop_.Run([this](const nlohmann::json& msg) { HandleMessage(msg); });
    }

private:
    void HandleMessage(const nlohmann::json& msg)
    {
        std::string method = msg.value("method", "");
        if (method.empty()) return;

        std::optional<nlohmann::json> id;
        if (msg.contains("id") && !msg["id"].is_null())
            id = msg["id"];

        if (method == "initialize")
            HandleInitialize(id);
        else if (method == "initialized")
            {}  // no-op notification
        else if (method == "textDocument/didOpen")
            HandleDidOpen(msg);
        else if (method == "textDocument/didChange")
            HandleDidChange(msg);
        else if (method == "textDocument/didSave")
            HandleDidSave(msg);
        else if (method == "textDocument/didClose")
            HandleDidClose(msg);
        else if (method == "textDocument/hover")
            HandleHover(msg, id);
        else if (method == "textDocument/definition")
            HandleDefinition(msg, id);
        else if (method == "textDocument/completion")
            HandleCompletion(msg, id);
        else if (method == "cflat/runDiagnostics")
            HandleRunDiagnostics(msg);
        else if (method == "shutdown")
        {
            shutdownReceived_ = true;
            SendResponse(id, nullptr);
        }
        else if (method == "exit")
            {}  // handled by loop exit condition
        else if (id)
        {
            // Unknown request - send method-not-found error
            nlohmann::json err = {
                {"code", -32601},
                {"message", "Method not found: " + method}
            };
            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id", *id},
                {"error", std::move(err)}
            };
            loop_.Send(std::move(resp));
        }
    }

    void HandleInitialize(const std::optional<nlohmann::json>& id)
    {
        nlohmann::json result = {
            {"capabilities", {
                {"textDocumentSync", 1},  // 1 = Full sync
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"completionProvider", {
                    {"triggerCharacters", {".", ">", ":", "#"}}
                }}
                // Unused-code hints are pushed as diagnostics carrying the Unnecessary
                // tag (see PublishDiagnostics); the client renders those faded with no
                // extra server capability required.
            }},
            {"serverInfo", {
                {"name", "cflat LSP"},
                {"version", "1.0"}
            }}
        };
        SendResponse(id, std::move(result));
    }

    void HandleDidOpen(const nlohmann::json& msg)
    {
        const auto* params = GetParams(msg);
        if (!params) return;
        if (!params->contains("textDocument") || !(*params)["textDocument"].is_object()) return;
        const auto& textDoc = (*params)["textDocument"];

        OpenDocument doc;
        doc.uri      = textDoc.value("uri",  "");
        doc.filePath = uriToFilePath(doc.uri);
        doc.text     = textDoc.value("text", "");
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            docs_[doc.uri] = doc;
        }
        ScheduleAnalysis(doc.uri, doc.filePath, doc.text, /*immediate=*/true);
    }

    void HandleDidChange(const nlohmann::json& msg)
    {
        const auto* params = GetParams(msg);
        if (!params) return;
        if (!params->contains("textDocument") || !(*params)["textDocument"].is_object()) return;

        std::string uri = (*params)["textDocument"].value("uri", "");
        std::string text;
        if (params->contains("contentChanges") && (*params)["contentChanges"].is_array())
        {
            // Full sync: use last change
            for (const auto& change : (*params)["contentChanges"])
                if (change.is_object())
                    text = change.value("text", "");
        }

        std::string filePath;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
            {
                it->second.text = text;
                filePath = it->second.filePath;
            }
        }
        ScheduleAnalysis(uri, filePath, text, /*immediate=*/false);
    }

    void HandleDidSave(const nlohmann::json& msg)
    {
        const auto* params = GetParams(msg);
        if (!params) return;
        if (!params->contains("textDocument") || !(*params)["textDocument"].is_object()) return;

        std::string uri = (*params)["textDocument"].value("uri", "");
        std::string text;
        std::string filePath;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
            {
                text     = it->second.text;
                filePath = it->second.filePath;
            }
        }
        if (!filePath.empty())
            ScheduleAnalysis(uri, filePath, text, /*immediate=*/true);
    }

    void HandleDidClose(const nlohmann::json& msg)
    {
        const auto* params = GetParams(msg);
        if (!params) return;
        if (!params->contains("textDocument") || !(*params)["textDocument"].is_object()) return;

        std::string uri = (*params)["textDocument"].value("uri", "");
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            docs_.erase(uri);
        }
        PublishDiagnostics(uri, {});
    }

    // Returns a pointer to msg["params"] if it exists and is an object, else nullptr.
    static const nlohmann::json* GetParams(const nlohmann::json& msg)
    {
        if (!msg.contains("params") || !msg["params"].is_object()) return nullptr;
        return &msg["params"];
    }

    static void ExtractTextDocPosition(const nlohmann::json& params, std::string& uri, int& line, int& character)
    {
        if (params.contains("textDocument") && params["textDocument"].is_object())
            uri = params["textDocument"].value("uri", "");
        if (params.contains("position") && params["position"].is_object())
        {
            line      = params["position"].value("line",      0);
            character = params["position"].value("character", 0);
        }
    }

    void HandleHover(const nlohmann::json& msg, const std::optional<nlohmann::json>& id)
    {
        const auto* params = GetParams(msg);
        if (!params) { SendResponse(id, nullptr); return; }

        std::string uri;
        int line = 0, character = 0;
        ExtractTextDocPosition(*params, uri, line, character);

        std::string text;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
                text = it->second.text;
        }

        std::string word = extractWordAt(text, line, character);
        auto index = GetCurrentIndex();
        const SymbolDef* def = word.empty() ? nullptr : index->Lookup(word);

        if (!def) { SendResponse(id, nullptr); return; }

        std::string value = def->signatureMarkdown;
        for (const auto& sig : def->overloadSignatures)
            value += "\n\n" + sig;
        if (!def->docComment.empty())
            value += "\n\n" + def->docComment;

        SendResponse(id, {
            {"contents", {
                {"kind", "markdown"},
                {"value", value}
            }}
        });
    }

    // For a generic instantiation mangled as "array__Explosion", returns "array".
    // Returns the name unchanged if no "__" separator is present.
    static std::string StripGenericSuffix(const std::string& name)
    {
        size_t dunder = name.find("__");
        return dunder != std::string::npos ? name.substr(0, dunder) : name;
    }

    void HandleDefinition(const nlohmann::json& msg, const std::optional<nlohmann::json>& id)
    {
        const auto* params = GetParams(msg);
        if (!params) { SendResponse(id, nullptr); return; }

        std::string uri;
        int line = 0, character = 0;
        ExtractTextDocPosition(*params, uri, line, character);

        std::string text;
        std::string filePath;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
            {
                text     = it->second.text;
                filePath = it->second.filePath;
            }
        }

        // Import line: jump directly to the imported file.
        std::string importFile = extractImportFilename(text, line);
        if (!importFile.empty())
        {
            std::string resolved = resolveImportPath(filePath, importFile, importSearchDir_, runtimeDir_);
            if (!resolved.empty())
            {
                lsp::Range range{ {0, 0}, {0, 0} };
                SendResponse(id, nlohmann::json::array({ lsp::locationToJson(lsp::Location{ filePathToUri(resolved), range }) }));
                return;
            }
        }

        std::string word = extractWordAt(text, line, character);
        auto index = GetCurrentIndex();
        const SymbolDef* def = nullptr;

        // If the cursor follows "receiver.", resolve via the receiver first.
        // Otherwise an unqualified Lookup(word) on a method name like "get" can
        // hijack the answer with a same-named top-level function from elsewhere
        // (e.g. string.cb's `get`).
        std::string receiver;
        if (!word.empty())
        {
            auto rp = extractReceiverAt(text, line, character);
            receiver = rp.first;
        }
        if (!receiver.empty())
        {
            const std::string* typeName = index->LookupVariableType(receiver);
            if (typeName)
            {
                // Variable receiver: try "Circle.center", then unqualified type "Circle.center".
                def = index->Lookup(*typeName + "." + word);
                if (!def)
                {
                    size_t dot = typeName->rfind('.');
                    if (dot != std::string::npos)
                        def = index->Lookup(typeName->substr(dot + 1) + "." + word);
                }
                // Generic instantiation: type is mangled (e.g. "array__Explosion").
                // Fall back to the template name ("array") so the method on the
                // generic template definition is discoverable.
                if (!def)
                    def = index->Lookup(StripGenericSuffix(*typeName) + "." + word);
            }
            else
            {
                // Namespace or type name used directly (e.g. "Math.square", "MyStruct.staticFn").
                def = index->Lookup(receiver + "." + word);
            }
        }

        // Plain identifier (no receiver): direct symbol lookup.
        if (!def && receiver.empty() && !word.empty())
            def = index->Lookup(word);

        // Cursor is on a variable name. Prefer jumping to the variable's own
        // declaration site when we recorded one; otherwise fall through to
        // the type's definition (legacy behavior for unlocated entries).
        if (!def && receiver.empty() && !word.empty())
        {
            if (const VariableInfo* vi = index->LookupVariable(word))
            {
                if (vi->line > 0 && !vi->file.empty())
                {
                    lsp::Range range{
                        { vi->line - 1, vi->column },
                        { vi->line - 1, vi->column + (int)word.size() }
                    };
                    lsp::Location loc{ filePathToUri(vi->file), range };
                    SendResponse(id, nlohmann::json::array({ lsp::locationToJson(loc) }));
                    return;
                }
                if (!vi->typeName.empty())
                {
                    def = index->Lookup(vi->typeName);
                    if (!def)
                    {
                        size_t dot = vi->typeName.rfind('.');
                        if (dot != std::string::npos)
                            def = index->Lookup(vi->typeName.substr(dot + 1));
                    }
                }
            }
        }

        if (!def) { SendResponse(id, nullptr); return; }

        lsp::Range range{
            { def->line - 1, def->column },
            { def->line - 1, def->column + (int)def->name.size() }
        };
        lsp::Location loc{ filePathToUri(def->file), range };
        SendResponse(id, nlohmann::json::array({ lsp::locationToJson(loc) }));
    }

    void HandleCompletion(const nlohmann::json& msg, const std::optional<nlohmann::json>& id)
    {
        int line = 0, character = 0;
        std::string uri, text;
        if (const auto* params = GetParams(msg))
            ExtractTextDocPosition(*params, uri, line, character);
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
                text = it->second.text;
        }

        auto index = GetCurrentIndex();
        nlohmann::json items = nlohmann::json::array();

        auto [receiver, partial] = extractReceiverAt(text, line, character);
        if (!receiver.empty())
        {
            // Resolve variable name to its type; fall back to treating receiver as type/namespace name.
            std::string typeName = receiver;
            if (auto* vt = index->LookupVariableType(receiver))
                typeName = *vt;

            // LookupPrefix("TypeName.partial") returns matching fields and methods.
            auto members = index->LookupPrefix(typeName + "." + partial);
            // Generic instantiation: also try the template name (e.g. "array" from
            // "array__Explosion") so methods defined on the template show up.
            if (members.empty())
                members = index->LookupPrefix(StripGenericSuffix(typeName) + "." + partial);
            for (const SymbolDef* def : members)
            {
                // Strip the qualifying type prefix ("X.method" -> "method") regardless
                // of whether the prefix is the variable's actual type or its template.
                size_t dot = def->name.rfind('.');
                std::string label = dot != std::string::npos
                    ? def->name.substr(dot + 1)
                    : def->name;
                nlohmann::json item = {{"label", label}, {"detail", def->signatureMarkdown}};
                switch (def->kind)
                {
                    case SymbolKind::Function: item["kind"] = lsp::CompletionItemKindFunction; break;
                    case SymbolKind::Field:    item["kind"] = lsp::CompletionItemKindField;    break;
                    default: break;
                }
                items.push_back(std::move(item));
            }
        }
        else
        {
            // Global prefix completion.
            std::string prefix = extractWordAt(text, line, character);
            auto defs = index->LookupPrefix(prefix);
            for (const SymbolDef* def : defs)
            {
                nlohmann::json item = {
                    {"label",  def->name},
                    {"detail", def->signatureMarkdown}
                };
                switch (def->kind)
                {
                    case SymbolKind::Function:  item["kind"] = lsp::CompletionItemKindFunction;  break;
                    case SymbolKind::Struct:    item["kind"] = lsp::CompletionItemKindStruct;    break;
                    case SymbolKind::Interface: item["kind"] = lsp::CompletionItemKindInterface; break;
                    case SymbolKind::Namespace: item["kind"] = lsp::CompletionItemKindModule;    break;
                    case SymbolKind::TypeAlias: item["kind"] = lsp::CompletionItemKindTypeParam; break;
                    case SymbolKind::Field:     item["kind"] = lsp::CompletionItemKindField;     break;
                }
                items.push_back(std::move(item));
            }
        }
        SendResponse(id, { {"isIncomplete", false}, {"items", std::move(items)} });
    }

    void HandleRunDiagnostics(const nlohmann::json& msg)
    {
        const auto* params = GetParams(msg);
        if (!params) return;

        std::string uri      = params->value("uri", "");
        std::string filePath = uriToFilePath(uri);
        std::string text;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
                text = it->second.text;
        }
        if (!filePath.empty())
            EnqueueAnalysis(uri, filePath, text);
    }

    // -----------------------------------------------------------------------
    // Job dispatch - every analysis goes through the worker pool.
    // -----------------------------------------------------------------------

    struct AnalysisJob
    {
        std::string uri;
        std::string filePath;
        std::string text;
        uint64_t    generation = 0;
    };

    void EnqueueAnalysis(const std::string& uri, const std::string& filePath, const std::string& text)
    {
        AnalysisJob job{uri, filePath, text};
        {
            std::lock_guard<std::mutex> lock(uriGenMutex_);
            job.generation = ++uriGeneration_[uri];
        }
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            jobQueue_.push_back(std::move(job));
        }
        jobCV_.notify_one();
    }

    void ScheduleAnalysis(const std::string& uri, const std::string& filePath,
                          const std::string& text, bool immediate)
    {
        if (immediate)
        {
            EnqueueAnalysis(uri, filePath, text);
            return;
        }

        {
            std::unique_lock<std::mutex> lock(debounceMutex_);
            pendingUri_      = uri;
            pendingFilePath_ = filePath;
            pendingText_     = text;
            debounceGeneration_++;
            debounceCV_.notify_all();
        }

        if (!debounceThread_.joinable())
            debounceThread_ = std::thread([this] { DebounceWorker(); });
    }

    void DebounceWorker()
    {
        uint64_t lastFiredGen = 0;
        while (true)
        {
            uint64_t gen;
            std::string uri, filePath, text;
            {
                std::unique_lock<std::mutex> lock(debounceMutex_);
                // Wait until there's something new to debounce, or shutdown.
                debounceCV_.wait(lock, [&] {
                    return stopDebounce_ || debounceGeneration_ != lastFiredGen;
                });
                if (stopDebounce_) return;
                gen      = debounceGeneration_;
                uri      = pendingUri_;
                filePath = pendingFilePath_;
                text     = pendingText_;
            }

            // Wait the quiet period; if a new change arrives, restart the wait.
            {
                std::unique_lock<std::mutex> lock(debounceMutex_);
                debounceCV_.wait_for(lock, std::chrono::milliseconds(250),
                    [this, gen] { return stopDebounce_ || debounceGeneration_ != gen; });
                if (stopDebounce_) return;
                if (debounceGeneration_ != gen) continue;  // newer change, redo
            }

            lastFiredGen = gen;
            EnqueueAnalysis(uri, filePath, text);
        }
    }

    // -----------------------------------------------------------------------
    // Worker pool - each thread owns one backend slot.
    // -----------------------------------------------------------------------

    void WorkerLoop()
    {
        while (true)
        {
            AnalysisJob job;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobCV_.wait(lock, [&] { return stopWorkers_ || !jobQueue_.empty(); });
                if (stopWorkers_) return;
                job = std::move(jobQueue_.front());
                jobQueue_.pop_front();
            }

            // Drop superseded jobs - a newer revision of this URI has been enqueued.
            {
                std::lock_guard<std::mutex> lock(uriGenMutex_);
                auto it = uriGeneration_.find(job.uri);
                if (it != uriGeneration_.end() && it->second != job.generation)
                    continue;
            }

            // Reserve a backend slot.
            size_t slot;
            {
                std::unique_lock<std::mutex> lock(backendMutex_);
                backendCV_.wait(lock, [&] { return !freeBackends_.empty(); });
                slot = freeBackends_.back();
                freeBackends_.pop_back();
            }

            // A single analysis must never take down the whole server: any C++ exception
            // that escapes here would call std::terminate on this worker thread. Catch it,
            // and always release the backend slot so the pool can't deadlock.
            try
            {
                RunAnalysisOnSlot(slot, job);
            }
            catch (const std::exception& e)
            {
                if (verbose_) std::cerr << std::format("[lsp] analysis threw on slot {}: {}\n", slot, e.what());
            }
            catch (...)
            {
                if (verbose_) std::cerr << std::format("[lsp] analysis threw on slot {} (unknown)\n", slot);
            }

            {
                std::lock_guard<std::mutex> lock(backendMutex_);
                freeBackends_.push_back(slot);
            }
            backendCV_.notify_one();
        }
    }

    void RunAnalysisOnSlot(size_t slot, const AnalysisJob& job)
    {
        const std::string& uri      = job.uri;
        const std::string& filePath = job.filePath;
        const std::string& text     = job.text;

        char tempDirBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tempDirBuf);
        std::string tempPath;
        {
            int counter;
            { std::lock_guard<std::mutex> lock(tempCounterMutex_); counter = tempFileCounter_++; }
            tempPath = std::string(tempDirBuf) +
                std::format("cflat_lsp_{}_{}_{}.cb", GetCurrentProcessId(), (int)slot, counter);
        }

        {
            std::ofstream tmp(tempPath, std::ios::binary);
            if (!tmp) return;
            tmp.write(text.data(), text.size());
        }

        std::vector<lsp::Diagnostic> diagnostics;
        auto newIndex = std::make_shared<LspSymbolIndex>();

        LLVMBackend* backend = backendPool_[slot].get();

        // Each backend slot is sticky across runs - reset before re-use.
        if (backendAnalyzed_[slot])
            backend->ResetForReanalysis();
        backendAnalyzed_[slot] = true;

        if (!filePath.empty())
        {
            std::filesystem::path fp(filePath);
            backend->SetSourceFileDir(fp.parent_path().string());
            backend->SetSourceDisplayName(fp.filename().string());  // make diagnostics point at the real document
        }
        else
        {
            backend->SetSourceDisplayName("");
        }

        backend->SetDiagnosticSink([&](const std::string& /*file*/, size_t line, size_t col, const std::string& msg, int severity)
        {
            lsp::Diagnostic diag;
            diag.range    = { { (int)line - 1, (int)col }, { (int)line - 1, (int)col + 1 } };
            diag.message  = msg;
            diag.severity = severity;
            diagnostics.push_back(diag);
        });
        // Faded hint regions reported during analysis (e.g. unreachable code). Positions
        // are document-relative (same text as the temp copy), so no remap is needed.
        backend->SetHintRegionSink([&](int sl, int sc, int el, int ec, const std::string& msg)
        {
            lsp::Diagnostic diag;
            diag.range    = { { sl - 1, sc }, { el - 1, ec } };
            diag.message  = msg;
            diag.severity = lsp::DiagnosticSeverityHint;
            diag.tags     = { lsp::DiagnosticTagUnnecessary };
            diagnostics.push_back(diag);
        });
        backend->SetSymbolSink(newIndex.get());

        bool ok = false;
        llvm::CrashRecoveryContext crc;
        bool recovered = crc.RunSafely([&]
        {
            ok = backend->Analyze(tempPath, importSearchDir_, runtimeDir_);
        });

        if (!recovered)
        {
            if (verbose_) std::cerr << std::format("[lsp] compiler crash on slot {}, replacing backend\n", slot);
            auto fresh = std::make_unique<LLVMBackend>();
            fresh->SetRuntimeDir(runtimeDir_);
            fresh->SetVerbose(verbose_);
            backendPool_[slot] = std::move(fresh);
            backendAnalyzed_[slot] = false;

            lsp::Diagnostic crashDiag;
            crashDiag.range   = { {0, 0}, {0, 1} };
            crashDiag.message = "Internal compiler error during analysis";
            diagnostics.push_back(crashDiag);
        }
        else
        {
            backend->SetDiagnosticSink(nullptr);
            backend->SetHintRegionSink(nullptr);
            backend->SetSymbolSink(nullptr);
        }

        // Non-throwing remove: a child process spawned during analysis (clang-cl, for
        // C-interop signature extraction) can transiently hold an inherited handle to a
        // temp .cb, making removal fail with a sharing violation. The throwing overload
        // would escape this worker thread and call std::terminate; tolerate the failure
        // (the temp file lives in %TEMP% and is reaped later) instead of crashing.
        {
            std::error_code rmEc;
            std::filesystem::remove(tempPath, rmEc);
        }

        if (!filePath.empty())
        {
            newIndex->RemapFile(tempPath, filePath);
            newIndex->RemapFile(std::filesystem::path(tempPath).filename().string(), filePath);
        }

        // Last-known-good cache - kept global for now. Edits land sequentially in
        // practice (debounce + single editor); bulk sweeps don't use this index.
        {
            std::lock_guard<std::mutex> lock(indexMutex_);
            bool hasErrors    = !diagnostics.empty();
            size_t newCount   = newIndex->SymbolCount();
            size_t cachedCount = currentIndex_ ? currentIndex_->SymbolCount() : 0;
            bool keepCached   = hasErrors && newCount < cachedCount;

            if (!keepCached)
                currentIndex_ = newIndex;
            else if (verbose_)
                std::cerr << std::format("[lsp] keeping cached index ({} symbols) over partial parse ({} symbols)\n", cachedCount, newCount);
        }

        // Occurrence-based unused-code hints (functions / locals / params / imports).
        // Suppressed while the file has errors: the candidate and use sets are then
        // incomplete (e.g. a call site in a half-typed region), which would produce
        // spurious "unused" hints mid-edit. Unreachable-code hints are unaffected -
        // they were already emitted by the backend during analysis.
        if (!filePath.empty())
        {
            bool hasError = false;
            for (const auto& d : diagnostics)
                if (d.severity == lsp::DiagnosticSeverityError) { hasError = true; break; }
            if (!hasError)
                AppendUnusedDiagnostics(text, filePath, *newIndex, diagnostics);
        }

        PublishDiagnostics(uri, diagnostics);
    }

    // Build faded "never used" hints by cross-referencing the declarations recorded
    // during analysis against an identifier-occurrence scan of the document. Only
    // declarations in this file are considered; the linkage/RAII guards were applied
    // at registration time (see UnusedCandidate). Underreports rather than misreports.
    void AppendUnusedDiagnostics(const std::string& text, const std::string& filePath,
                                 const LspSymbolIndex& index,
                                 std::vector<lsp::Diagnostic>& out)
    {
        auto counts = scanIdentifierOccurrences(text);

        auto addFaded = [&](int line1, int col0, int len, const std::string& msg)
        {
            lsp::Diagnostic d;
            d.range    = { { line1 - 1, col0 }, { line1 - 1, col0 + len } };
            d.message  = msg;
            d.severity = lsp::DiagnosticSeverityHint;
            d.tags     = { lsp::DiagnosticTagUnnecessary };
            out.push_back(std::move(d));
        };

        // Functions / locals / params declared in this file with no use beyond the
        // declaration itself (occurrence count of 1).
        auto isPlainIdentifier = [](const std::string& s) {
            if (s.empty() || std::isdigit((unsigned char)s[0])) return false;
            for (char c : s)
                if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
            return true;
        };

        for (const auto& cand : index.Candidates())
        {
            if (cand.file != filePath) continue;
            if (cand.isExported || cand.hasDestructor) continue;
            if (cand.kind == SymbolKind::Function && cand.name == "main") continue;
            // Non-identifier names (operators, mangled forms) can never appear as a bare
            // token in the scan, so we cannot prove them unused - skip to stay sound.
            if (!isPlainIdentifier(cand.name)) continue;

            auto it = counts.find(cand.name);
            int n = (it != counts.end()) ? it->second : 0;
            if (n > 1) continue;  // referenced somewhere beyond its declaration

            std::string what = (cand.kind == SymbolKind::Function)
                ? "function '" + cand.name + "' is never used"
                : "'" + cand.name + "' is never used";
            addFaded(cand.line, cand.col, (int)cand.name.size(), what);
        }

        AppendUnusedImports(text, filePath, index, counts, out);
    }

    // An `import "x.cb"` is flagged when none of the symbols that file defines are
    // referenced in this document. Caveat: a symbol used only transitively (never named
    // here) is invisible to the text scan, so a genuinely-needed import can be flagged;
    // the hint is faded and trivially dismissed. Unresolvable imports are left alone.
    void AppendUnusedImports(const std::string& text, const std::string& filePath,
                             const LspSymbolIndex& index,
                             const std::unordered_map<std::string, int>& counts,
                             std::vector<lsp::Diagnostic>& out)
    {
        // Map each canonical defining-file to the set of names it defines.
        // canonCache avoids repeated canonical() syscalls for symbols that share a file.
        std::unordered_map<std::string, std::string> canonCache;
        auto canon = [&canonCache](const std::string& p) -> const std::string& {
            auto [it, inserted] = canonCache.emplace(p, std::string{});
            if (inserted)
            {
                std::error_code ec;
                auto c = std::filesystem::canonical(std::filesystem::path(p).lexically_normal(), ec);
                it->second = ec ? std::string{} : c.string();
            }
            return it->second;
        };

        std::unordered_map<std::string, std::vector<std::string>> namesByFile;
        for (const auto& [name, def] : index.Symbols())
        {
            if (def.file.empty() || def.file == filePath) continue;
            const std::string& key = canon(def.file);
            if (!key.empty()) namesByFile[key].push_back(name);
        }

        // Pre-split text into lines to avoid O(N^2) repeated scans in extractImportFilename.
        std::vector<std::string_view> lines;
        {
            const char* p = text.data(), *end = p + text.size(), *lstart = p;
            while (p != end)
            {
                if (*p == '\n') { lines.emplace_back(lstart, p - lstart); lstart = p + 1; }
                ++p;
            }
            lines.emplace_back(lstart, end - lstart);
        }

        for (int line = 0; line < (int)lines.size(); ++line)
        {
            // Parse the import filename directly from the pre-split line (O(1) per line).
            const std::string_view ln = lines[line];
            size_t i = 0;
            while (i < ln.size() && std::isspace((unsigned char)ln[i])) i++;
            if (ln.compare(i, 6, "import") != 0) continue;
            i += 6;
            while (i < ln.size() && std::isspace((unsigned char)ln[i])) i++;
            if (i >= ln.size() || ln[i] != '"') continue;
            i++;
            size_t fnStart = i;
            while (i < ln.size() && ln[i] != '"') i++;
            if (i >= ln.size()) continue;
            std::string fname(ln.substr(fnStart, i - fnStart));
            if (!fname.ends_with(".cb")) continue;

            std::string resolved = resolveImportPath(filePath, fname, importSearchDir_, runtimeDir_);
            if (resolved.empty()) continue;
            auto it = namesByFile.find(canon(resolved));
            if (it == namesByFile.end() || it->second.empty()) continue;  // nothing to conclude

            bool anyUsed = false;
            for (const auto& nm : it->second)
            {
                auto cIt = counts.find(nm);
                if (cIt != counts.end() && cIt->second > 0) { anyUsed = true; break; }
            }
            if (anyUsed) continue;

            // Compute column range from the pre-split line (no extra scan).
            int startCol = 0, endCol = (int)ln.size();
            while (startCol < endCol && std::isspace((unsigned char)ln[startCol])) startCol++;
            lsp::Diagnostic d;
            d.range    = { { line, startCol }, { line, endCol } };
            d.message  = "unused import '" + fname + "'";
            d.severity = lsp::DiagnosticSeverityHint;
            d.tags     = { lsp::DiagnosticTagUnnecessary };
            out.push_back(std::move(d));
        }
    }

    void PublishDiagnostics(const std::string& uri, const std::vector<lsp::Diagnostic>& diags)
    {
        nlohmann::json diagArray = nlohmann::json::array();
        for (const auto& d : diags)
            diagArray.push_back(lsp::diagnosticToJson(d));

        nlohmann::json notification = {
            {"jsonrpc", "2.0"},
            {"method",  "textDocument/publishDiagnostics"},
            {"params",  {
                {"uri",         uri},
                {"diagnostics", std::move(diagArray)}
            }}
        };
        loop_.Send(std::move(notification));
    }

    void SendResponse(const std::optional<nlohmann::json>& id, nlohmann::json result)
    {
        if (!id) return;
        nlohmann::json resp = {
            {"jsonrpc", "2.0"},
            {"id",      *id},
            {"result",  std::move(result)}
        };
        loop_.Send(std::move(resp));
    }

    std::shared_ptr<LspSymbolIndex> GetCurrentIndex()
    {
        std::lock_guard<std::mutex> lock(indexMutex_);
        return currentIndex_;
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    JsonRpcLoop loop_;
    std::string runtimeDir_;
    std::string importSearchDir_;

    std::mutex docsMutex_;
    std::unordered_map<std::string, OpenDocument> docs_;

    // Backend pool - one LLVMBackend per slot. Workers check out a free slot,
    // run analysis, and return it. backendAnalyzed_[slot] tracks whether
    // ResetForReanalysis is needed before the next run on that slot.
    unsigned int poolSize_ = 1;
    std::vector<std::unique_ptr<LLVMBackend>> backendPool_;
    std::vector<bool> backendAnalyzed_;  // sized on first use in WorkerLoop via lazy resize
    std::vector<size_t> freeBackends_;
    std::mutex backendMutex_;
    std::condition_variable backendCV_;

    std::mutex tempCounterMutex_;
    int tempFileCounter_ = 0;

    // Job queue - every analysis (immediate or debounced) lands here.
    std::deque<AnalysisJob> jobQueue_;
    std::mutex jobMutex_;
    std::condition_variable jobCV_;
    bool stopWorkers_ = false;
    std::vector<std::thread> workers_;

    // Per-URI generation counter - older jobs for the same URI are dropped.
    std::mutex uriGenMutex_;
    std::unordered_map<std::string, uint64_t> uriGeneration_;

    std::mutex indexMutex_;
    std::shared_ptr<LspSymbolIndex> currentIndex_;

    std::thread debounceThread_;
    std::mutex debounceMutex_;
    std::condition_variable debounceCV_;
    uint64_t debounceGeneration_ = 0;
    bool stopDebounce_ = false;
    std::string pendingUri_;
    std::string pendingFilePath_;
    std::string pendingText_;

    bool verbose_ = false;
    bool shutdownReceived_ = false;
};

} // anonymous namespace

int RunLspServer(int argc, char* argv[])
{
    int protocolFd = _dup(_fileno(stdout));
    _setmode(protocolFd, _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    // stdout is reserved for the JSON-RPC channel (written via _write(protocolFd_)).
    // Redirect anything that writes to std::cout - LSP verbose logs and backend
    // diagnostics under -v - to stderr so it cannot corrupt the protocol stream.
    std::cout.rdbuf(std::cerr.rdbuf());

    // Prevent child processes (e.g. clang-cl spawned for C-interop signature extraction)
    // from inheriting the JSON-RPC pipe handles. _dup and the CRT std fds are inheritable
    // by default on Windows; an inherited protocol handle silently breaks the LSP channel
    // when the child runs, taking the server down with it.
    auto clearInherit = [](int fd)
    {
        intptr_t h = _get_osfhandle(fd);
        if (h != -1) SetHandleInformation((HANDLE)h, HANDLE_FLAG_INHERIT, 0);
    };
    clearInherit(protocolFd);
    clearInherit(_fileno(stdin));
    clearInherit(_fileno(stdout));

    llvm::CrashRecoveryContext::Enable();

    std::string runtimeDir = GetExeDir();

    bool verbose = false;
    std::string importDir;
    unsigned int poolSizeOverride = 0;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--verbose" || arg == "-v")
            verbose = true;
        else if ((arg == "--import-dir" || arg == "-i") && i + 1 < argc)
            importDir = argv[++i];
        else if (arg == "--lsp-pool-size" && i + 1 < argc)
        {
            int v = std::atoi(argv[++i]);
            if (v > 0) poolSizeOverride = (unsigned int)v;
        }
    }

    if (verbose) std::cerr << "[lsp] server starting\n";
    LspServer server(protocolFd, runtimeDir, importDir, verbose, poolSizeOverride);
    if (verbose) std::cerr << "[lsp] entering Run()\n";
    server.Run();
    if (verbose) std::cerr << "[lsp] Run() returned\n";
    return 0;
}
