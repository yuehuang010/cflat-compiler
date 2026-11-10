#pragma once

struct CHeaderRefusalDep
{
        std::string path;
        int64_t mtime = 0;
        uintmax_t size = 0;
};

struct CHeaderRefusalMru
{
        std::mutex mutex;
        uint64_t groupKey = 0;
        std::string validityKey;
        std::string text;
        std::vector<CHeaderRefusalDep> deps;
        bool valid = false;
};

inline CHeaderRefusalMru gCHeaderRefusalMru;

inline std::string CHeaderRefusalValidityKey(
    uint64_t groupKey, const std::vector<std::string>& paths,
    const std::vector<int64_t>& mtimes, uint64_t contentHash)
{
        std::string key = std::to_string(groupKey);
        for (size_t i = 0; i < paths.size(); ++i)
            key += "|H" + paths[i] + "|M" + std::to_string(mtimes[i]);
        key += "|X" + std::to_string(contentHash);
        return key;
}

inline bool LookupCHeaderRefusal(uint64_t groupKey, const std::string& validityKey,
                                 std::string& text, std::string& stalePath)
{
        std::lock_guard<std::mutex> lock(gCHeaderRefusalMru.mutex);
        if (!gCHeaderRefusalMru.valid || gCHeaderRefusalMru.groupKey != groupKey
            || gCHeaderRefusalMru.validityKey != validityKey)
            return false;
        for (const auto& dep : gCHeaderRefusalMru.deps)
        {
            std::error_code mtimeEc;
            std::error_code sizeEc;
            const auto mtime = std::filesystem::last_write_time(dep.path, mtimeEc);
            const auto size = std::filesystem::file_size(dep.path, sizeEc);
            if (mtimeEc || sizeEc || (int64_t)mtime.time_since_epoch().count() != dep.mtime
                || size != dep.size)
            {
                stalePath = dep.path;
                gCHeaderRefusalMru.validityKey.clear();
                gCHeaderRefusalMru.text.clear();
                gCHeaderRefusalMru.deps.clear();
                gCHeaderRefusalMru.valid = false;
                return false;
            }
        }
        text = gCHeaderRefusalMru.text;
        return true;
}

inline void RememberCHeaderRefusal(uint64_t groupKey, std::string validityKey,
                                   std::string text, std::vector<CHeaderRefusalDep> deps)
{
        if (deps.empty()) return;
        std::lock_guard<std::mutex> lock(gCHeaderRefusalMru.mutex);
        gCHeaderRefusalMru.groupKey = groupKey;
        gCHeaderRefusalMru.validityKey = std::move(validityKey);
        gCHeaderRefusalMru.text = std::move(text);
        gCHeaderRefusalMru.deps = std::move(deps);
        gCHeaderRefusalMru.valid = true;
}

inline void ClearCHeaderRefusal(uint64_t groupKey)
{
        std::lock_guard<std::mutex> lock(gCHeaderRefusalMru.mutex);
        if (gCHeaderRefusalMru.valid && gCHeaderRefusalMru.groupKey == groupKey)
        {
            gCHeaderRefusalMru.validityKey.clear();
            gCHeaderRefusalMru.text.clear();
            gCHeaderRefusalMru.deps.clear();
            gCHeaderRefusalMru.valid = false;
        }
}

inline bool CaptureCHeaderRefusalDeps(const std::vector<std::string>& includes,
                                      std::vector<CHeaderRefusalDep>& deps)
{
        std::unordered_set<std::string> seen;
        for (const auto& include : includes)
        {
            std::error_code mtimeEc;
            std::error_code sizeEc;
            const auto mtime = std::filesystem::last_write_time(include, mtimeEc);
            const auto size = std::filesystem::file_size(include, sizeEc);
            if (mtimeEc || sizeEc || !seen.insert(include).second) continue;
            deps.push_back({ include, (int64_t)mtime.time_since_epoch().count(), size });
        }
        return !deps.empty();
}


// Identity helper declared once per default-wrapper chunk; an incremental group remembers it.
inline const std::string kCxxWrapperPidDecl =
    "template <class T> struct __cflat_pid { typedef T type; };\n";


// Scoped wall-clock timer for one stage of a C++ header import. Prints under -v only.
struct CxxExtractionStageTimer
{
        bool enabled;
        std::string name;
        std::optional<size_t> count;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        CxxExtractionStageTimer(bool isEnabled, std::string stage,
                                std::optional<size_t> itemCount = std::nullopt)
            : enabled(isEnabled), name(std::move(stage)), count(itemCount) {}

        ~CxxExtractionStageTimer()
        {
            if (!enabled) return;
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            try  // verbose timing only; never terminate from a destructor
            {
                if (count.has_value())
                    std::cout << std::format(
                        "[verbose]   extraction stage {}: {:.3f} ms (count: {})\n",
                        name, ms, *count);
                else
                    std::cout << std::format("[verbose]   extraction stage {}: {:.3f} ms\n",
                                             name, ms);
            }
            catch (...) {}
        }
};


inline const std::string kNegativeCxxRequestMarker = "__cflat_negative_request";
// Negative-cache reason meaning "bare class template, every parameter defaulted: request `<>`".
inline const std::string kCxxAllDefaultTemplateRetry = "__cflat_all_default_template_retry";
inline const std::string kScalarCxxRequestMarker = "__cflat_scalar_typedef_request";
// A |CONST entry for a class-typed constant: the class's C++ spelling rides in `file`.
inline const std::string kClassConstantCxxRequestMarker = "__cflat_class_constant_request";
// A |CONST entry for an unscoped enumerator: its enum's C++ spelling in `file`, value in enums[0].
inline const std::string kEnumeratorCxxRequestMarker = "__cflat_enumerator_request";


// Rejection text the owning-group candidate loop recognizes; never shown to the user.
inline constexpr const char* kCxxOwnershipProbeRejected =
    "C++ ownership probe rejected this candidate";


inline void ScanHeaderNamespaceNames(std::string_view text,
                                     std::unordered_set<std::string>& out,
                                     std::unordered_set<std::string>* out2 = nullptr,
                                     std::unordered_set<std::string>* out3 = nullptr)
{
        auto identChar = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };
        static constexpr std::string_view kw = "namespace";
        // Anchored on the keyword's 'p' (index 5): memchr skips far more text than a search
        // keyed on its common leading 'n'. Finds the same occurrences as text.find(kw, pos).
        constexpr size_t anchor = 5;
        auto findKeyword = [&](size_t from) -> size_t {
            const char* base = text.data();
            const size_t size = text.size();
            for (size_t i = from + anchor; i < size;)
            {
                const void* hit = std::memchr(base + i, 'p', size - i);
                if (hit == nullptr) return std::string_view::npos;
                const size_t k = static_cast<const char*>(hit) - base;
                if (k + kw.size() - anchor <= size
                    && std::memcmp(base + k - anchor, kw.data(), kw.size()) == 0)
                    return k - anchor;
                i = k + 1;
            }
            return std::string_view::npos;
        };
        for (size_t pos = 0; (pos = findKeyword(pos)) != std::string_view::npos; pos += kw.size())
        {
            if (pos > 0 && identChar(text[pos - 1])) continue;
            size_t i = pos + kw.size();
            if (i < text.size() && identChar(text[i])) continue;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            const size_t start = i;
            while (i < text.size() && identChar(text[i])) ++i;
            if (i > start)
            {
                std::string name(text.substr(start, i - start));
                out.insert(name);
                if (out2 != nullptr) out2->insert(name);
                if (out3 != nullptr) out3->insert(std::move(name));
            }
        }
}

inline void CollectHeaderNamespaceNames(const std::string& path,
                                        std::unordered_set<std::string>& out,
                                        std::unordered_set<std::string>* out2 = nullptr,
                                        std::unordered_set<std::string>* out3 = nullptr)
{
        llvm::TimeTraceScope namespaceScope("CxxNamespaceFileScan", path);
        auto buffer = llvm::MemoryBuffer::getFile(path, /*IsText*/ false,
                                                  /*RequiresNullTerminator*/ false);
        if (!buffer) return;
        const llvm::StringRef text = (*buffer)->getBuffer();
        ScanHeaderNamespaceNames(std::string_view(text.data(), text.size()), out, out2, out3);
}

// File content hash; must equal LLVMBackend::HashFileFnv1a over the same bytes (xxh3, not FNV:
// a byte-loop FNV cost ~6 ms per compile on the 6 MB simdjson.h).

/*
 * CollectHeaderNamespaceNames over many headers: the scan only reads, so a few workers share
 * the files and their sets merge at the end - the union is the serial result. `buffers`, when
 * given, holds the bytes clang already read for each path (a null entry reads the file).
 * `hashes`, when given, receives the FNV-1a content hash of each path it does not already hold,
 * from those same bytes: the header cache's dependency stamps then need no second read.
 */
inline void CollectHeaderNamespaceNamesParallel(const std::vector<std::string>& paths,
                                                int64_t hashTime,
                                                std::unordered_set<std::string>& out,
                                                std::unordered_set<std::string>* out2 = nullptr,
                                                const std::vector<std::string_view>* buffers = nullptr,
                                                std::unordered_map<std::string, std::pair<uint64_t, int64_t>>* hashes = nullptr)
{
        llvm::TimeTraceScope namespaceScope("CxxNamespaceScan");
        const size_t count = paths.size();
        if (buffers != nullptr && buffers->size() != count) buffers = nullptr;
        // Each path is scanned (and hashed) once; a re-entered header adds no names.
        std::vector<size_t> work;
        std::vector<char> wantHash;
        {
            std::unordered_set<std::string_view> seen;
            work.reserve(count);
            wantHash.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                if (!seen.insert(paths[i]).second) continue;
                work.push_back(i);
                wantHash.push_back(hashes != nullptr && buffers != nullptr
                                   && (*buffers)[i].data() != nullptr
                                   && hashes->count(paths[i]) == 0);
            }
        }
        const unsigned workers = work.size() < 64 ? 1u : 4u;
        std::vector<std::unordered_set<std::string>> found(workers);
        std::vector<uint64_t> fileHashes(work.size(), 0);
        LLVMBackend::RunParallel(work.size(), workers, [&](size_t w, unsigned worker) {
            const size_t i = work[w];
            const std::string_view text = buffers != nullptr ? (*buffers)[i] : std::string_view();
            if (text.data() == nullptr)
            {
                CollectHeaderNamespaceNames(paths[i], found[worker]);
                return;
            }
            ScanHeaderNamespaceNames(text, found[worker]);
            if (wantHash[w])
            {
                fileHashes[w] = Fnv1aBytes(text);
            }
        });
        for (const auto& names : found)
            for (const auto& name : names)
            {
                out.insert(name);
                if (out2 != nullptr) out2->insert(name);
            }
        if (hashes != nullptr)
            for (size_t w = 0; w < work.size(); ++w)
                if (wantHash[w]) hashes->emplace(paths[work[w]],
                    std::pair<uint64_t, int64_t>{fileHashes[w], hashTime});
}


inline bool UseCxxIncrementalRequests()
{
        static const bool enabled = [] {
            const char* value = std::getenv("CFLAT_CPP_INCREMENTAL");
            if (value == nullptr) return true;
            const std::string_view setting(value);
            return setting != "0" && setting != "off" && setting != "false";
        }();
        return enabled;
}


inline bool CxxErrorsAreInsideWrapper(const std::string& diagnostics,
                                      const std::string& sourcePath,
                                      const std::string& source,
                                      const std::string& wrapperText)
{
        if (diagnostics.empty() || sourcePath.empty() || wrapperText.empty()) return false;
        const size_t start = source.rfind(wrapperText);
        if (start == std::string::npos) return false;
        const size_t firstLine = 1 + std::count(source.begin(), source.begin() + start, '\n');
        const size_t lastLine = firstLine + std::count(wrapperText.begin(), wrapperText.end(), '\n');
        const std::string expectedPath = std::filesystem::path(sourcePath).lexically_normal().string();
        bool foundError = false;
        size_t begin = 0;
        while (begin < diagnostics.size())
        {
            size_t end = diagnostics.find('\n', begin);
            if (end == std::string::npos) end = diagnostics.size();
            const std::string line = diagnostics.substr(begin, end - begin);
            begin = end + 1;
            size_t errorAt = line.find(": error:");
            const size_t fatalAt = line.find(": fatal error:");
            if (fatalAt != std::string::npos
                && (errorAt == std::string::npos || fatalAt < errorAt))
                errorAt = fatalAt;
            if (errorAt == std::string::npos) continue;
            const size_t colSep = line.rfind(':', errorAt - 1);
            const size_t rowSep = colSep == std::string::npos ? colSep : line.rfind(':', colSep - 1);
            if (rowSep == std::string::npos || colSep == std::string::npos) return false;
            const std::string path = std::filesystem::path(line.substr(0, rowSep))
                                         .lexically_normal().string();
            const std::string row = line.substr(rowSep + 1, colSep - rowSep - 1);
            if (path != expectedPath || row.empty()
                || !std::all_of(row.begin(), row.end(),
                                [](unsigned char c) { return std::isdigit(c); }))
                return false;
            const size_t lineNumber = std::stoull(row);
            if (lineNumber < firstLine || lineNumber > lastLine) return false;
            foundError = true;
        }
        return foundError;
}

