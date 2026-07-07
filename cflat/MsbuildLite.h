// MsbuildLite.h - lightweight evaluator for the MSBuild .props/.targets files that
// native NuGet packages ship under build/native/. Extracts include dirs, .lib paths,
// and copy-to-output DLLs without a real MSBuild engine.
//
// Scope (see internal/plan/package-nuget-import.md, "MSBuild .props/.targets
// lightweight evaluation"): PropertyGroup definitions with $(Prop) expansion,
// ItemGroup items + metadata, and a small Condition evaluator ('=='/'!='/Exists()/
// And/Or/parentheses). <Target> elements are skipped entirely. Unknown constructs
// evaluate as false and are reported via verbose diagnostics, never as hard errors.
//
// Header-only, pure STL (no LLVM/ANTLR deps) so it stays unit-testable standalone.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// No global `namespace fs` alias here: this header is transitively included into
// LLVMBackend.h, so a translation-unit-wide alias would leak. Each function that
// needs it declares a local `namespace fs = std::filesystem;` instead.

class MsbuildLite
{
public:
    struct EvalResult
    {
        std::vector<std::string> includeDirs;  // absolute include roots (AdditionalIncludeDirectories)
        std::vector<std::string> libs;         // absolute .lib paths (AdditionalDependencies / link items)
        std::vector<std::string> dlls;         // absolute copy-to-output files (.dll etc.)
    };

    // Evaluate one .props or .targets file. 'seedProps' pre-seeds well-known
    // properties (Platform, PlatformTarget, Configuration, NuGetPackageRoot, ...);
    // MSBuildThisFileDirectory is derived from 'path' internally. Appends into 'out'
    // so the caller can run .props then .targets through the same result.
    // 'importRoot' (optional): when nonempty, <Import> elements may resolve anywhere
    // UNDER that canonicalized directory (still depth-limited and existence-checked),
    // not just the same-dir/subdir of the importing file - a NuGet package folder is
    // the natural sandbox (e.g. WebView2's build/native/*.targets imports ../../build).
    // Empty (the default) preserves the same-dir-or-subdir-only behavior.
    // Returns false only on I/O or XML well-formedness failure ('errorMsg' set);
    // an evaluable file that yields nothing is success with an empty result.
    static bool EvaluateFile(const std::string& path,
                             const std::map<std::string, std::string>& seedProps,
                             EvalResult& out,
                             std::string& errorMsg,
                             bool verbose = false,
                             const std::string& importRoot = std::string())
    {
        std::string content;
        if (!ReadFile(path, content, errorMsg))
            return false;

        XmlNode root;
        if (!ParseXml(content, root, errorMsg))
            return false;

        if (!IEquals(root.name, "Project"))
        {
            errorMsg = "root element is not <Project> in " + path;
            return false;
        }

        EvalState st;
        st.verbose = verbose;
        st.out = &out;
        if (!importRoot.empty())
            st.importRoot = NormalizeAbs(importRoot);
        for (const auto& kv : seedProps)
            st.props[NormalizeKey(kv.first)] = kv.second;

        // Seed dedupe sets from whatever the caller already accumulated (e.g. a
        // prior .props pass), so a second EvaluateFile call into the same 'out'
        // does not duplicate entries.
        for (const auto& s : out.includeDirs) st.seenIncludeDirs.insert(DedupeKey(s));
        for (const auto& s : out.libs)        st.seenLibs.insert(DedupeKey(s));
        for (const auto& s : out.dlls)        st.seenDlls.insert(DedupeKey(s));

        std::string dirStr = DirWithTrailingSep(path);
        st.props[NormalizeKey("MSBuildThisFileDirectory")] = dirStr;

        WalkChildren(root, st, dirStr, 0);
        return true;
    }

private:
    static constexpr int kMaxImportDepth = 4;

    // ---------------------------------------------------------------
    // Minimal XML DOM + hand-rolled parser
    // ---------------------------------------------------------------
    struct XmlNode
    {
        std::string name;
        std::vector<std::pair<std::string, std::string>> attrs;
        std::vector<XmlNode> children;
        std::string text;
    };

    // Recursive-descent XML parser: elements, attributes, text, self-closing
    // tags, comments, the XML declaration, and CDATA. No DTD/namespace support -
    // these files use unprefixed element names, which is all we match against.
    class XParser
    {
    public:
        explicit XParser(const std::string& src) : s_(src) {}

        bool ParseDocument(XmlNode& root, std::string& err)
        {
            if (!SkipMisc(err)) return false;
            if (AtEnd() || Cur() != '<') { err = "expected root element"; return false; }
            return ParseElement(root, err);
        }

    private:
        const std::string& s_;
        size_t pos_ = 0;

        bool AtEnd() const { return pos_ >= s_.size(); }
        char Cur() const { return s_[pos_]; }
        void SkipWs() { while (!AtEnd() && std::isspace((unsigned char)Cur())) pos_++; }

        // Skips whitespace, the XML declaration, comments, and DOCTYPE-like
        // declarations that can appear before/between elements.
        bool SkipMisc(std::string& err)
        {
            while (true)
            {
                SkipWs();
                if (AtEnd()) return true;
                if (s_.compare(pos_, 2, "<?") == 0)
                {
                    size_t e = s_.find("?>", pos_ + 2);
                    if (e == std::string::npos) { err = "unterminated processing instruction"; return false; }
                    pos_ = e + 2;
                    continue;
                }
                if (s_.compare(pos_, 4, "<!--") == 0)
                {
                    size_t e = s_.find("-->", pos_ + 4);
                    if (e == std::string::npos) { err = "unterminated comment"; return false; }
                    pos_ = e + 3;
                    continue;
                }
                if (s_.compare(pos_, 2, "<!") == 0)
                {
                    size_t e = s_.find('>', pos_ + 2);
                    if (e == std::string::npos) { err = "unterminated declaration"; return false; }
                    pos_ = e + 1;
                    continue;
                }
                break;
            }
            return true;
        }

        bool ParseName(std::string& name)
        {
            size_t start = pos_;
            while (!AtEnd() && (std::isalnum((unsigned char)Cur()) || Cur() == '_' || Cur() == '-' || Cur() == '.' || Cur() == ':'))
                pos_++;
            if (pos_ == start) return false;
            name = s_.substr(start, pos_ - start);
            return true;
        }

        // Parses "<Name attr="val" ...>" or "<Name attr="val" .../>" (the
        // latter returns immediately with no children/text).
        bool ParseElement(XmlNode& node, std::string& err)
        {
            pos_++; // consume '<'
            if (!ParseName(node.name)) { err = "expected element name"; return false; }

            while (true)
            {
                SkipWs();
                if (AtEnd()) { err = "unexpected end inside <" + node.name + "> tag"; return false; }
                if (Cur() == '/')
                {
                    pos_++;
                    SkipWs();
                    if (AtEnd() || Cur() != '>') { err = "malformed self-closing tag <" + node.name + ">"; return false; }
                    pos_++;
                    return true;
                }
                if (Cur() == '>') { pos_++; break; }

                std::string aname;
                if (!ParseName(aname)) { err = "expected attribute name in <" + node.name + ">"; return false; }
                SkipWs();
                if (AtEnd() || Cur() != '=') { err = "expected '=' after attribute '" + aname + "'"; return false; }
                pos_++;
                SkipWs();
                if (AtEnd() || (Cur() != '"' && Cur() != '\'')) { err = "expected quoted value for attribute '" + aname + "'"; return false; }
                char q = Cur();
                pos_++;
                size_t start = pos_;
                while (!AtEnd() && Cur() != q) pos_++;
                if (AtEnd()) { err = "unterminated attribute value for '" + aname + "'"; return false; }
                std::string raw = s_.substr(start, pos_ - start);
                pos_++; // closing quote
                node.attrs.emplace_back(aname, DecodeEntities(raw));
            }

            return ParseContent(node, err);
        }

        // Parses child nodes/text until the matching close tag for 'node'.
        bool ParseContent(XmlNode& node, std::string& err)
        {
            while (true)
            {
                if (AtEnd()) { err = "missing close tag for <" + node.name + ">"; return false; }
                if (s_.compare(pos_, 4, "<!--") == 0)
                {
                    size_t e = s_.find("-->", pos_ + 4);
                    if (e == std::string::npos) { err = "unterminated comment"; return false; }
                    pos_ = e + 3;
                    continue;
                }
                if (s_.compare(pos_, 9, "<![CDATA[") == 0)
                {
                    size_t e = s_.find("]]>", pos_ + 9);
                    if (e == std::string::npos) { err = "unterminated CDATA"; return false; }
                    node.text += s_.substr(pos_ + 9, e - (pos_ + 9));
                    pos_ = e + 3;
                    continue;
                }
                if (Cur() == '<')
                {
                    if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '/')
                    {
                        pos_ += 2;
                        std::string closeName;
                        if (!ParseName(closeName)) { err = "malformed closing tag"; return false; }
                        SkipWs();
                        if (AtEnd() || Cur() != '>') { err = "malformed closing tag </" + closeName + ">"; return false; }
                        pos_++;
                        if (closeName != node.name)
                        {
                            err = "mismatched close tag: expected </" + node.name + "> got </" + closeName + ">";
                            return false;
                        }
                        return true;
                    }
                    if (s_.compare(pos_, 2, "<?") == 0)
                    {
                        size_t e = s_.find("?>", pos_ + 2);
                        if (e == std::string::npos) { err = "unterminated processing instruction"; return false; }
                        pos_ = e + 2;
                        continue;
                    }
                    XmlNode child;
                    if (!ParseElement(child, err)) return false;
                    node.children.push_back(std::move(child));
                    continue;
                }
                size_t start = pos_;
                while (!AtEnd() && Cur() != '<') pos_++;
                node.text += DecodeEntities(s_.substr(start, pos_ - start));
            }
        }
    };

    static bool ParseXml(const std::string& content, XmlNode& root, std::string& err)
    {
        XParser p(content);
        return p.ParseDocument(root, err);
    }

    static void AppendUtf8(std::string& out, unsigned long cp)
    {
        if (cp <= 0x7F) { out += (char)cp; }
        else if (cp <= 0x7FF)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    // Decodes the five standard XML entities plus numeric character references
    // (&#NN; / &#xHH;). Anything else beginning with '&' is left as-is.
    static std::string DecodeEntities(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            if (s[i] == '&')
            {
                size_t semi = s.find(';', i + 1);
                if (semi != std::string::npos && semi - i <= 10)
                {
                    std::string ent = s.substr(i + 1, semi - i - 1);
                    if (ent == "amp") { out += '&'; i = semi + 1; continue; }
                    if (ent == "lt") { out += '<'; i = semi + 1; continue; }
                    if (ent == "gt") { out += '>'; i = semi + 1; continue; }
                    if (ent == "quot") { out += '"'; i = semi + 1; continue; }
                    if (ent == "apos") { out += '\''; i = semi + 1; continue; }
                    if (!ent.empty() && ent[0] == '#')
                    {
                        bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                        try
                        {
                            long code = std::stol(ent.substr(hex ? 2 : 1), nullptr, hex ? 16 : 10);
                            AppendUtf8(out, (unsigned long)code);
                            i = semi + 1;
                            continue;
                        }
                        catch (...) { /* fall through, emit literally */ }
                    }
                }
            }
            out += s[i];
            i++;
        }
        return out;
    }

    // ---------------------------------------------------------------
    // String / path helpers
    // ---------------------------------------------------------------
    static std::string NormalizeKey(std::string s)
    {
        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    static bool IEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    }

    static bool EndsWithCI(const std::string& s, const std::string& suffix)
    {
        if (s.size() < suffix.size()) return false;
        return IEquals(s.substr(s.size() - suffix.size()), suffix);
    }

    static std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    static bool IsAbsolutePath(const std::string& p)
    {
        namespace fs = std::filesystem;
        if (p.empty()) return false;
        return fs::path(p).is_absolute();
    }

    static std::string JoinPath(const std::string& base, const std::string& rel)
    {
        namespace fs = std::filesystem;
        if (rel.empty()) return base;
        if (IsAbsolutePath(rel)) return rel;
        return (fs::path(base) / fs::path(rel)).string();
    }

    // Lowercased, backslash-normalized key used only for cross-call dedupe -
    // Windows paths are case-insensitive so this avoids near-duplicate entries.
    static std::string DedupeKey(const std::string& p)
    {
        std::string k = p;
        for (auto& c : k) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); }
        while (!k.empty() && k.back() == '\\') k.pop_back();
        return k;
    }

    static std::string NormalizeAbs(const std::string& p)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(fs::path(p), ec);
        return ec ? p : canon.string();
    }

    // MSBuildThisFileDirectory always carries a trailing separator, matching
    // real MSBuild semantics.
    static std::string DirWithTrailingSep(const std::string& filePath)
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::path(filePath).parent_path();
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(dir, ec);
        std::string s = (ec ? dir : canon).string();
        if (!s.empty() && s.back() != '\\' && s.back() != '/') s += "\\";
        return s;
    }

    static bool PathExists(const std::string& p)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        return fs::exists(fs::path(p), ec);
    }

    // ---------------------------------------------------------------
    // Property model: $(Prop) expansion. Undefined -> empty. A property
    // FUNCTION ($([...]...)) is out of v1 scope; callers must treat the whole
    // containing expression as unevaluable when hasPropFunc comes back true.
    // ---------------------------------------------------------------
    using PropMap = std::map<std::string, std::string>;

    static std::string ExpandProps(const std::string& in, const PropMap& props, bool& hasPropFunc)
    {
        hasPropFunc = false;
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size())
        {
            if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '(')
            {
                size_t j = i + 2;
                int depth = 1;
                while (j < in.size() && depth > 0)
                {
                    if (in[j] == '(') depth++;
                    else if (in[j] == ')') { depth--; if (depth == 0) break; }
                    j++;
                }
                if (depth != 0) { out += in[i]; i++; continue; } // unmatched '$(' - literal
                std::string inner = Trim(in.substr(i + 2, j - (i + 2)));
                if (!inner.empty() && inner[0] == '[')
                {
                    hasPropFunc = true;
                }
                else
                {
                    auto it = props.find(NormalizeKey(inner));
                    if (it != props.end()) out += it->second;
                }
                i = j + 1;
            }
            else
            {
                out += in[i];
                i++;
            }
        }
        return out;
    }

    // ---------------------------------------------------------------
    // Evaluation state carried through the whole (possibly multi-file) walk.
    // ---------------------------------------------------------------
    struct EvalState
    {
        PropMap props;
        bool verbose = false;
        EvalResult* out = nullptr;
        std::set<std::string> seenIncludeDirs, seenLibs, seenDlls;
        // Canonicalized sandbox for <Import> resolution; empty = same-dir/subdir only.
        std::string importRoot;
    };

    static void Verbose(EvalState& st, const std::string& msg)
    {
        if (st.verbose) std::cout << "[verbose] msbuild-lite: " << msg << "\n";
    }

    static bool GetAttr(const XmlNode& node, const std::string& name, std::string& value)
    {
        for (const auto& kv : node.attrs)
            if (IEquals(kv.first, name)) { value = kv.second; return true; }
        return false;
    }

    // ---------------------------------------------------------------
    // Condition evaluator: 'a'=='b' / !=, Exists('path'), !, And/Or,
    // parentheses, true/false. Anything else -> false (with a verbose note).
    // ---------------------------------------------------------------
    struct CondEval
    {
        const std::string& s;
        size_t pos = 0;
        bool bad = false;
        const std::string& baseDir;

        void SkipWs() { while (pos < s.size() && std::isspace((unsigned char)s[pos])) pos++; }

        bool MatchWord(const char* w)
        {
            SkipWs();
            size_t n = std::strlen(w);
            if (pos + n > s.size()) return false;
            for (size_t i = 0; i < n; i++)
                if (std::tolower((unsigned char)s[pos + i]) != std::tolower((unsigned char)w[i])) return false;
            if (pos + n < s.size() && (std::isalnum((unsigned char)s[pos + n]) || s[pos + n] == '_')) return false;
            pos += n;
            return true;
        }

        bool ParseString(std::string& out)
        {
            SkipWs();
            if (pos >= s.size() || s[pos] != '\'') { bad = true; return false; }
            pos++;
            size_t start = pos;
            while (pos < s.size() && s[pos] != '\'') pos++;
            if (pos >= s.size()) { bad = true; return false; }
            out = s.substr(start, pos - start);
            pos++;
            return true;
        }

        bool ParseUnary(bool& result)
        {
            SkipWs();
            if (pos < s.size() && s[pos] == '(')
            {
                pos++;
                if (!ParseOr(result)) return false;
                SkipWs();
                if (pos >= s.size() || s[pos] != ')') { bad = true; return false; }
                pos++;
                return true;
            }
            if (pos < s.size() && s[pos] == '!')
            {
                pos++;
                bool inner;
                if (!ParseUnary(inner)) return false;
                result = !inner;
                return true;
            }
            if (MatchWord("true")) { result = true; return true; }
            if (MatchWord("false")) { result = false; return true; }
            if (MatchWord("Exists"))
            {
                SkipWs();
                if (pos >= s.size() || s[pos] != '(') { bad = true; return false; }
                pos++;
                std::string arg;
                if (!ParseString(arg)) return false;
                SkipWs();
                if (pos >= s.size() || s[pos] != ')') { bad = true; return false; }
                pos++;
                std::string abs = IsAbsolutePath(arg) ? arg : JoinPath(baseDir, arg);
                result = PathExists(abs);
                return true;
            }

            std::string lhs;
            if (pos >= s.size() || s[pos] != '\'') { bad = true; return false; }
            if (!ParseString(lhs)) return false;
            SkipWs();
            bool eq;
            if (pos + 1 < s.size() && s[pos] == '=' && s[pos + 1] == '=') { eq = true; pos += 2; }
            else if (pos + 1 < s.size() && s[pos] == '!' && s[pos + 1] == '=') { eq = false; pos += 2; }
            else { bad = true; return false; }
            std::string rhs;
            if (!ParseString(rhs)) return false;
            result = IEquals(lhs, rhs);
            if (!eq) result = !result;
            return true;
        }

        bool ParseAnd(bool& result)
        {
            bool r;
            if (!ParseUnary(r)) return false;
            while (true)
            {
                size_t save = pos;
                if (MatchWord("and"))
                {
                    bool rhs;
                    if (!ParseUnary(rhs)) return false;
                    r = r && rhs;
                }
                else { pos = save; break; }
            }
            result = r;
            return true;
        }

        bool ParseOr(bool& result)
        {
            bool r;
            if (!ParseAnd(r)) return false;
            while (true)
            {
                size_t save = pos;
                if (MatchWord("or"))
                {
                    bool rhs;
                    if (!ParseAnd(rhs)) return false;
                    r = r || rhs;
                }
                else { pos = save; break; }
            }
            result = r;
            return true;
        }
    };

    static bool EvalCondition(const std::string& rawCond, EvalState& st, const std::string& currentDir)
    {
        bool hasFunc = false;
        std::string expanded = ExpandProps(rawCond, st.props, hasFunc);
        if (hasFunc)
        {
            Verbose(st, "condition contains a property function, unevaluable -> false: " + rawCond);
            return false;
        }
        CondEval ce{expanded, 0, false, currentDir};
        bool result = false;
        bool ok = ce.ParseOr(result);
        ce.SkipWs();
        if (!ok || ce.bad || ce.pos != expanded.size())
        {
            Verbose(st, "unsupported condition construct -> false: " + rawCond);
            return false;
        }
        return result;
    }

    // Absent/empty Condition = true; otherwise defers to EvalCondition.
    static bool CheckCondition(const XmlNode& node, EvalState& st, const std::string& currentDir)
    {
        std::string cond;
        if (!GetAttr(node, "Condition", cond)) return true;
        if (Trim(cond).empty()) return true;
        return EvalCondition(cond, st, currentDir);
    }

    // ---------------------------------------------------------------
    // ';'-separated item/metadata list splitting: trims, drops empty and
    // "%(...)" recursion tokens (self-reference to the accumulated list).
    // ---------------------------------------------------------------
    static std::vector<std::string> SplitList(const std::string& value)
    {
        std::vector<std::string> out;
        std::stringstream ss(value);
        std::string piece;
        while (std::getline(ss, piece, ';'))
        {
            std::string t = Trim(piece);
            if (t.empty()) continue;
            if (t.find("%(") != std::string::npos) continue;
            out.push_back(t);
        }
        return out;
    }

    static void AddUnique(std::vector<std::string>& list, std::set<std::string>& seen, const std::string& absPath)
    {
        std::string key = DedupeKey(absPath);
        if (seen.insert(key).second) list.push_back(absPath);
    }

    static void HarvestIncludeList(EvalState& st, const std::string& rawList, const std::string& baseDir)
    {
        namespace fs = std::filesystem;
        for (const auto& tok : SplitList(rawList))
        {
            std::string abs = NormalizeAbs(IsAbsolutePath(tok) ? tok : JoinPath(baseDir, tok));
            std::error_code ec;
            if (fs::exists(abs, ec) && fs::is_directory(abs, ec))
                AddUnique(st.out->includeDirs, st.seenIncludeDirs, abs);
            else
                Verbose(st, "include dir does not exist, skipped: " + abs);
        }
    }

    static void HarvestLibList(EvalState& st, const std::string& rawList, const std::string& baseDir)
    {
        namespace fs = std::filesystem;
        for (const auto& tok : SplitList(rawList))
        {
            if (!EndsWithCI(tok, ".lib")) continue;
            std::string abs = NormalizeAbs(IsAbsolutePath(tok) ? tok : JoinPath(baseDir, tok));
            std::error_code ec;
            if (fs::exists(abs, ec) && fs::is_regular_file(abs, ec))
                AddUnique(st.out->libs, st.seenLibs, abs);
            else
                Verbose(st, "lib token unresolved, treated as a system lib and skipped: " + tok);
        }
    }

    // ---------------------------------------------------------------
    // Wildcard expansion for Include specs (only when '*'/'?' are present).
    // Safe-by-construction: every fixed path segment before a wildcard must
    // already exist, or the branch is pruned.
    // ---------------------------------------------------------------
    static std::regex WildcardToRegex(const std::string& pattern)
    {
        std::string re = "^";
        for (char c : pattern)
        {
            if (c == '*') re += ".*";
            else if (c == '?') re += ".";
            else if (std::strchr(".^$+(){}|[]\\", c)) { re += '\\'; re += c; }
            else re += c;
        }
        re += "$";
        return std::regex(re, std::regex::icase);
    }

    static void GlobRecurse(const std::vector<std::string>& segs, size_t idx, const std::filesystem::path& current, std::vector<std::string>& results)
    {
        namespace fs = std::filesystem;
        if (idx == segs.size()) { results.push_back(current.string()); return; }
        const std::string& seg = segs[idx];
        bool hasWild = seg.find('*') != std::string::npos || seg.find('?') != std::string::npos;
        std::error_code ec;

        if (!hasWild)
        {
            fs::path next = current / seg;
            if (idx + 1 < segs.size() && (!fs::exists(next, ec) || !fs::is_directory(next, ec))) return;
            GlobRecurse(segs, idx + 1, next, results);
            return;
        }
        if (seg == "**")
        {
            GlobRecurse(segs, idx + 1, current, results); // zero-directory match
            if (fs::exists(current, ec) && fs::is_directory(current, ec))
                for (const auto& entry : fs::directory_iterator(current, ec))
                    if (entry.is_directory()) GlobRecurse(segs, idx, entry.path(), results);
            return;
        }
        if (!fs::exists(current, ec) || !fs::is_directory(current, ec)) return;
        std::regex re = WildcardToRegex(seg);
        for (const auto& entry : fs::directory_iterator(current, ec))
        {
            std::string name = entry.path().filename().string();
            if (std::regex_match(name, re)) GlobRecurse(segs, idx + 1, entry.path(), results);
        }
    }

    // Returns candidate absolute paths for 'spec'. Non-wildcard specs return
    // exactly one path (existence is the caller's responsibility to check).
    static std::vector<std::string> ResolveIncludeSpec(const std::string& spec, const std::string& baseDir)
    {
        namespace fs = std::filesystem;
        std::string abs = IsAbsolutePath(spec) ? spec : JoinPath(baseDir, spec);
        for (auto& c : abs) if (c == '/') c = '\\';

        if (abs.find('*') == std::string::npos && abs.find('?') == std::string::npos)
            return { NormalizeAbs(abs) };

        std::string root;
        size_t start = 0;
        size_t colon = abs.find(':');
        if (colon != std::string::npos && colon + 1 < abs.size() && abs[colon + 1] == '\\')
        {
            root = abs.substr(0, colon + 1); // "C:"
            start = colon + 2;
        }
        else if (!abs.empty() && abs[0] == '\\')
        {
            root = "";
            start = 1;
        }

        std::vector<std::string> segments;
        std::stringstream ss(abs.substr(start));
        std::string seg;
        while (std::getline(ss, seg, '\\'))
            if (!seg.empty()) segments.push_back(seg);

        std::vector<std::string> results;
        GlobRecurse(segments, 0, fs::path(root.empty() ? "\\" : root + "\\"), results);
        return results;
    }

    // ---------------------------------------------------------------
    // Document walk
    // ---------------------------------------------------------------
    static void ProcessPropertyGroup(const XmlNode& node, EvalState& st, const std::string& currentDir)
    {
        for (const auto& prop : node.children)
        {
            if (!CheckCondition(prop, st, currentDir))
            {
                Verbose(st, "property '" + prop.name + "' skipped (condition false)");
                continue;
            }
            bool hasFunc = false;
            std::string val = ExpandProps(prop.text, st.props, hasFunc);
            if (hasFunc)
            {
                Verbose(st, "property '" + prop.name + "' has a property function, unevaluable, skipped");
                continue;
            }
            st.props[NormalizeKey(prop.name)] = val;

            // Defensive: honor these two well-known names even if authored as a
            // bare property rather than item metadata (spec: "any element...").
            if (IEquals(prop.name, "AdditionalIncludeDirectories")) HarvestIncludeList(st, val, currentDir);
            if (IEquals(prop.name, "AdditionalDependencies")) HarvestLibList(st, val, currentDir);
        }
    }

    static void ProcessItemGroup(const XmlNode& node, EvalState& st, const std::string& currentDir)
    {
        namespace fs = std::filesystem;
        for (const auto& item : node.children)
        {
            if (!CheckCondition(item, st, currentDir))
            {
                Verbose(st, "item '" + item.name + "' skipped (condition false)");
                continue;
            }

            std::string includeRaw;
            bool hasInclude = GetAttr(item, "Include", includeRaw);
            std::string includeExpanded;
            if (hasInclude)
            {
                bool hasFunc = false;
                includeExpanded = ExpandProps(includeRaw, st.props, hasFunc);
                if (hasFunc)
                {
                    Verbose(st, "item '" + item.name + "' Include has a property function, unevaluable, skipped");
                    continue;
                }
            }

            std::map<std::string, std::string> meta;
            for (const auto& m : item.children)
            {
                if (!CheckCondition(m, st, currentDir)) continue;
                bool hasFunc = false;
                std::string mv = ExpandProps(m.text, st.props, hasFunc);
                if (hasFunc)
                {
                    Verbose(st, "metadata '" + m.name + "' has a property function, unevaluable, skipped");
                    continue;
                }
                meta[NormalizeKey(m.name)] = mv;
            }

            auto itInc = meta.find("ADDITIONALINCLUDEDIRECTORIES");
            if (itInc != meta.end()) HarvestIncludeList(st, itInc->second, currentDir);

            auto itDep = meta.find("ADDITIONALDEPENDENCIES");
            if (itDep != meta.end()) HarvestLibList(st, itDep->second, currentDir);

            if (hasInclude && !includeExpanded.empty())
            {
                for (const auto& candidate : ResolveIncludeSpec(includeExpanded, currentDir))
                {
                    std::error_code ec;
                    if (!fs::exists(candidate, ec) || !fs::is_regular_file(candidate, ec)) continue;
                    if (EndsWithCI(candidate, ".lib")) AddUnique(st.out->libs, st.seenLibs, candidate);
                    else if (EndsWithCI(candidate, ".dll")) AddUnique(st.out->dlls, st.seenDlls, candidate);
                }
            }
        }
    }

    // Follows an <Import> only when the resolved Project path stays at or
    // below 'currentDir' (no ".." escape) and the file exists; depth-limited.
    static void ProcessImport(const XmlNode& node, EvalState& st, const std::string& currentDir, int importDepth)
    {
        if (!CheckCondition(node, st, currentDir))
        {
            Verbose(st, "Import skipped (condition false)");
            return;
        }
        std::string projRaw;
        if (!GetAttr(node, "Project", projRaw))
        {
            Verbose(st, "Import missing 'Project' attribute, skipped");
            return;
        }
        bool hasFunc = false;
        std::string projExpanded = ExpandProps(projRaw, st.props, hasFunc);
        if (hasFunc)
        {
            Verbose(st, "Import Project contains a property function, unevaluable, skipped: " + projRaw);
            return;
        }
        if (Trim(projExpanded).empty())
        {
            Verbose(st, "Import Project expands to empty, skipped: " + projRaw);
            return;
        }

        namespace fs = std::filesystem;
        std::string target = NormalizeAbs(IsAbsolutePath(projExpanded) ? projExpanded : JoinPath(currentDir, projExpanded));
        std::string curDirNorm = NormalizeAbs(currentDir);

        std::error_code ec;
        // Default sandbox is same-dir-or-subdir of the importing file. When an
        // importRoot is set, widen it to "anywhere under importRoot" (prefix check).
        bool escapesUp;
        if (!st.importRoot.empty())
        {
            fs::path relRoot = fs::relative(target, st.importRoot, ec);
            escapesUp = ec || relRoot.empty() || *relRoot.begin() == "..";
            if (escapesUp)
                Verbose(st, "Import target escapes the package importRoot, out of scope, skipped: " + target);
        }
        else
        {
            fs::path rel = fs::relative(target, curDirNorm, ec);
            escapesUp = ec || rel.empty() || *rel.begin() == "..";
            if (escapesUp)
                Verbose(st, "Import target is not same-directory-or-subdirectory of the importing file, out of v1 scope, skipped: " + target);
        }
        if (escapesUp)
            return;
        if (importDepth >= kMaxImportDepth)
        {
            Verbose(st, "Import depth limit reached, skipped: " + target);
            return;
        }
        if (!fs::exists(target, ec))
        {
            Verbose(st, "Import target does not exist, skipped: " + target);
            return;
        }

        std::string content, readErr;
        if (!ReadFile(target, content, readErr))
        {
            Verbose(st, "Import target unreadable, skipped: " + target + " (" + readErr + ")");
            return;
        }
        XmlNode importedRoot;
        std::string xmlErr;
        if (!ParseXml(content, importedRoot, xmlErr))
        {
            Verbose(st, "Import target is not well-formed XML, skipped: " + target + " (" + xmlErr + ")");
            return;
        }

        std::string importedDir = DirWithTrailingSep(target);
        std::string savedDir = st.props[NormalizeKey("MSBuildThisFileDirectory")];
        st.props[NormalizeKey("MSBuildThisFileDirectory")] = importedDir;
        WalkChildren(importedRoot, st, importedDir, importDepth + 1);
        st.props[NormalizeKey("MSBuildThisFileDirectory")] = savedDir;
    }

    static void WalkChildren(const XmlNode& parent, EvalState& st, const std::string& currentDir, int importDepth)
    {
        for (const auto& child : parent.children)
        {
            if (IEquals(child.name, "PropertyGroup"))
            {
                if (!CheckCondition(child, st, currentDir)) { Verbose(st, "PropertyGroup skipped (condition false)"); continue; }
                ProcessPropertyGroup(child, st, currentDir);
            }
            else if (IEquals(child.name, "ItemGroup") || IEquals(child.name, "ItemDefinitionGroup"))
            {
                if (!CheckCondition(child, st, currentDir)) { Verbose(st, "ItemGroup skipped (condition false)"); continue; }
                ProcessItemGroup(child, st, currentDir);
            }
            else if (IEquals(child.name, "Import"))
            {
                ProcessImport(child, st, currentDir, importDepth);
            }
            else if (IEquals(child.name, "Target"))
            {
                Verbose(st, "skipping <Target> element (out of v1 scope)");
            }
            else
            {
                Verbose(st, "skipping unsupported element <" + child.name + ">");
            }
        }
    }

    // ---------------------------------------------------------------
    // File I/O
    // ---------------------------------------------------------------
    static bool ReadFile(const std::string& path, std::string& content, std::string& err)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            err = "cannot open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        content = ss.str();
        if (content.size() >= 3 &&
            (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        {
            content.erase(0, 3);
        }
        return true;
    }
};
