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
#include <windows.h>
#include <nlohmann/json.hpp>

namespace {

// URI <-> file path conversion
std::string uriToFilePath(const std::string& uri)
{
    if (uri.size() < 8 || uri.substr(0, 8) != "file:///")
        return uri;
    std::string path = uri.substr(8);  // strip "file:///"
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
    std::string uri = "file:///";
    for (char c : path)
    {
        if (c == '\\') uri += '/';
        else if (c == ':') uri += "%3A";
        else uri += c;
    }
    return uri;
}

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
    if (ln.substr(i, 6) != "import") return {};
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

// Extract the word (identifier) at a given 0-based line/character position in text.
std::string extractWordAt(const std::string& text, int line, int character)
{
    int currentLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            if (currentLine == line)
                break;
            currentLine++;
            lineStart = i + 1;
        }
    }
    size_t col = static_cast<size_t>(character);
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    std::string lineText = text.substr(lineStart, lineEnd - lineStart);

    if (col >= lineText.size()) return {};

    auto isWord = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    size_t start = col;
    while (start > 0 && isWord(lineText[start - 1])) start--;
    size_t end = col;
    while (end < lineText.size() && isWord(lineText[end])) end++;
    return lineText.substr(start, end - start);
}

// Extract the identifier before a '.' at the given position, plus any partial identifier after it.
// Returns {"", ""} if the character before the cursor (or before any partial word) is not '.'.
std::pair<std::string, std::string> extractReceiverAt(const std::string& text, int line, int character)
{
    int currentLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            if (currentLine == line) break;
            currentLine++;
            lineStart = i + 1;
        }
    }
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    std::string lineText = text.substr(lineStart, lineEnd - lineStart);

    size_t col = static_cast<size_t>(character) < lineText.size() ? static_cast<size_t>(character) : lineText.size();
    auto isWord = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };

    // Scan backward over any partial identifier typed after the dot
    size_t partialStart = col;
    while (partialStart > 0 && isWord(lineText[partialStart - 1]))
        partialStart--;
    std::string partial = lineText.substr(partialStart, col - partialStart);

    // Expect a '.' immediately before the partial
    if (partialStart == 0 || lineText[partialStart - 1] != '.')
        return {"", ""};

    // Scan backward over the receiver identifier
    size_t dotPos = partialStart - 1;
    size_t recvEnd = dotPos;
    size_t recvStart = dotPos;
    while (recvStart > 0 && isWord(lineText[recvStart - 1]))
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
    LspServer(int protocolFd, const std::string& runtimeDir, const std::string& importDir, bool verbose)
        : loop_(protocolFd, verbose)
        , runtimeDir_(runtimeDir)
        , importSearchDir_(importDir)
        , verbose_(verbose)
        , compiler_(std::make_unique<LLVMBackend>())
        , currentIndex_(std::make_shared<LspSymbolIndex>())
    {
        compiler_->SetRuntimeDir(runtimeDir_);
        compiler_->SetVerbose(verbose_);
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
        else if (method == "mycompiler/runDiagnostics")
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
            // Unknown request — send method-not-found error
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
            }},
            {"serverInfo", {
                {"name", "MyCompiler LSP"},
                {"version", "1.0"}
            }}
        };
        SendResponse(id, std::move(result));
    }

    void HandleDidOpen(const nlohmann::json& msg)
    {
        if (!msg.contains("params") || !msg["params"].is_object()) return;
        const auto& params = msg["params"];
        if (!params.contains("textDocument") || !params["textDocument"].is_object()) return;
        const auto& textDoc = params["textDocument"];

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
        if (!msg.contains("params") || !msg["params"].is_object()) return;
        const auto& params = msg["params"];
        if (!params.contains("textDocument") || !params["textDocument"].is_object()) return;

        std::string uri = params["textDocument"].value("uri", "");
        std::string text;
        if (params.contains("contentChanges") && params["contentChanges"].is_array())
        {
            // Full sync: use last change
            for (const auto& change : params["contentChanges"])
                if (change.is_object())
                    text = change.value("text", "");
        }

        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
                it->second.text = text;
        }

        std::string filePath;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
                filePath = it->second.filePath;
        }
        ScheduleAnalysis(uri, filePath, text, /*immediate=*/false);
    }

    void HandleDidSave(const nlohmann::json& msg)
    {
        if (!msg.contains("params") || !msg["params"].is_object()) return;
        const auto& params = msg["params"];
        if (!params.contains("textDocument") || !params["textDocument"].is_object()) return;

        std::string uri = params["textDocument"].value("uri", "");
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
        if (!msg.contains("params") || !msg["params"].is_object()) return;
        const auto& params = msg["params"];
        if (!params.contains("textDocument") || !params["textDocument"].is_object()) return;

        std::string uri = params["textDocument"].value("uri", "");
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            docs_.erase(uri);
        }
        PublishDiagnostics(uri, {});
    }

    void HandleHover(const nlohmann::json& msg, const std::optional<nlohmann::json>& id)
    {
        if (!msg.contains("params") || !msg["params"].is_object())
            { SendResponse(id, nullptr); return; }
        const auto& params = msg["params"];

        std::string uri;
        int line = 0, character = 0;
        if (params.contains("textDocument") && params["textDocument"].is_object())
            uri = params["textDocument"].value("uri", "");
        if (params.contains("position") && params["position"].is_object())
        {
            line      = params["position"].value("line",      0);
            character = params["position"].value("character", 0);
        }

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

        SendResponse(id, {
            {"contents", {
                {"kind", "markdown"},
                {"value", def->signatureMarkdown}
            }}
        });
    }

    void HandleDefinition(const nlohmann::json& msg, const std::optional<nlohmann::json>& id)
    {
        if (!msg.contains("params") || !msg["params"].is_object())
            { SendResponse(id, nullptr); return; }
        const auto& params = msg["params"];

        std::string uri;
        int line = 0, character = 0;
        if (params.contains("textDocument") && params["textDocument"].is_object())
            uri = params["textDocument"].value("uri", "");
        if (params.contains("position") && params["position"].is_object())
        {
            line      = params["position"].value("line",      0);
            character = params["position"].value("character", 0);
        }

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
        const SymbolDef* def = word.empty() ? nullptr : index->Lookup(word);

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
        if (msg.contains("params") && msg["params"].is_object())
        {
            const auto& params = msg["params"];
            if (params.contains("position") && params["position"].is_object())
            {
                line      = params["position"].value("line",      0);
                character = params["position"].value("character", 0);
            }
            if (params.contains("textDocument") && params["textDocument"].is_object())
                uri = params["textDocument"].value("uri", "");
        }
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
            for (const SymbolDef* def : members)
            {
                std::string label = def->name.size() > typeName.size() + 1
                    ? def->name.substr(typeName.size() + 1)
                    : def->name;
                nlohmann::json item = {{"label", label}, {"detail", def->signatureMarkdown}};
                switch (def->kind)
                {
                    case SymbolKind::Function: item["kind"] = 3; break;  // Function
                    case SymbolKind::Field:    item["kind"] = 5; break;  // Field
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
                    case SymbolKind::Function:  item["kind"] = 3;  break;  // Function
                    case SymbolKind::Struct:    item["kind"] = 22; break;  // Struct
                    case SymbolKind::Interface: item["kind"] = 8;  break;  // Interface
                    case SymbolKind::Namespace: item["kind"] = 9;  break;  // Module
                    case SymbolKind::TypeAlias: item["kind"] = 25; break;  // TypeParameter
                    case SymbolKind::Field:     item["kind"] = 5;  break;  // Field
                }
                items.push_back(std::move(item));
            }
        }
        SendResponse(id, { {"isIncomplete", false}, {"items", std::move(items)} });
    }

    void HandleRunDiagnostics(const nlohmann::json& msg)
    {
        if (!msg.contains("params") || !msg["params"].is_object()) return;
        const auto& params = msg["params"];

        std::string uri      = params.value("uri", "");
        std::string filePath = uriToFilePath(uri);
        std::string text;
        {
            std::lock_guard<std::mutex> lock(docsMutex_);
            if (auto it = docs_.find(uri); it != docs_.end())
                text = it->second.text;
        }
        if (!filePath.empty())
            RunAnalysis(uri, filePath, text);
    }

    // -----------------------------------------------------------------------
    // Debounced analysis scheduling
    // -----------------------------------------------------------------------

    void ScheduleAnalysis(const std::string& uri, const std::string& filePath,
                          const std::string& text, bool immediate)
    {
        if (immediate)
        {
            {
                std::unique_lock<std::mutex> lock(debounceMutex_);
                pendingUri_      = uri;
                pendingFilePath_ = filePath;
                pendingText_     = text;
                debounceGeneration_++;
                debounceCV_.notify_all();
            }
            RunAnalysis(uri, filePath, text);
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
        while (true)
        {
            uint64_t gen;
            std::string uri, filePath, text;
            {
                std::unique_lock<std::mutex> lock(debounceMutex_);
                gen      = debounceGeneration_;
                uri      = pendingUri_;
                filePath = pendingFilePath_;
                text     = pendingText_;
            }

            {
                std::unique_lock<std::mutex> lock(debounceMutex_);
                debounceCV_.wait_for(lock, std::chrono::milliseconds(250),
                    [this, gen] { return stopDebounce_ || debounceGeneration_ != gen; });

                if (stopDebounce_) return;
                if (debounceGeneration_ != gen) continue;
            }

            RunAnalysis(uri, filePath, text);
        }
    }

    // -----------------------------------------------------------------------
    // Core analysis
    // -----------------------------------------------------------------------

    void RunAnalysis(const std::string& uri, const std::string& filePath, const std::string& text)
    {
        char tempDirBuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tempDirBuf);
        std::string tempPath = std::string(tempDirBuf) +
            std::format("cflat_lsp_{}_{}.cb", GetCurrentProcessId(), tempFileCounter_++);

        {
            std::ofstream tmp(tempPath, std::ios::binary);
            if (!tmp) return;
            tmp.write(text.data(), text.size());
        }

        std::vector<lsp::Diagnostic> diagnostics;
        auto newIndex = std::make_shared<LspSymbolIndex>();

        {
            std::lock_guard<std::mutex> lock(analysisMutex_);

            if (analysisCount_ > 0)
                compiler_->ResetForReanalysis();
            analysisCount_++;

            // Let the compiler resolve relative imports from the real source directory,
            // not the %TEMP% directory where the temp analysis file lives.
            if (!filePath.empty())
                compiler_->SetSourceFileDir(
                    std::filesystem::path(filePath).parent_path().string());

            compiler_->SetDiagnosticSink([&](const std::string& file, size_t line, size_t col, const std::string& msg)
            {
                lsp::Diagnostic diag;
                diag.range   = { { (int)line - 1, (int)col }, { (int)line - 1, (int)col + 1 } };
                diag.message = msg;
                diagnostics.push_back(diag);
            });

            compiler_->SetSymbolSink(newIndex.get());

            bool ok = false;
            llvm::CrashRecoveryContext crc;
            bool recovered = crc.RunSafely([&]
            {
                ok = compiler_->Analyze(tempPath, importSearchDir_, runtimeDir_);
            });

            if (!recovered)
            {
                if (verbose_) std::cerr << "[lsp] compiler crash during analysis, resetting\n";
                compiler_ = std::make_unique<LLVMBackend>();
                compiler_->SetRuntimeDir(runtimeDir_);
                compiler_->SetVerbose(verbose_);
                analysisCount_ = 0;

                lsp::Diagnostic crashDiag;
                crashDiag.range   = { {0, 0}, {0, 1} };
                crashDiag.message = "Internal compiler error during analysis";
                diagnostics.push_back(crashDiag);
            }

            compiler_->SetDiagnosticSink(nullptr);
            compiler_->SetSymbolSink(nullptr);
        }

        std::filesystem::remove(tempPath);

        // Symbols were registered under the temp path; remap to the real source path
        // so that GoToDefinition jumps to the actual file, not the temp file.
        // The compiler stores only the filename component in def.file (e.g. "cflat_lsp_42.cb"),
        // not the full temp path, so we remap both forms.
        if (!filePath.empty())
        {
            newIndex->RemapFile(tempPath, filePath);
            newIndex->RemapFile(std::filesystem::path(tempPath).filename().string(), filePath);
        }

        {
            std::lock_guard<std::mutex> lock(indexMutex_);
            currentIndex_ = newIndex;
        }

        PublishDiagnostics(uri, diagnostics);
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

    std::mutex analysisMutex_;
    std::unique_ptr<LLVMBackend> compiler_;
    int analysisCount_    = 0;
    int tempFileCounter_  = 0;

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
    std::cout.rdbuf(std::cerr.rdbuf());

    llvm::CrashRecoveryContext::Enable();

    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    std::string runtimeDir = std::filesystem::path(pgmptr ? pgmptr : "").parent_path().string();

    bool verbose = false;
    std::string importDir;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--verbose" || arg == "-v")
            verbose = true;
        else if ((arg == "--import-dir" || arg == "-i") && i + 1 < argc)
            importDir = argv[++i];
    }

    if (verbose) _write(2, "[lsp] server starting\n", 22);
    LspServer server(protocolFd, runtimeDir, importDir, verbose);
    if (verbose) _write(2, "[lsp] entering Run()\n", 21);
    server.Run();
    if (verbose) _write(2, "[lsp] Run() returned\n", 21);
    return 0;
}
