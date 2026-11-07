#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <bit>
#include <cctype>
#include <thread>
#include <fstream>
#include <iterator>
#include <llvm/Support/xxhash.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "CppStructName.h"
#include "CxxIncrementalGroup.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include "TypeMangling.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>
#include <chrono>
#include <fstream>
#include <map>
#include <unordered_map>
#include <set>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

#include "CxxSpelling.h"

std::string ClangClCppStandard(const std::string& standard)
{
        std::string value = standard;
        if (value.starts_with("gnu++")) value.replace(0, 5, "c++");
        if (value == "c++26") return "c++latest";
        return value;
}
bool IsSystemCxxHeaderPath(const std::string& path)
{
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (p.find("/c++/v1/") != std::string::npos) return true;   // libc++
        // MSVC STL: <VS>/VC/Tools/MSVC/<version>/include/<header>.
        if (p.find("/VC/Tools/MSVC/") != std::string::npos) return true;
        // libstdc++: /usr/include/c++/14/, /usr/include/<triple>/c++/14/. The component after
        // "/c++/" is the release, so require it to start with a digit - that is what separates a
        // standard-library header from a user directory that happens to be named "c++".
        for (size_t pos = 0; (pos = p.find("/c++/", pos)) != std::string::npos; pos += 5)
        {
            const size_t comp = pos + 5;
            if (comp < p.size() && std::isdigit((unsigned char)p[comp])) return true;
        }
        return false;
}
uint64_t Fnv1aBytes(std::string_view bytes)
{
        return llvm::xxh3_64bits(llvm::ArrayRef<uint8_t>(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}
std::string StripCxxRefAndCv(std::string_view spelling)
{
        std::string text(spelling);
        while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
        while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
        for (;;)
        {
            bool changed = false;
            while (!text.empty() && text.back() == '&')
            {
                text.pop_back();
                while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
                changed = true;
            }
            for (const char* prefix : { "const ", "volatile ", "class ", "struct " })
                if (text.starts_with(prefix))
                {
                    text.erase(0, std::strlen(prefix));
                    while (!text.empty() && std::isspace((unsigned char)text.front()))
                        text.erase(text.begin());
                    changed = true;
                    break;
                }
            if (!changed) break;
        }
        return text;
}
uint64_t HashWrapperKey(std::string_view key)
{
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : key)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash;
}
/*
 * Erase every occurrence of a declarator keyword, matched as a WHOLE TOKEN. A plain find/erase
 * loop also eats the keyword out of an identifier - "constant" becomes "ant", "classBase"
 * becomes "Base" - which loses the type's identity and silently decays the parameter to void*.
 * A word that does not start/end with an identifier character (e.g. "&") has no boundary to
 * check on that side, so it is erased wherever it appears.
 */
void EraseDeclaratorToken(std::string& s, const char* word)
{
        const size_t n = std::strlen(word);
        if (n == 0) return;
        auto ident = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };
        const bool headIdent = ident(word[0]);
        const bool tailIdent = ident(word[n - 1]);
        for (size_t pos = 0; (pos = s.find(word, pos)) != std::string::npos; )
        {
            const bool okBefore = !headIdent || pos == 0 || !ident(s[pos - 1]);
            const bool okAfter  = !tailIdent || pos + n >= s.size() || !ident(s[pos + n]);
            if (okBefore && okAfter) s.erase(pos, n);
            else                     pos += n;
        }
    }
bool CxxSpellingIsBareNullptrT(std::string spelled)
{
        std::erase_if(spelled, [](unsigned char c) { return std::isspace(c) != 0; });
        for (bool again = true; again;)
        {
            again = false;
            for (std::string_view w : { "const", "volatile" })
            {
                if (spelled.starts_with(w)) { spelled.erase(0, w.size()); again = true; }
                if (spelled.ends_with(w)) { spelled.resize(spelled.size() - w.size()); again = true; }
            }
        }
        if (spelled.starts_with("::")) spelled.erase(0, 2);
        return spelled == "std::nullptr_t" || spelled == "std::__1::nullptr_t"
            || spelled == "nullptr_t" || spelled == "decltype(nullptr)";
}
bool CxxSpellingHasTopLevelParen(const std::string& spelling)
{
        int angleDepth = 0;
        int parenDepth = 0;
        for (char c : spelling)
        {
            if (c == '(')
            {
                if (angleDepth == 0 && parenDepth == 0) return true;
                ++parenDepth;
            }
            else if (c == ')' && parenDepth > 0) --parenDepth;
            else if (parenDepth == 0 && c == '<') ++angleDepth;
            else if (parenDepth == 0 && c == '>' && angleDepth > 0) --angleDepth;
        }
        return false;
}
bool IsCxxClosureReferenceResult(std::string spelling)
{
        while (spelling.starts_with("const ") || spelling.starts_with("volatile "))
            spelling.erase(0, spelling.find(' ') + 1);
        while (!spelling.empty() && std::isspace((unsigned char)spelling.back())) spelling.pop_back();
        return spelling.starts_with("__cflat_closure<")
            && (spelling.ends_with('&') || spelling.ends_with('*'));
}
std::string CxxClosureReferenceResultRefusal(const std::string& callee)
{
        return std::format("C++ '{}' returns a reference to the CFlat closure it was given; CFlat cannot "
                           "use that reference as a value - call the closure inside C++ or return it "
                           "by value", callee);
}
bool ParseCxxArrayParameter(const std::string& spelling, std::string& element,
                                   uint64_t& extent)
{
        const size_t lb = spelling.rfind('[');
        const size_t rb = spelling.find(']', lb == std::string::npos ? 0 : lb);
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) return false;
        std::string digits = spelling.substr(lb + 1, rb - lb - 1);
        extent = 0;
        for (char c : digits)
        {
            if (c < '0' || c > '9') return false;
            extent = extent * 10 + (uint64_t)(c - '0');
        }
        if (extent == 0) return false;
        element = spelling.substr(0, lb);
        while (!element.empty() && std::isspace((unsigned char)element.back())) element.pop_back();
        if (!element.empty() && element.back() == ')') element.pop_back();
        const size_t open = element.rfind('(');
        if (open != std::string::npos) element.erase(open);
        element.erase(std::remove(element.begin(), element.end(), '&'), element.end());
        element.erase(std::remove(element.begin(), element.end(), '*'), element.end());
        while (!element.empty() && std::isspace((unsigned char)element.back())) element.pop_back();
        return !element.empty();
}
/*
 * True when a C++ parameter spelling is an LVALUE reference to a const NON-POINTER type
 * ("const int &", "const double &"). CFlat carries no const qualifier of its own, so this is the
 * only record that an RVALUE may bind the parameter by materializing a temporary. An rvalue
 * reference and a reference to a pointer ("const char *&") are excluded - both keep their own
 * shape and their own lowering.
 */
bool CxxParamIsConstLvalueReference(const std::string& spelling)
{
        std::string s = spelling;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        int refs = 0;
        while (!s.empty() && s.back() == '&') { s.pop_back(); ++refs; }
        if (refs != 1) return false;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        // `int *const` qualifies the POINTER, not the referent, so a trailing '*' is not one.
        if (s.empty() || s.back() == '*') return false;
        if (s.rfind("const ", 0) == 0)
            return true;
        // East const ("int const &"); a pointer spelling is a reference to pointer instead.
        return s.find('*') == std::string::npos && s.size() >= 5
            && s.compare(s.size() - 5, 5, "const") == 0
            && (s.size() == 5 || !std::isalnum((unsigned char)s[s.size() - 6]));
}
bool CxxRvalueReferenceIsConst(const std::string& spelling)
{
        std::string lvalueSpelling = spelling;
        const size_t ref = lvalueSpelling.rfind("&&");
        if (ref == std::string::npos) return false;
        lvalueSpelling.replace(ref, 2, "&");
        return CxxParamIsConstLvalueReference(lvalueSpelling);
}
bool CxxSpellingHasVolatileTarget(const std::string& spelling)
{
        if (spelling.find('(') != std::string::npos) return false;
        std::string head;
        int depth = 0;
        for (char c : spelling)
        {
            if (c == '<') ++depth;
            else if (c == '>') --depth;
            else if (depth == 0 && (c == '*' || c == '&')) break;
            else if (depth == 0) head += c;
        }
        for (size_t at = head.find("volatile"); at != std::string::npos;
             at = head.find("volatile", at + 8))
        {
            const bool leftOk = at == 0 || !(std::isalnum((unsigned char)head[at - 1])
                                             || head[at - 1] == '_' || head[at - 1] == ':');
            const size_t end = at + 8;
            const bool rightOk = end >= head.size()
                || !(std::isalnum((unsigned char)head[end]) || head[end] == '_');
            if (leftOk && rightOk) return true;
        }
        return false;
}
bool CxxParamIsConstRefToPointer(const std::string& spelling)
{
        std::string s = spelling;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        int refs = 0;
        while (!s.empty() && s.back() == '&') { s.pop_back(); ++refs; }
        if (refs != 1) return false;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (s.size() < 5 || s.compare(s.size() - 5, 5, "const") != 0) return false;
        s.erase(s.size() - 5);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return !s.empty() && s.back() == '*';
}
/*
 * Pointee const levels of a C pointer spelling, bottom-relative like ConstMask (bit 0 = the
 * innermost pointee): `const int **` -> 1, `const int *const *` -> 3. The parameter's own
 * top-level const is not part of it. 0 when the spelling is not `depth` plain stars.
 */
uint32_t CSpellingPointeeConstMask(const std::string& spelling, int depth)
{
        if (spelling.find_first_of("()[<&") != std::string::npos || depth <= 0) return 0;
        std::vector<std::string> segments(1);
        for (char c : spelling)
        {
            if (c == '*') segments.emplace_back();
            else segments.back() += c;
        }
        if ((int)segments.size() - 1 != depth) return 0;
        auto hasConst = [](const std::string& seg) {
            for (size_t at = seg.find("const"); at != std::string::npos; at = seg.find("const", at + 1))
            {
                const bool leftOk = at == 0 || !(std::isalnum((unsigned char)seg[at - 1]) || seg[at - 1] == '_');
                const size_t end = at + 5;
                const bool rightOk = end >= seg.size() || !(std::isalnum((unsigned char)seg[end]) || seg[end] == '_');
                if (leftOk && rightOk) return true;
            }
            return false;
        };
        uint32_t mask = 0;
        for (int k = 0; k < depth; ++k)
            if (k < 32 && hasConst(segments[k])) mask |= uint32_t{ 1 } << k;
        return mask;
}
bool CxxSpellingHasConstPointee(const std::string& spelling)
{
        if (spelling.find('(') != std::string::npos) return false;
        std::string head;
        int depth = 0;
        bool sawStar = false;
        for (char c : spelling)
        {
            if (c == '<') ++depth;
            else if (c == '>') --depth;
            else if (depth == 0 && c == '*') { sawStar = true; break; }
            else if (depth == 0) head += c;
        }
        if (!sawStar) return false;
        for (size_t at = head.find("const"); at != std::string::npos;
             at = head.find("const", at + 1))
        {
            const bool leftOk = at == 0 || !(std::isalnum((unsigned char)head[at - 1])
                                             || head[at - 1] == '_' || head[at - 1] == ':');
            const size_t end = at + 5;
            const bool rightOk = end >= head.size()
                || !(std::isalnum((unsigned char)head[end]) || head[end] == '_');
            if (leftOk && rightOk) return true;
        }
        return false;
}
/*
 * Pointee const levels of a C++ pointer spelling, bottom-relative like ConstMask: the C helper's
 * levels once a trailing reference is dropped, and the innermost pointee from the template-aware
 * head scan the C helper does not attempt (`const std::vector<int> *`).
 */
uint32_t CxxSpellingPointeeConstMask(const std::string& spelling, int depth)
{
        std::string s = spelling;
        while (!s.empty() && (s.back() == ' ' || s.back() == '&')) s.pop_back();
        uint32_t mask = CSpellingPointeeConstMask(s, depth);
        if (depth > 0 && CxxSpellingHasConstPointee(spelling)) mask |= 1u;
        return mask;
}
bool CxxSpellingHasTopLevelConst(const std::string& spelling)
{
        int templateDepth = 0;
        for (size_t i = 0; i < spelling.size(); ++i)
        {
            if (spelling[i] == '<') { ++templateDepth; continue; }
            if (spelling[i] == '>') { --templateDepth; continue; }
            if (templateDepth != 0 || i + 5 > spelling.size()
                || spelling.compare(i, 5, "const") != 0)
                continue;
            const bool leftOk = i == 0 || !(std::isalnum((unsigned char)spelling[i - 1])
                                             || spelling[i - 1] == '_' || spelling[i - 1] == ':');
            const size_t end = i + 5;
            const bool rightOk = end == spelling.size()
                || !(std::isalnum((unsigned char)spelling[end]) || spelling[end] == '_');
            if (leftOk && rightOk) return true;
        }
        return false;
}
bool CxxSpellingHasTopLevelObjectConst(const std::string& spelling)
{
        size_t lastStar = std::string::npos;
        int templateDepth = 0;
        for (size_t i = 0; i < spelling.size(); ++i)
        {
            if (spelling[i] == '<') ++templateDepth;
            else if (spelling[i] == '>' && templateDepth > 0) --templateDepth;
            else if (templateDepth == 0 && spelling[i] == '*') lastStar = i;
        }
        if (lastStar == std::string::npos) return CxxSpellingHasTopLevelConst(spelling);
        const std::string suffix = spelling.substr(lastStar + 1);
        for (size_t at = suffix.find("const"); at != std::string::npos;
             at = suffix.find("const", at + 5))
        {
            const bool leftOk = at == 0 || !(std::isalnum((unsigned char)suffix[at - 1])
                                             || suffix[at - 1] == '_');
            const size_t end = at + 5;
            const bool rightOk = end >= suffix.size()
                || !(std::isalnum((unsigned char)suffix[end]) || suffix[end] == '_');
            if (leftOk && rightOk) return true;
        }
        return false;
}
std::string CxxMemberValueSpelling(const std::string& spelling)
{
        std::string out = spelling;
        while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
        for (;;)
        {
            bool changed = false;
            while (!out.empty() && (out.back() == '*' || out.back() == '&'))
            {
                out.pop_back();
                while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
                changed = true;
            }
            for (const char* q : { "const ", "volatile ", "class ", "struct ", "union " })
                if (out.starts_with(q))
                {
                    out.erase(0, std::strlen(q));
                    changed = true;
                    break;
                }
            if (!changed) break;
        }
        return out;
}
std::string CxxFieldIdentitySpelling(const std::string& spelling)
{
        const std::string value = CxxMemberValueSpelling(spelling);
        const size_t open = value.find('<');
        const size_t close = value.rfind('>');
        if (open == std::string::npos || close == std::string::npos || close < open) return value;

        std::vector<std::string> args;
        size_t start = open + 1;
        int depth = 0;
        for (size_t i = start; i <= close; ++i)
        {
            const char ch = i == close ? ',' : value[i];
            if (ch == '<') ++depth;
            else if (ch == '>') --depth;
            else if (ch == ',' && depth == 0)
            {
                std::string arg = value.substr(start, i - start);
                const size_t first = arg.find_first_not_of(" \t");
                const size_t last = arg.find_last_not_of(" \t");
                if (first != std::string::npos) arg = arg.substr(first, last - first + 1);
                args.push_back(CxxFieldIdentitySpelling(arg));
                start = i + 1;
            }
        }
        if (args.empty()) return value;

        const std::string base = value.substr(0, open);
        auto isDefaultArg = [&](size_t index) -> std::string
        {
            if (args.empty()) return {};
            const std::string& first = args[0];
            const std::string second = args.size() > 1 ? args[1] : std::string();
            if (base == "std::vector" && index == 1)
                return "std::allocator<" + first + ">";
            if (base == "std::unique_ptr" && index == 1)
                return "std::default_delete<" + first + ">";
            if (base == "std::basic_string")
            {
                if (index == 1) return "std::char_traits<" + first + ">";
                if (index == 2) return "std::allocator<" + first + ">";
            }
            if (base == "std::map" || base == "std::multimap")
            {
                if (index == 2) return "std::less<" + first + ">";
                if (index == 3)
                    return "std::allocator<std::pair<const " + first + ", " + second + ">>";
            }
            if (base == "std::unordered_map" || base == "std::unordered_multimap")
            {
                if (index == 2) return "std::hash<" + first + ">";
                if (index == 3) return "std::equal_to<" + first + ">";
                if (index == 4)
                    return "std::allocator<std::pair<const " + first + ", " + second + ">>";
            }
            if (base == "std::set" || base == "std::multiset")
            {
                if (index == 1) return "std::less<" + first + ">";
                if (index == 2) return "std::allocator<" + first + ">";
            }
            if (base == "std::unordered_set" || base == "std::unordered_multiset")
            {
                if (index == 1) return "std::hash<" + first + ">";
                if (index == 2) return "std::equal_to<" + first + ">";
                if (index == 3) return "std::allocator<" + first + ">";
            }
            return {};
        };
        std::vector<bool> defaults(args.size(), false);
        for (size_t i = 1; i < args.size(); ++i)
        {
            const std::string expected = isDefaultArg(i);
            defaults[i] = !expected.empty() && args[i] == CxxFieldIdentitySpelling(expected);
        }

        size_t identityArgCount = args.size();
        while (identityArgCount > 1 && defaults[identityArgCount - 1])
            --identityArgCount;
        std::string result = base + "<";
        for (size_t i = 0; i < identityArgCount; ++i)
        {
            if (i != 0) result += ", ";
            result += args[i];
        }
        result += ">";
        return result;
}
std::string CxxDefaultWrapperName(const std::string& linkageName, size_t omittedArity)
{
        std::string out = "__cflat_dflt_";
        for (char c : linkageName)
        {
            const unsigned char byte = (unsigned char)c;
            if (std::isalnum(byte)) out += (char)byte;
            else
            {
                constexpr char hex[] = "0123456789ABCDEF";
                out += '_';
                out += hex[byte >> 4];
                out += hex[byte & 0x0F];
            }
        }
        out += "_" + std::to_string(omittedArity);
        return out;
}
bool HasNonConstDefaultSuffix(const std::vector<cflat_cinterop::RawDefaultArg>& defaults,
                                     size_t first)
{
        if (first >= defaults.size()) return false;
        for (size_t i = first; i < defaults.size(); ++i)
        {
            if (defaults[i].kind.empty()) return false;
            if (defaults[i].kind == "unsupported") continue;
            if (defaults[i].kind == "nonconst") return true;
        }
        return false;
}
std::string CxxNameFromCflat(const std::string& name)
{
        std::string out = name;
        for (size_t pos = 0; (pos = out.find('.', pos)) != std::string::npos; pos += 2)
            out.replace(pos, 1, "::");
        return "::" + out;
}
void PrependCv(std::string& spelling, std::string_view cv)
{
        if (!spelling.starts_with(cv)) spelling.insert(0, cv);
}
std::vector<std::string> CxxRequestIncludeRoots(const std::vector<std::string>& headers)
{
    std::vector<std::string> roots{ "<new>" };
    roots.insert(roots.end(), headers.begin(), headers.end());
    return roots;
}
std::string FirstCxxErrorLine(const std::string& text)
{
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) return line;
            start = end == text.size() ? end : end + 1;
        }
        return text;
}
std::string TrimCxxBraceType(std::string text)
{
        while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
        while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
        for (;;)
        {
            bool changed = false;
            for (const char* prefix : { "const ", "volatile ", "class ", "struct " })
                if (text.starts_with(prefix))
                {
                    text.erase(0, std::strlen(prefix));
                    changed = true;
                    break;
                }
            if (!changed) break;
        }
        return text;
}
std::string CxxBraceTargetElementSpelling(const std::string& parameter)
{
        std::string type = StripCxxRefAndCv(parameter);
        while (!type.empty() && type.back() == '*')
        {
            type.pop_back();
            while (!type.empty() && std::isspace((unsigned char)type.back())) type.pop_back();
        }
        const size_t open = type.find('<');
        if (open == std::string::npos) return {};
        const std::string base = type.substr(0, open);
        if (base.find("initializer_list") == std::string::npos
            && base.find("ArrayRef") == std::string::npos
            && base.find("IListRef") == std::string::npos
            && base.find("vector") == std::string::npos)
            return {};
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; i < type.size(); ++i)
        {
            if (type[i] == '<') ++depth;
            else if (type[i] == '>' && --depth == 0) { close = i; break; }
        }
        if (close == std::string::npos || close != type.size() - 1) return {};
        const std::string templateArgs = type.substr(open + 1, close - open - 1);
        size_t elementEnd = templateArgs.size();
        int nestedDepth = 0;
        for (size_t i = 0; i < templateArgs.size(); ++i)
        {
            if (templateArgs[i] == '<') ++nestedDepth;
            else if (templateArgs[i] == '>') --nestedDepth;
            else if (templateArgs[i] == ',' && nestedDepth == 0)
            {
                elementEnd = i;
                break;
            }
        }
        std::string element = TrimCxxBraceType(templateArgs.substr(0, elementEnd));
        if (element.empty() || element.find('<') != std::string::npos
            || element.find('>') != std::string::npos)
            return {};
        return element;
}
bool CxxBraceParameterIsPointer(const std::string& parameter)
{
        std::string type = StripCxxRefAndCv(parameter);
        return !type.empty() && type.back() == '*';
}
CxxBraceContainerKind CxxBraceContainerKindOf(const std::string& parameter)
{
        if (CxxBraceParameterIsPointer(parameter)) return CxxBraceContainerKind::Unsupported;
        std::string type = StripCxxRefAndCv(parameter);
        while (!type.empty() && type.back() == '*')
        {
            type.pop_back();
            while (!type.empty() && std::isspace((unsigned char)type.back())) type.pop_back();
        }
        const size_t open = type.find('<');
        if (open == std::string::npos)
            return type.find("TensorList") != std::string::npos
                ? CxxBraceContainerKind::ArrayRef : CxxBraceContainerKind::Unsupported;
        const std::string base = type.substr(0, open);
        if (base.find("initializer_list") != std::string::npos)
            return CxxBraceContainerKind::InitializerList;
        if (base.find("ArrayRef") != std::string::npos || base.find("IListRef") != std::string::npos)
            return CxxBraceContainerKind::ArrayRef;
        if (base.find("vector") != std::string::npos) return CxxBraceContainerKind::Vector;
        return CxxBraceContainerKind::PointerPairClass;
}
bool CxxArrayReferenceParameter(const std::string& parameter, std::string& element,
                                       std::string& extent)
{
        size_t ref = parameter.find("(&)[");
        size_t open = ref == std::string::npos ? ref : ref + 3;
        if (ref == std::string::npos)
        {
            ref = parameter.find("(&&)[");
            open = ref == std::string::npos ? ref : ref + 4;
        }
        if (ref == std::string::npos) return false;
        const size_t close = parameter.find(']', open);
        if (close == std::string::npos || close + 1 != parameter.size()) return false;
        extent = parameter.substr(open + 1, close - open - 1);
        element = TrimCxxBraceType(parameter.substr(0, ref));
        if (element.ends_with(" const")) element.resize(element.size() - 6);
        return !element.empty() && element.find_first_of("&*[(<") == std::string::npos;
}
std::string CxxInitializerListOverStorage(const std::string& listType, const std::string& il,
                                                 const std::string& p, size_t count)
{
        const std::string n = std::to_string(count);
        return "\n" + listType + " " + il + ";\n#if defined(_MSVC_STL_VERSION)\nconst void* " + il
            + "_words[2] = { " + p + ", " + p + " + " + n
            + " };\n#elif defined(_LIBCPP_VERSION) || defined(__GLIBCXX__)\n"
            "struct { const void* b; decltype(sizeof 0) n; } " + il + "_words{ "
            + p + ", " + n + " };\n#else\n#error \"cflat: std::initializer_list "
            "layout of this standard library is unknown\"\n#endif\n"
            "static_assert(sizeof(" + il + ") == sizeof(" + il + "_words));\n"
            "__builtin_memcpy((void*)&" + il + ", &" + il + "_words, sizeof "
            + il + ");\n";
}
bool CxxBraceElementsAllScalar(const LLVMBackend::CxxBraceArgument& brace)
{
        for (const auto& source : brace.elements)
        {
            llvm::Type* type = source.Primary != nullptr ? source.Primary->getType() : nullptr;
            if (source.TypeAndValue.Pointer || type == nullptr
                || (!type->isIntegerTy() && !type->isFloatingPointTy()))
                return false;
        }
        return true;
}
std::string CxxZeroLiteralSpelling(const std::string& identity)
{
        static const std::map<std::string, std::string> kZeroLiterals = {
            { "int", "0" }, { "u32", "0u" }, { "long", "0L" }, { "ulong", "0UL" },
            { "i64", "0LL" }, { "u64", "0ULL" } };
        const auto it = kZeroLiterals.find(identity);
        return it != kZeroLiterals.end() ? it->second : std::string();
}
std::string CxxFunctionPointerReferentParameter(const std::string& parameter)
{
        for (std::string marker : { std::string("(*&&)"), std::string("(*const &&)"),
                                    std::string("(*volatile &&)"),
                                    std::string("(*const volatile &&)"),
                                    std::string("(*&)"), std::string("(*const &)"),
                                    std::string("(*volatile &)"),
                                    std::string("(*const volatile &)") })
            if (size_t at = parameter.find(marker); at != std::string::npos)
                return parameter.substr(0, at) + "(*)" + parameter.substr(at + marker.size());
        return {};
}
std::string CxxTypeofInner(const std::string& spelling)
{
        constexpr std::string_view prefix = "__typeof__(";
        if (!spelling.starts_with(prefix) || !spelling.ends_with(")")) return {};
        return spelling.substr(prefix.size(), spelling.size() - prefix.size() - 1);
}
bool IsCxxConstRefToPointerSpelling(std::string type)
{
        while (!type.empty() && type.back() == ' ') type.pop_back();
        if (!type.ends_with("&") || type.ends_with("&&")) return false;
        type.pop_back();
        while (!type.empty() && type.back() == ' ') type.pop_back();
        if (!type.ends_with("const")) return false;
        type.resize(type.size() - 5);
        while (!type.empty() && type.back() == ' ') type.pop_back();
        return type.ends_with("*");
}
bool IsCxxArrayReferenceSpelling(const std::string& type)
{
        return type.find("(&)[") != std::string::npos || type.find("(&&)[") != std::string::npos;
}
bool CxxBraceFieldIsReference(const std::string& spelling)
{
        const size_t last = spelling.find_last_not_of(" \t\r\n");
        if (last != std::string::npos && spelling[last] == '&') return true;

        for (size_t open = 0; open < spelling.size(); ++open)
        {
            if (spelling[open] != '(') continue;
            int depth = 1;
            size_t close = open + 1;
            for (; close < spelling.size() && depth != 0; ++close)
            {
                if (spelling[close] == '(') ++depth;
                else if (spelling[close] == ')') --depth;
            }
            if (depth != 0) break;
            --close;
            const size_t suffix = close + 1;
            if (suffix < spelling.size()
                && (spelling[suffix] == '(' || spelling[suffix] == '[')
                && spelling.find('&', open + 1) < close)
                return true;
            open = close;
        }
        return false;
}
bool CxxBraceFieldIsFunctionReference(const std::string& spelling)
{
        for (size_t open = 0; open < spelling.size(); ++open)
        {
            if (spelling[open] != '(') continue;
            int depth = 1;
            size_t close = open + 1;
            for (; close < spelling.size() && depth != 0; ++close)
            {
                if (spelling[close] == '(') ++depth;
                else if (spelling[close] == ')') --depth;
            }
            if (depth != 0) break;
            --close;
            const size_t suffix = close + 1;
            if (suffix < spelling.size() && spelling[suffix] == '(')
            {
                const std::string_view declarator(spelling.data() + open + 1, close - open - 1);
                return declarator.find('&') != std::string_view::npos
                    && declarator.find('*') == std::string_view::npos;
            }
            open = close;
        }
        return false;
}
bool CxxBraceFieldIsRvalueReference(const std::string& spelling)
{
        const size_t last = spelling.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) return false;
        if (spelling[last] == '&' && last > 0)
        {
            const size_t previous = spelling.find_last_not_of(" \t\r\n", last - 1);
            if (previous != std::string::npos && spelling[previous] == '&') return true;
        }
        for (size_t open = 0; open < spelling.size(); ++open)
        {
            if (spelling[open] != '(') continue;
            int depth = 1;
            size_t close = open + 1;
            for (; close < spelling.size() && depth != 0; ++close)
            {
                if (spelling[close] == '(') ++depth;
                else if (spelling[close] == ')') --depth;
            }
            if (depth != 0) break;
            --close;
            const size_t suffix = close + 1;
            if (suffix < spelling.size()
                && (spelling[suffix] == '(' || spelling[suffix] == '['))
            {
                const size_t reference = spelling.find("&&", open + 1);
                if (reference != std::string::npos && reference + 1 < close) return true;
            }
            open = close;
        }
        return false;
}
std::string CxxNamedParameter(const std::string& spelling,
                                     const std::string& parameterName)
{
        for (size_t open = 0; open < spelling.size(); ++open)
        {
            if (spelling[open] != '(') continue;
            int depth = 1;
            size_t close = open + 1;
            for (; close < spelling.size() && depth != 0; ++close)
            {
                if (spelling[close] == '(') ++depth;
                else if (spelling[close] == ')') --depth;
            }
            if (depth != 0) break;
            --close;
            const size_t suffix = close + 1;
            if (suffix < spelling.size()
                && (spelling[suffix] == '(' || spelling[suffix] == '['))
            {
                const size_t reference = spelling.rfind('&', close);
                if (reference != std::string::npos && reference > open)
                    return spelling.substr(0, reference + 1) + " " + parameterName
                        + spelling.substr(reference + 1);
            }
            open = close;
        }
        return spelling + " " + parameterName;
}
std::string NormalizeCxxOwnershipPath(const std::string& value)
{
        if (value.empty()) return {};
        std::filesystem::path path(value);
        std::error_code ec;
        path = std::filesystem::absolute(path, ec);
        if (ec) path = std::filesystem::path(value);
        std::string normalized = path.lexically_normal().generic_string();
#if defined(_WIN32)
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
        return normalized;
}
std::string CxxSpellingWithoutRef(const std::string& spelling,
                                         CxxReferenceKind* kind)
{
        std::string s = spelling;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        int refs = 0;
        while (!s.empty() && s.back() == '&') { s.pop_back(); ++refs; }
        if (kind != nullptr) *kind = CxxReferenceKind::None;
        if (refs == 0) return spelling;
        static const char* const cvWords[] = { "const", "volatile", "restrict", "__restrict",
                                               "__restrict__", "_Nonnull", "_Nullable",
                                               "_Null_unspecified" };
        for (bool stripped = true; stripped; )
        {
            stripped = false;
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            for (const char* word : cvWords)
            {
                const size_t n = std::strlen(word);
                if (s.size() < n || s.compare(s.size() - n, n, word) != 0) continue;
                if (s.size() > n
                    && (std::isalnum((unsigned char)s[s.size() - n - 1])
                        || s[s.size() - n - 1] == '_'))
                    continue;
                s.erase(s.size() - n);
                stripped = true;
                break;
            }
        }
        if (kind != nullptr)
            *kind = refs > 1 ? CxxReferenceKind::Rvalue
                             : (!s.empty() && s.back() == '*'
                                ? CxxReferenceKind::RefToPointer : CxxReferenceKind::Lvalue);
        return s;
    }
bool IsPlainCFlatIdentifier(const std::string& name)
{
        antlr4::ANTLRInputStream input(name);
        CFlatLexer lexer(&input);
        lexer.removeErrorListeners();
        const auto tokens = lexer.getAllTokens();
        return tokens.size() == 1 && tokens.front()->getType() == CFlatLexer::Identifier;
}
bool HasAdjacentIdentifiers(const std::string& text)
{
        std::string previous;
        for (size_t i = 0; i < text.size(); )
        {
            const unsigned char c = (unsigned char)text[i];
            if (std::isspace(c)) { ++i; continue; }
            if (std::isalpha(c) || c == '_')
            {
                size_t end = i;
                while (end < text.size() && (std::isalnum((unsigned char)text[end]) || text[end] == '_')) ++end;
                std::string word = text.substr(i, end - i);
                if (!previous.empty() && previous != "sizeof") return true;
                previous = std::move(word);
                i = end;
                continue;
            }
            if (c == '"' || c == '\'')
            {
                const char quote = text[i];
                for (++i; i < text.size() && text[i] != quote; ++i)
                    if (text[i] == '\\') ++i;
                ++i;
            }
            else if (std::isdigit(c))
            {
                while (i < text.size() && (std::isalnum((unsigned char)text[i]) || text[i] == '.')) ++i;
            }
            else ++i;
            previous.clear();
        }
        return false;
}
/*
 * Whether a conditional arm's referent (or object) is const, read from its C++ shape.
 */
bool CxxTernaryArmConst(const LLVMBackend::TypeAndValue& arm)
{
        // A call result returned `const T` by value is a const prvalue, not a const object.
        return arm.IsCxxRef() ? arm.CxxConstRef() : arm.IsConstObject() || arm.IsCxxConstValueReturn;
}
/*
 * clang's verdict on `c ? A : B` for two C++ class arms. The arms are spelled with their exact cv
 * and category: an lvalue `*p`, an xvalue `static_cast<T &&>(*p)`, or for a prvalue arm a call to a
 * declared-only function (only decltype sees it, so no copy is modelled). Each probe is a wrapper
 * request; a failing one is a clang answer. Ill-formed and ambiguous conditionals are refusals.
 */
const char* CxxArmCategoryTag(LLVMBackend::CxxArmCategory category)
{
        using Category = LLVMBackend::CxxArmCategory;
        return category == Category::Prvalue ? "prvalue"
            : category == Category::Xvalue ? "xvalue" : "lvalue";
}

void MarkCxxDeclaredConstField(LLVMBackend::TypeAndValue& tv, bool isConst)
{
        if (!isConst) return;
        const int depth = tv.IsArrayView ? tv.ElementPointerDepth() : tv.ValuePointerDepth();
        if (depth < 32) tv.ConstMask |= uint32_t{ 1 } << depth;
}
bool CxxIncrementalSpellingSafe(const std::string& spelling)
{
        (void)spelling;
        return true;
}
std::string DirectBraceConstantText(const LLVMBackend::NamedVariable& element,
                                           const std::string& spelling, bool nullConstant)
{
        if (element.TypeAndValue.Pointer || element.Primary == nullptr) return {};
        if (nullConstant && llvm::isa_and_nonnull<llvm::ConstantInt>(element.Primary)
            && llvm::cast<llvm::ConstantInt>(element.Primary)->isZero())
            if (std::string zero = CxxZeroLiteralSpelling(element.LiteralIdentity); !zero.empty())
                return zero;
        if (auto* constantInt = llvm::dyn_cast<llvm::ConstantInt>(element.Primary))
        {
            const unsigned bits = constantInt->getBitWidth();
            if (bits > 64) return {};
            const bool isUnsigned = element.TypeAndValue.IsUnsignedInteger() != -1 || bits == 1;
            const std::string value = isUnsigned
                ? std::to_string(constantInt->getZExtValue()) + "ULL"
                : (bits == 64 && constantInt->isMinValue(true))
                    ? std::string("(-9223372036854775807LL - 1)")
                    : std::to_string(constantInt->getSExtValue()) + "LL";
            return "static_cast<" + spelling + ">(" + value + ")";
        }
        if (auto* constantFp = llvm::dyn_cast<llvm::ConstantFP>(element.Primary))
        {
            llvm::APFloat value = constantFp->getValueAPF();
            if (!value.isFinite()) return {};
            const llvm::Type* type = constantFp->getType();
            if (!type->isFloatTy() && !type->isDoubleTy()) return {};
            bool lossy = false;
            value.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &lossy);
            return "static_cast<" + spelling + ">(" + std::format("{:a}", value.convertToDouble())
                .insert(value.isNegative() ? 1 : 0, "0x") + ")";
        }
        return {};
}

std::string CxxRequestOdrUsePreamble()
{
        std::string src;
        src += "template <typename T> static auto __cflat_use_value_members_impl(T* p, int) -> "
               "decltype((void)p->value_or(typename T::value_type{}), "
               "(void)p->emplace(typename T::value_type{}), void()) { ";
        src += "(void)p->value_or(typename T::value_type{}); "
               "(void)p->emplace(typename T::value_type{}); }\n";
        src += "template <typename T> static void __cflat_use_value_members_impl(T*, long) {}\n";
        src += "template <typename T> static auto __cflat_use_optional_members_impl(T* p, int) -> "
               "decltype((void)p->has_value(), (void)p->reset(), (void)p->value(), void()) { ";
        src += "(void)p->has_value(); p->reset(); (void)p->value(); }\n";
        src += "template <typename T> static void __cflat_use_optional_members_impl(T*, long) {}\n";
        return src;
}

std::string BuildStdFunctionCtorUse(const std::string& cxxSpelling,
                                           const std::string& marker)
{
        std::string ret;
        std::string params;
        if (!cflat_cinterop::SplitStdFunctionSpelling(cxxSpelling, ret, params)) return {};
        std::string helper = "__cflat_std_function_ctor_";
        for (char c : cxxSpelling)
            helper += std::isalnum((unsigned char)c) ? c : '_';
        // Returns 'm': the extractor declares this helper 'void*' with returnsThis
        // (CClangExtract.cpp), so the emitted definition must carry the same type.
        return "extern \"C\" __attribute__((weak)) void* " + helper + "(void* m, " + ret
             + " (*a1)(" + params + ")) { (void)::new (m) " + marker + "(a1); return m; }\n";
}
