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
#include "CxxSpelling.h"
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

#include "LLVMBackend_CInteropShared.h"

bool LLVMBackend::RequestCxxBraceConstructor(
        const std::string& typeName, std::vector<NamedVariable>& arguments,
        std::vector<CxxBraceArgument>& braceArguments, std::string& wrapperName,
        std::string& error)
{
        error.clear();
        wrapperName.clear();
        if (braceArguments.empty()) return false;
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return false;

        const cflat_cinterop::RawCxxMember* selected = nullptr;
        for (const auto& member : record->second.members)
        {
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                || member.paramTypes.size() < arguments.size() + 1)
                continue;
            bool fits = true;
            for (const auto& brace : braceArguments)
                if (brace.parameterIndex + 1 >= member.paramTypes.size()) { fits = false; break; }
            if (!fits) continue;
            if (selected == nullptr || member.paramTypes.size() == arguments.size() + 1)
            {
                selected = &member;
                if (member.paramTypes.size() == arguments.size() + 1) break;
            }
        }
        if (selected == nullptr) return false;

        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(typeName, ownerSpelling))
        {
            error = "the C++ class type is not registered";
            return false;
        }
        auto braceForArgument = [&](size_t index) -> const CxxBraceArgument* {
            for (const auto& brace : braceArguments)
                if (brace.argumentIndex == index) return &brace;
            return nullptr;
        };
        std::vector<std::string> parameterSpellings{ ownerSpelling + " *" };
        std::vector<std::string> callArguments;
        std::string wrapperBodyPrefix;
        std::string selectorSource;
        std::map<size_t, bool> selectorElements;
        bool wrapperNeedsInitializerList = false;
        // A braced list whose object a constructor can keep a view of (an initializer_list or an
        // array reference) is backed by a CALLER-frame array, as C++ keeps it to the end of the
        // caller's full-expression; a thunk-local array would dangle once the thunk returns.
        struct BackedBrace
        {
            size_t argumentIndex = 0;
            size_t flatIndex = 0;
            std::string element;
        };
        std::vector<BackedBrace> backedBraces;
        // Which braced arguments are backed, and as what: an initializer_list candidate outranks
        // an array for a braced list ([over.ics.list]), so arrays count only without one.
        std::map<size_t, bool> backedList;
        auto scalarElement = [&](const std::string& element) { return IsCxxScalarOrBoolBraceElement(element); };
        // The parameter as a selector declares it (an array's extent is the list's own count, a
        // deduced `N` included); empty when it cannot take the backed list of that kind.
        auto backedParameter = [&](const std::string& parameter, size_t count, bool list) {
            std::string element;
            std::string extent;
            if (list)
                return CxxBraceContainerKindOf(parameter) == CxxBraceContainerKind::InitializerList
                    && scalarElement(CxxBraceTargetElementSpelling(parameter))
                    ? parameter : std::string();
            if (!CxxArrayReferenceParameter(parameter, element, extent) || !scalarElement(element))
                return std::string();
            if (!extent.empty() && std::all_of(extent.begin(), extent.end(),
                    [](char c) { return std::isdigit((unsigned char)c) != 0; })
                && std::stoull(extent) != count)
                return std::string();
            return parameter.substr(0, parameter.rfind('[')) + "[" + std::to_string(count) + "]";
        };
        auto fitsArity = [&](const cflat_cinterop::RawCxxMember& member) {
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                || member.paramTypes.size() < arguments.size() + 1)
                return false;
            // A trailing parameter pack left without arguments is empty, as in C++.
            for (size_t n = arguments.size() + 1; n < member.paramTypes.size(); ++n)
                if ((n >= member.defaultArgs.size() || member.defaultArgs[n].kind.empty())
                    && !(n + 1 == member.paramTypes.size() && member.paramTypes[n].ends_with("...")))
                    return false;
            return true;
        };
        for (const auto& brace : braceArguments)
        {
            if (brace.hasCxxClassElements || brace.elements.empty()) continue;
            bool anyList = false;
            bool anyArray = false;
            for (const auto& member : record->second.members)
            {
                if (!fitsArity(member)) continue;
                const std::string& parameter = member.paramTypes[brace.argumentIndex + 1];
                anyList = anyList || !backedParameter(parameter, brace.elements.size(), true).empty();
                anyArray = anyArray || !backedParameter(parameter, brace.elements.size(), false).empty();
            }
            if (anyList || anyArray) backedList[brace.argumentIndex] = anyList;
        }
        // Every constructor that can take the backed lists, by its FULL parameter list: which one
        // clang picks depends on the other arguments too.
        struct SelectorCandidate
        {
            std::vector<std::string> params;   // positions 0.. (after `this`), defaults included
            size_t required = 0;
            // A constructor template's own type / non-type parameters (`type-parameter-D-I`,
            // `value-parameter-D-I` in clang's spelling), mirrored as template parameters of the
            // selector overload; templateDeclarators holds each one's `class` / `auto` form.
            std::vector<std::string> templateParams;
            std::vector<std::string> templateDeclarators;
            // A template parameter only a DEFAULT argument names: C++ never deduces it there,
            // so the candidate is not viable.
            bool undeducible = false;
        };
        std::vector<SelectorCandidate> selectorCandidates;
        std::string selectorNs;
        if (!backedList.empty())
        {
            std::set<std::string> seen;
            for (const auto& member : record->second.members)
            {
                if (!fitsArity(member)) continue;
                SelectorCandidate candidate;
                candidate.required = arguments.size();
                bool viable = true;
                std::string key;
                std::set<std::string> deduced;
                for (size_t n = 1; n < member.paramTypes.size() && viable; ++n)
                {
                    std::string parameter = member.paramTypes[n];
                    if (parameter.ends_with("..."))
                    {
                        if (n > arguments.size()) break;
                        std::string packPattern = parameter.substr(0, parameter.size() - 3);
                        const bool constLvaluePack = packPattern.starts_with("const ")
                            && packPattern.find('*') == std::string::npos
                            && packPattern.ends_with('&');
                        if (packPattern.find('&') != std::string::npos && !constLvaluePack)
                        {
                            error = std::format("a C++ constructor of '{}' passes an argument into a "
                                                "parameter pack beside its brace-list constructors; "
                                                "CFlat cannot rank that call",
                                                DisplayCxxClassName(typeName));
                            return false;
                        }
                        // A pattern naming another template parameter too (`pair<U, A>&...`), or only a
                        // non-type one (`array<int, N>&...`), cannot be mirrored per element; refuse as
                        // master did rather than drop the candidate (a sibling would win silently).
                        std::set<std::string> patternParams;
                        for (const std::string_view prefix : { "type-parameter-", "value-parameter-" })
                            for (size_t at = packPattern.find(prefix); at != std::string::npos;
                                 at = packPattern.find(prefix, at + 1))
                            {
                                size_t end = at + prefix.size();
                                while (end < packPattern.size()
                                       && (std::isdigit((unsigned char)packPattern[end]) != 0
                                           || packPattern[end] == '-'))
                                    ++end;
                                patternParams.insert(packPattern.substr(at, end - at));
                            }
                        if (patternParams.size() > 1
                            || packPattern.find("type-parameter-") == std::string::npos)
                        {
                            error = std::format("a C++ constructor of '{}' passes an argument into a "
                                                "parameter pack beside its brace-list constructors; "
                                                "CFlat cannot rank that call",
                                                DisplayCxxClassName(typeName));
                            return false;
                        }
                        for (size_t packed = n - 1; packed < arguments.size(); ++packed)
                        {
                            const std::string name = "packarg_" + std::to_string(packed);
                            candidate.templateParams.push_back(name);
                            candidate.templateDeclarators.push_back("class " + name);
                            std::string expanded = packPattern;
                            bool substituted = false;
                            for (size_t at = expanded.find("type-parameter-"); at != std::string::npos;
                                 at = expanded.find("type-parameter-", at + name.size()))
                            {
                                size_t end = at + std::string_view("type-parameter-").size();
                                while (end < expanded.size()
                                       && (std::isdigit((unsigned char)expanded[end]) != 0
                                           || expanded[end] == '-'))
                                    ++end;
                                expanded.replace(at, end - at, name);
                                substituted = true;
                            }
                            if (!substituted)
                            {
                                viable = false;
                                break;
                            }
                            candidate.params.push_back(expanded);
                            key += expanded + "|";
                        }
                        if (!viable) break;
                        break;
                    }
                    if (auto kind = backedList.find(n - 1); kind != backedList.end())
                    {
                        auto brace = std::find_if(braceArguments.begin(), braceArguments.end(),
                            [&](const CxxBraceArgument& b) { return b.argumentIndex == n - 1; });
                        parameter = backedParameter(parameter, brace->elements.size(), kind->second);
                        viable = !parameter.empty();
                    }
                    for (const bool value : { false, true })
                    {
                        const std::string prefix = value ? "value-parameter-" : "type-parameter-";
                        for (size_t at = parameter.find(prefix); at != std::string::npos;
                             at = parameter.find(prefix, at))
                        {
                            size_t end = at + prefix.size();
                            while (end < parameter.size()
                                   && (std::isdigit((unsigned char)parameter[end]) != 0
                                       || parameter[end] == '-'))
                                ++end;
                            std::string name = (value ? "vp" : "tp")
                                + parameter.substr(at + prefix.size() - 1, end - at - prefix.size() + 1);
                            std::replace(name.begin(), name.end(), '-', '_');
                            parameter.replace(at, end - at, name);
                            if (std::find(candidate.templateParams.begin(),
                                          candidate.templateParams.end(), name)
                                == candidate.templateParams.end())
                            {
                                candidate.templateParams.push_back(name);
                                candidate.templateDeclarators.push_back(
                                    (value ? "auto " : "class ") + name);
                            }
                            if (n <= arguments.size()) deduced.insert(name);
                            else if (deduced.count(name) == 0) candidate.undeducible = true;
                            at += name.size();
                        }
                    }
                    candidate.params.push_back(parameter);
                    key += parameter + "|";
                }
                if (viable && seen.insert(key).second) selectorCandidates.push_back(std::move(candidate));
            }
            if (selectorCandidates.empty()) backedList.clear();
            if (!selectorCandidates.empty()
                && std::all_of(selectorCandidates.begin(), selectorCandidates.end(),
                               [](const SelectorCandidate& c) { return c.undeducible; }))
            {
                error = std::format("no C++ constructor of '{}' accepts this brace list: a "
                                    "template parameter named only by a default argument is "
                                    "never deduced", DisplayCxxClassName(typeName));
                return false;
            }
        }
        // A lone constructor template still needs clang to deduce its parameters.
        const bool useSelector = selectorCandidates.size() > 1
            || (!selectorCandidates.empty() && !selectorCandidates.front().templateParams.empty());
        if (useSelector)
        {
            // Several constructors can take the lists: clang ranks the COMPLETE call against a
            // mirror of them, each returning its own parameter types (constants stay constants,
            // so narrowing reads as at the source).
            std::string callText;
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                std::string one;
                if (auto brace = braceForArgument(i); brace != nullptr)
                {
                    std::vector<std::string> candidateElements;
                    for (const auto& candidate : selectorCandidates)
                    {
                        std::string element;
                        std::string extent;
                        if (!CxxArrayReferenceParameter(candidate.params[i], element, extent))
                            element = CxxBraceTargetElementSpelling(candidate.params[i]);
                        candidateElements.push_back(element);
                    }
                    one = "{";
                    for (size_t e = 0; e < brace->elements.size(); ++e)
                    {
                        const std::string element = CxxBraceSelectorElement(brace->elements[e],
                                                                            candidateElements);
                        if (element.empty())
                        {
                            error = "brace arguments must contain scalar values of one type";
                            return false;
                        }
                        one += (e != 0 ? ", " : "") + element;
                    }
                    one += "}";
                }
                else
                {
                    std::vector<std::string> candidateTypes;
                    for (const auto& candidate : selectorCandidates)
                        candidateTypes.push_back(TrimCxxBraceType(StripCxxRefAndCv(candidate.params[i])));
                    const NamedVariable& arg = arguments[i];
                    one = CxxBraceSelectorElement(arg, candidateTypes);
                    if (IsNullPointerConstantArgument(arg, true))
                    {
                        if (arg.IsCxxMacroGlobal && !arg.CallerName.empty())
                            one = arg.CallerName;
                        else if (std::string zero = CxxZeroLiteralSpelling(arg.LiteralIdentity);
                                 !zero.empty())
                            one = zero;
                    }
                    std::string spelling;
                    auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                    // A string literal ranks as the `const char[N]` lvalue it is in C++.
                    if (one.empty() && constant != nullptr && IsStringLiteralConstant(constant))
                        one = "declval<const char (&)["
                            + std::to_string(stringLiteralLenByPtr.at(constant) + 1) + "]>()";
                    else if (one.empty() && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arg.Primary))
                        one = "nullptr";
                    if (one.empty() && !arg.TypeAndValue.Pointer && IsCxxRecord(arg.TypeAndValue.TypeName)
                        && CxxSpellingForCflatType(arg.TypeAndValue.TypeName, spelling))
                    {
                        const bool named = arg.Storage != nullptr || (!arg.CallerName.empty()
                            && FindVariableStorage(arg.CallerName).Storage != nullptr)
                            || (!arg.IsExplicitMove
                                && (arg.CxxLvalueKind == 1 || arg.CxxLvalueKind == 2))
                            || !arg.IsRvalue;
                        one = "declval<" + spelling + (named && !arg.IsExplicitMove ? " &" : "") + ">()";
                    }
                    else if (one.empty() && arg.TypeAndValue.Pointer)
                    {
                        std::string pointee = arg.TypeAndValue.TypeName;
                        if (arg.TypeAndValue.ElemPointer) pointee += "*";
                        if (CxxSpellingForCflatType(pointee, spelling))
                            one = "declval<" + spelling + " *>()";
                    }
                    if (one.empty())
                    {
                        error = std::format("C++ constructor argument {} of '{}' cannot be ranked "
                                            "against its brace-list overloads", i + 1,
                                            DisplayCxxClassName(typeName));
                        return false;
                    }
                }
                callText += (i != 0 ? ", " : "") + one;
            }
            std::string candidatesText;
            for (const auto& candidate : selectorCandidates)
            {
                for (const auto& parameter : candidate.params) candidatesText += parameter + ",";
                candidatesText += ";";
            }
            selectorNs = std::format("__cflat_bsel_{:016x}",
                HashWrapperKey(typeName + "|" + candidatesText + "|" + callText));
            selectorSource = "namespace " + selectorNs + " {\ntemplate <class U> U && declval();\n"
                "template <class T> struct el;\n"
                "template <class T> struct el<std::initializer_list<T>> { using t = T; };\n"
                "template <class T> struct el<const std::initializer_list<T>> { using t = T; };\n"
                "template <class T> struct el<const std::initializer_list<T> &> { using t = T; };\n"
                "template <class T> struct el<std::initializer_list<T> &&> { using t = T; };\n"
                "template <class T, decltype(sizeof 0) N> struct el<T (&)[N]> { using t = T; };\n"
                "template <class T, decltype(sizeof 0) N> struct el<T (&&)[N]> { using t = T; };\n"
                "template <class T> struct nc { using t = T; };\n"
                "template <class T> struct nc<const T> { using t = T; };\n";
            std::string members;
            for (size_t c = 0; c < selectorCandidates.size(); ++c)
            {
                const auto& candidate = selectorCandidates[c];
                std::string tpl;
                std::string tplArgs;
                for (size_t t = 0; t < candidate.templateParams.size(); ++t)
                {
                    tpl += (tpl.empty() ? "template <" : ", ") + candidate.templateDeclarators[t];
                    tplArgs += (tplArgs.empty() ? "<" : ", ") + candidate.templateParams[t];
                }
                if (!tpl.empty()) { tpl += "> "; tplArgs += ">"; }
                std::string cand = tpl + "struct c" + std::to_string(c) + " {";
                members += tpl + "static auto sel(";
                for (size_t n = 0; n < candidate.params.size(); ++n)
                {
                    cand += " using p" + std::to_string(n) + " = " + candidate.params[n] + ";";
                    members += (n != 0 ? ", " : "") + candidate.params[n];
                    if (n >= candidate.required)
                        members += " = declval<" + candidate.params[n] + ">()";
                }
                selectorSource += cand + " };\n";
                members += ") -> c" + std::to_string(c) + tplArgs + ";\n";
            }
            selectorSource += "struct S {\n" + members + "};\nusing R = decltype(S::sel(" + callText
                + "));\n";
            for (const auto& [index, list] : backedList)
                selectorSource += "using Q" + std::to_string(index) + " = typename el<R::p"
                    + std::to_string(index) + ">::t;\nusing E" + std::to_string(index)
                    + " = typename nc<Q" + std::to_string(index) + ">::t;\n";
            selectorSource += "}\n";
            // The namespace is keyed by its own text only while the wrapper also keys on element
            // spellings: two wrappers may share it, so the incremental TU parses it once.
            cxxBraceSelectorSources_.insert(selectorSource);
            wrapperNeedsInitializerList = true;
        }
        size_t flatParameterIndex = 1;
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const auto* brace = braceForArgument(i);
            if (brace != nullptr)
            {
                const size_t rawIndex = i + 1;
                const size_t count = brace->elements.size();
                std::string listElement;
                std::string arrayElement;
                std::string listType;
                if (auto kind = backedList.find(i); kind != backedList.end())
                {
                    if (useSelector)
                    {
                        const std::string e = selectorNs + "::E" + std::to_string(i);
                        (kind->second ? listElement : arrayElement) = e;
                        listType = "std::initializer_list<" + selectorNs + "::Q"
                                 + std::to_string(i) + ">";
                        selectorElements[i] = true;
                    }
                    else
                    {
                        const std::string& parameter = selectorCandidates.front().params[i];
                        std::string extent;
                        if (kind->second)
                        {
                            listElement = CxxBraceTargetElementSpelling(parameter);
                            listType = StripCxxRefAndCv(parameter);
                        }
                        else
                            CxxArrayReferenceParameter(parameter, arrayElement, extent);
                    }
                }
                if (!listElement.empty() || !arrayElement.empty())
                {
                    const bool list = !listElement.empty();
                    BackedBrace backed;
                    backed.argumentIndex = i;
                    backed.flatIndex = flatParameterIndex++;
                    backed.element = list ? listElement : arrayElement;
                    const std::string p = "p" + std::to_string(backed.flatIndex);
                    const std::string n = std::to_string(count);
                    if (list)
                    {
                        wrapperNeedsInitializerList = true;
                        const std::string il = "__cflat_il_" + std::to_string(i);
                        parameterSpellings.push_back("const " + backed.element + " *");
                        wrapperBodyPrefix += CxxInitializerListOverStorage(listType, il, p, count);
                        // A prvalue copy binds a by-value, `const &` and `&&` parameter alike.
                        callArguments.push_back(listType + "(" + il + ")");
                    }
                    else
                    {
                        // A braced list is a prvalue: the xvalue binds `const T (&)[N]` and
                        // `T (&&)[N]`, never `T (&)[N]`, exactly like the list did.
                        parameterSpellings.push_back(backed.element + " (&)[" + n + "]");
                        callArguments.push_back("static_cast<" + backed.element + " (&&)[" + n
                                                + "]>(" + p + ")");
                    }
                    backedBraces.push_back(std::move(backed));
                    continue;
                }
                // A list copy-list-initializing a CLASS parameter through that class's own
                // initializer_list<E> constructor (an ArrayRef-like view) is backed the same way
                // when every constructor that fits takes that one class there.
                std::string classElement;
                std::string classSpelling;
                if (backedList.empty() && !brace->hasCxxClassElements && !brace->elements.empty()
                    && CxxBraceElementsAllScalar(*brace))
                    for (const auto& member : record->second.members)
                    {
                        const std::string target = fitsArity(member)
                            ? StripCxxRefAndCv(member.paramTypes[rawIndex]) : ownerSpelling;
                        if (target == ownerSpelling) continue;
                        const std::string e = CxxClassBraceListElement(member.paramTypes[rawIndex]);
                        if (e.empty() || (!classSpelling.empty() && target != classSpelling))
                        {
                            classElement.clear();
                            break;
                        }
                        classElement = e;
                        classSpelling = target;
                    }
                if (!classElement.empty())
                {
                    BackedBrace backed;
                    backed.argumentIndex = i;
                    backed.flatIndex = flatParameterIndex++;
                    backed.element = classElement;
                    const std::string il = "__cflat_il_" + std::to_string(i);
                    wrapperNeedsInitializerList = true;
                    parameterSpellings.push_back("const " + classElement + " *");
                    wrapperBodyPrefix += CxxInitializerListOverStorage(
                        "std::initializer_list<" + classElement + ">", il,
                        "p" + std::to_string(backed.flatIndex), count);
                    // Copy-list-initialized as the list was; a bare `{il}` would also reach the
                    // owner's copy constructor through a nested conversion (ambiguous).
                    callArguments.push_back("[&]() -> " + classSpelling + " { return { " + il
                                            + " }; }()");
                    backedBraces.push_back(std::move(backed));
                    continue;
                }
                std::string braceType = CxxBraceElementSpelling(
                    *brace, rawIndex < selected->paramTypes.size() ? selected->paramTypes[rawIndex]
                                                                      : std::string());
                if (!brace->elements.empty() && braceType.empty())
                {
                    error = "brace arguments must contain scalar values of one type";
                    return false;
                }
                if (rawIndex < selected->paramTypes.size()
                    && braceType == CxxBraceTargetElementSpelling(selected->paramTypes[rawIndex])
                    && !CheckCxxBraceNarrowing(*brace, braceType, DisplayCxxClassName(typeName),
                                               "constructor", error))
                    return false;
                std::string braceCall = "{";
                for (size_t element = 0; element < brace->elements.size(); ++element)
                {
                    if (element != 0) braceCall += ", ";
                    parameterSpellings.push_back(braceType);
                    braceCall += "p" + std::to_string(flatParameterIndex++);
                }
                braceCall += "}";
                callArguments.push_back(std::move(braceCall));
            }
            else
            {
                const size_t rawIndex = i + 1;
                if (!backedList.empty())
                {
                    // The chosen constructor's own parameter (clang's pick among several).
                    const std::string selectedParameter = useSelector
                        ? selectorNs + "::R::p" + std::to_string(i)
                        : selectorCandidates.front().params[i];
                    parameterSpellings.push_back(selectedParameter);
                    const std::string argument = "p" + std::to_string(flatParameterIndex++);
                    callArguments.push_back("static_cast<" + selectedParameter + ">("
                                            + argument + ")");
                    continue;
                }
                if (rawIndex >= selected->paramTypes.size())
                {
                    error = "the C++ constructor has too few parameters for this call";
                    return false;
                }
                parameterSpellings.push_back(selected->paramTypes[rawIndex]);
                callArguments.push_back("p" + std::to_string(flatParameterIndex++));
            }
        }

        std::string targetCall = ownerSpelling + "(";
        for (size_t i = 0; i < callArguments.size(); ++i)
        {
            if (i != 0) targetCall += ", ";
            targetCall += callArguments[i];
        }
        targetCall += ")";
        std::string hashKey = "ctor" + typeName + targetCall;
        for (const auto& param : parameterSpellings) hashKey += param;
        for (const auto& brace : braceArguments)
        {
            hashKey += "|brace|";
            hashKey += std::to_string(brace.parameterIndex);
            hashKey += std::to_string(brace.elements.size());
            hashKey += CxxBraceElementSpelling(brace,
                brace.parameterIndex + 1 < selected->paramTypes.size()
                    ? selected->paramTypes[brace.parameterIndex + 1] : std::string());
        }
        hashKey += selectorSource + wrapperBodyPrefix;
        wrapperName = std::format("__cflat_tpl_{:016x}", HashWrapperKey(hashKey));
        std::string wrapperSource = wrapperNeedsInitializerList
            ? "#include <initializer_list>\n#include <new>\n" : std::string();
        wrapperSource += selectorSource;
        wrapperSource += "extern \"C\" __attribute__((weak)) void " + wrapperName + "(";
        for (size_t i = 0; i < parameterSpellings.size(); ++i)
        {
            if (i != 0) wrapperSource += ", ";
            const std::string name = "p" + std::to_string(i);
            // An array-reference declarator names its parameter inside the parentheses.
            if (const size_t ref = parameterSpellings[i].find("(&)"); ref != std::string::npos)
                wrapperSource += std::string(parameterSpellings[i]).insert(ref + 2, name);
            else
                wrapperSource += parameterSpellings[i] + " " + name;
        }
        wrapperSource += ")";
        if (selected->isNoexcept) wrapperSource += " noexcept";
        wrapperSource += " { " + wrapperBodyPrefix + "new (p0) " + targetCall + "; }\n";

        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        if (groupIt == cxxTypeOwnerGroup_.end())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, typeName);
        if (group.headers.empty())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry bound;
        std::string wrapperError;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName, "BRACE_CTOR",
                                         bound, wrapperError))
        {
            error = std::format("C++ brace constructor call '{}' does not match (clang: {})",
                                typeName, FirstCxxErrorLine(wrapperError));
            return false;
        }
        bound.name = wrapperName;
        RegisterCSignatures({ bound }, selected->file.empty() ? group.headers.front() : selected->file);
        bool registered = false;
        if (auto it = functionTable.find(wrapperName); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                { registered = true; break; }
        if (!registered)
        {
            error = "the generated C++ brace constructor wrapper could not be registered";
            return false;
        }
        for (const BackedBrace& backed : backedBraces)
        {
            auto brace = std::find_if(braceArguments.begin(), braceArguments.end(),
                [&](const CxxBraceArgument& b) { return b.argumentIndex == backed.argumentIndex; });
            TypeAndValue elementType;
            llvm::Type* elementTy = nullptr;
            if (selectorElements.count(backed.argumentIndex) != 0
                && backed.flatIndex < bound.params.size())
            {
                // clang's pick, from the canonical parameter: `const E *` or `E (&)[N]`.
                std::string element;
                std::string extent;
                const std::string spelled = backed.flatIndex < bound.paramSpellings.size()
                    ? bound.paramSpellings[backed.flatIndex] : std::string();
                if (!CxxArrayReferenceParameter(spelled, element, extent))
                {
                    element = spelled;
                    if (element.ends_with("*")) element.pop_back();
                    element = TrimCxxBraceType(element);
                }
                if (MapCTypeToTypeAndValue(element, elementType, true) && !elementType.Pointer)
                    elementTy = GetType(elementType);
            }
            else if (MapCTypeToTypeAndValue(backed.element, elementType, true))
                elementTy = GetType(elementType);
            if (brace == braceArguments.end() || backed.flatIndex >= bound.params.size()
                || elementTy == nullptr
                || (!elementTy->isIntegerTy() && !elementTy->isFloatingPointTy()))
            {
                error = std::format("C++ brace-list element type '{}' has no scalar layout",
                                    backed.element);
                return false;
            }
            NamedVariable data;
            if (!EmitCxxBraceBacking(*brace, elementType, elementTy, bound.params[backed.flatIndex],
                                     DisplayCxxClassName(typeName), "constructor", data, error))
                return false;
            brace->wrapperArguments.clear();
            brace->wrapperArguments.push_back(std::move(data));
        }
        ExpandCxxBraceArguments(arguments, braceArguments);
        return true;
}

// Typed C++ constant text for a compile-time constant scalar brace element, else empty.

/*
 * A brace element that reads an imported enumerator or integer macro constant records the constant, so
 * narrowing reads it as a constant expression (clang does). A CFlat-declared enumerator is not recorded.
 */
void LLVMBackend::RecordCxxConstantBraceElement(NamedVariable& element, llvm::Value* value)
{
        element.CxxConstantValue = nullptr;
        if (element.TypeAndValue.Pointer || element.TypeAndValue.ElemPointer) return;
        // A scoped enumerator converts to no integer: its constant value is not a brace constant.
        if (element.TypeAndValue.IsScopedEnum
            || IsScopedEnumTypeName(ResolveEnumTypeName(element.TypeAndValue.TypeName))) return;
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
        auto* global = load != nullptr ? llvm::dyn_cast<llvm::GlobalVariable>(load->getPointerOperand()) : nullptr;
        if (global == nullptr || !global->hasInitializer()) return;
        const bool cflatEnumerator = CflatEnumeratorConstant(
            global, ResolveEnumTypeName(element.TypeAndValue.TypeName)) != nullptr;
        if (!element.IsCxxMacroGlobal && !IsImportedEnumeratorStorage(global) && !cflatEnumerator) return;
        element.CxxConstantValue = llvm::dyn_cast<llvm::ConstantInt>(global->getInitializer());
}

std::string LLVMBackend::CxxImportedConstantLiteral(const NamedVariable& element) const
{
        // C++ treats an imported enumerator or integer macro as a constant; CFlat keeps its global.
        // The literal keeps the constant's own C++ type, so overloads and narrowing see what clang sees.
        if (element.TypeAndValue.Pointer || element.TypeAndValue.IsScopedEnum || element.Storage == nullptr
            || IsScopedEnumTypeName(ResolveEnumTypeName(element.TypeAndValue.TypeName)))
            return {};
        // A CFlat enumerator is a constant of its backing integer: spelled as that type, value kept.
        const std::string enumKey = ResolveEnumTypeName(element.TypeAndValue.TypeName);
        if (const llvm::ConstantInt* value = CflatEnumeratorConstant(element.Storage, enumKey))
        {
            std::string backing;
            if (!CxxCflatEnumBackingSpelling(enumKey, backing) || value->getBitWidth() > 64) return {};
            TypeAndValue probe;
            probe.TypeName = GetEnumBackingType(enumKey);
            const std::string literal = probe.IsUnsignedInteger() > 0
                ? std::to_string(value->getZExtValue()) : std::to_string(value->getSExtValue());
            return "(" + backing + ")" + literal;
        }
        if (!element.IsCxxMacroGlobal && !IsImportedEnumeratorStorage(element.Storage)) return {};
        if (IsImportedEnumeratorStorage(element.Storage))
        {
            // An unscoped enumerator is spelled as its qualified name: its C++ type is the
            // enumeration, not the backing integer. The global is named `ns.A` (`A` at file scope).
            const std::string name = element.Storage->getName().str();
            const std::string enumType = element.TypeAndValue.TypeName;
            const size_t enumDot = enumType.rfind('.');
            const size_t nameDot = name.rfind('.');
            const std::string enumScope = enumDot == std::string::npos ? std::string() : enumType.substr(0, enumDot);
            const std::string nameScope = nameDot == std::string::npos ? std::string() : name.substr(0, nameDot);
            // `ns.A.e` is the enumerator of enum `ns.A` (spelled `ns::A::e`, valid for an unscoped enum);
            // `ns.e` is one declared directly in the namespace.
            if (enumScope != nameScope && enumType != nameScope) return {};
            std::string qualified;
            for (char c : name)
            {
                if (c == '.') qualified += "::";
                else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') qualified += c;
                else return {};
            }
            return qualified;
        }
        auto* global = llvm::dyn_cast<llvm::GlobalVariable>(element.Storage);
        if (global == nullptr || !global->hasInitializer()) return {};
        auto* value = llvm::dyn_cast<llvm::ConstantInt>(global->getInitializer());
        if (value == nullptr || value->getBitWidth() > 64 || value->getValue().isMinSignedValue())
            return {};
        // Signedness and width come from the imported TYPE, not the APInt's high bit: an unsigned
        // 4294967295U is not -1, and an unsigned ULLONG_MAX stays 2^64-1.
        const TypeAndValue& tv = element.TypeAndValue;
        const bool isUnsigned = tv.IsUnsignedInteger() != -1;
        if (!isUnsigned && tv.IsInteger() == -1) return {};
        std::string spelled;
        if (!CxxSpellingForCflatType(tv.TypeName, spelled)) return {};
        const bool negative = !isUnsigned && value->getValue().isNegative();
        const std::string digits = isUnsigned ? std::to_string(value->getZExtValue())
                                              : std::to_string(negative ? -value->getSExtValue() : value->getSExtValue());
        if (spelled == "int")
            return negative ? "(-" + digits + ")" : digits;
        std::string suffix;
        if (spelled == "unsigned int") suffix = "U";
        else if (spelled == "long") suffix = "L";
        else if (spelled == "unsigned long") suffix = "UL";
        else if (spelled == "long long") suffix = "LL";
        else if (spelled == "unsigned long long") suffix = "ULL";
        return "static_cast<" + spelled + ">(" + (negative ? "(-" + digits + suffix + ")" : digits + suffix) + ")";
}

static bool CxxHeaderHasIncludeGuard(const std::string& contents)
{
        if (contents.find("#pragma once") != std::string::npos) return true;
        std::vector<std::string> directives;
        bool inBlockComment = false;
        size_t offset = 0;
        while (offset <= contents.size() && directives.size() < 2)
        {
            const size_t end = contents.find('\n', offset);
            const std::string_view line(contents.data() + offset,
                (end == std::string::npos ? contents.size() : end) - offset);
            offset = end == std::string::npos ? contents.size() + 1 : end + 1;
            std::string visible;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (inBlockComment)
                {
                    const size_t close = line.find("*/", i);
                    if (close == std::string_view::npos) break;
                    i = close + 1;
                    inBlockComment = false;
                    continue;
                }
                if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') break;
                if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*')
                {
                    inBlockComment = true;
                    ++i;
                    continue;
                }
                visible += line[i];
            }
            const size_t first = visible.find_first_not_of(" \t\r");
            if (first == std::string::npos) continue;
            visible.erase(0, first);
            if (visible.front() != '#') return false;
            directives.push_back(std::move(visible));
        }
        if (directives.size() != 2) return false;
        auto parseDirective = [](const std::string& line) {
            std::pair<std::string, std::string> parsed;
            size_t i = 1;
            auto skipSpace = [&] {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            };
            auto readToken = [&] {
                const size_t start = i;
                while (i < line.size()
                    && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_'))
                    ++i;
                return line.substr(start, i - start);
            };
            skipSpace();
            parsed.first = readToken();
            skipSpace();
            parsed.second = readToken();
            return parsed;
        };
        const auto first = parseDirective(directives[0]);
        const auto second = parseDirective(directives[1]);
        return first.first == "ifndef" && second.first == "define"
            && !first.second.empty() && first.second == second.second;
}





bool LLVMBackend::CxxRecordFieldTypeSpelling(const std::string& typeName,
                                              const std::string& fieldName,
                                              std::string& spelling,
                                              bool* isReference) const
{
        spelling.clear();
        if (isReference != nullptr) *isReference = false;
        const std::string resolvedType = ResolveTypeAlias(typeName);
        auto record = cxxRecordEntries_.find(resolvedType);
        if (record == cxxRecordEntries_.end()) return false;
        auto field = std::find_if(record->second.fields.begin(), record->second.fields.end(),
            [&](const CRecordFieldEntry& candidate) { return candidate.name == fieldName; });
        if (field == record->second.fields.end()) return false;
        spelling = field->ctype;
        if (isReference != nullptr) *isReference = CxxBraceFieldIsReference(spelling);
        return !spelling.empty();
}

size_t LLVMBackend::CxxAggregateBaseCount(const std::string& typeName) const
{
        const std::string resolvedType = ResolveTypeAlias(typeName);
        auto record = cxxRecordEntries_.find(resolvedType);
        return record == cxxRecordEntries_.end() ? 0 : record->second.bases.size();
}

std::string LLVMBackend::CxxAggregateBaseTypeAt(const std::string& typeName, size_t index) const
{
        const std::string resolvedType = ResolveTypeAlias(typeName);
        auto record = cxxRecordEntries_.find(resolvedType);
        if (record == cxxRecordEntries_.end() || index >= record->second.bases.size())
            return {};
        const auto& base = record->second.bases[index];
        if (base.access != cflat_cinterop::AccessPublic || base.isVirtual) return {};
        return ResolveCxxBaseRecordName(base);
}

size_t LLVMBackend::CxxAggregateBaseFieldCountAt(const std::string& typeName,
                                                  size_t index) const
{
        const std::string resolvedType = ResolveTypeAlias(typeName);
        auto record = cxxRecordEntries_.find(resolvedType);
        if (record == cxxRecordEntries_.end() || index >= record->second.bases.size()) return 0;
        const auto& base = record->second.bases[index];
        if (base.access != cflat_cinterop::AccessPublic || base.isVirtual) return 0;
        const std::string baseType = ResolveCxxBaseRecordName(base);
        auto baseRecord = cxxRecordEntries_.find(baseType);
        return baseRecord == cxxRecordEntries_.end() ? 0 : baseRecord->second.fields.size();
}

std::string LLVMBackend::CxxAggregateFlattenedFieldNameAt(const std::string& typeName,
                                                          size_t index) const
{
        const std::string resolvedType = ResolveTypeAlias(typeName);
        auto record = cxxRecordEntries_.find(resolvedType);
        if (record == cxxRecordEntries_.end() || index >= record->second.fields.size()) return {};
        return record->second.fields[index].name;
}

bool LLVMBackend::RequestCxxDirectBraceConstructor(
        const std::string& typeName, const std::vector<NamedVariable>& elements,
        const std::string& initializerList, bool copyListInit, std::string& wrapperName,
        std::string& error, bool allowIncremental,
        std::vector<size_t>* forwardedElementIndices,
        const std::vector<std::string>* elementTargetTypes)
{
        error.clear();
        wrapperName.clear();
        if (forwardedElementIndices != nullptr) forwardedElementIndices->clear();
        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(typeName, ownerSpelling))
        {
            error = "the C++ class type is not registered";
            return false;
        }
        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        if (groupIt == cxxTypeOwnerGroup_.end())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, typeName);
        if (group.headers.empty())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        // Incremental requests include the group's headers into one live clang TU. Guardless
        // user headers must use standalone requests or a later empty-brace spelling redefines them.
        const bool headersGuarded = std::all_of(group.ownerHeaders.begin(), group.ownerHeaders.end(),
            [this](const std::string& header) {
                if (IsSystemCxxHeaderPath(header)) return true;
                const std::string cacheKey = std::filesystem::path(header).lexically_normal().string();
                auto cached = cxxHeaderGuardCache_.find(cacheKey);
                if (cached != cxxHeaderGuardCache_.end()) return cached->second;
                std::ifstream in(header, std::ios::binary);
                bool guarded = false;
                if (in)
                {
                    const std::string contents((std::istreambuf_iterator<char>(in)),
                                               std::istreambuf_iterator<char>());
                    guarded = CxxHeaderHasIncludeGuard(contents);
                }
                cxxHeaderGuardCache_.emplace(cacheKey, guarded);
                return guarded;
            });
        allowIncremental = allowIncremental && headersGuarded;
        std::vector<std::string> parameterTypes{ ownerSpelling + " *" };
        std::vector<std::string> elementParameters(elements.size());
        std::vector<size_t> elementParameterTypes(elements.size(), 0);
        std::vector<bool> elementIsFunctionReference(elements.size(), false);
        std::string init = initializerList;
        size_t nextParameter = 1;
        auto isBareNullptrLiteral = [](const NamedVariable& element) {
            const auto& type = element.TypeAndValue;
            return type.TypeName.empty() && !type.Pointer && !type.IsCxxNullptrT
                && !type.IsFunctionPointer && element.LiteralIdentity.empty()
                && element.Storage == nullptr
                && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(element.Primary);
        };
        for (size_t i = 0; i < elements.size(); ++i)
        {
            std::string spelling;
            const auto& type = elements[i].TypeAndValue;
            const std::string targetType = elementTargetTypes != nullptr
                && i < elementTargetTypes->size() ? (*elementTargetTypes)[i] : std::string();
            if (elements[i].IsDefaultArgument)
            {
                if (type.TypeName.empty() || !CxxSpellingForCflatType(type.TypeName, spelling))
                {
                    error = std::format("C++ brace-list element type '{}' has no C++ spelling",
                                        type.TypeName);
                    return false;
                }
                if (type.Pointer && spelling.find('*') == std::string::npos) spelling += " *";
                if (type.Pointer || type.IsAlias || type.IsRvalueRef
                    || !IsCxxRecord(type.TypeName))
                {
                    error = "a deferred C++ brace default requires a by-value class target";
                    return false;
                }
                const std::string marker = "__cflat_arg_" + std::to_string(i) + "__";
                size_t at = 0;
                const std::string replacement = spelling + "{}";
                while ((at = init.find(marker, at)) != std::string::npos)
                {
                    init.replace(at, marker.size(), replacement);
                    at += replacement.size();
                }
                continue;
            }
            if (isBareNullptrLiteral(elements[i]))
            {
                const std::string marker = "__cflat_arg_" + std::to_string(i) + "__";
                size_t at = 0;
                while ((at = init.find(marker, at)) != std::string::npos)
                {
                    init.replace(at, marker.size(), "nullptr");
                    at += sizeof("nullptr") - 1;
                }
                continue;
            }
            const bool targetIsReference = CxxBraceFieldIsReference(targetType);
            if (targetIsReference)
            {
                elementIsFunctionReference[i] = CxxBraceFieldIsFunctionReference(targetType);
                if (elementIsFunctionReference[i]
                    && !CxxThinFunctionPointerSpelling(type, spelling))
                {
                    // An existing C++ function reference (no CFlat spelling): bind by reference, clang checks it.
                    elementIsFunctionReference[i] = false;
                    spelling = targetType;
                }
                else if (elementIsFunctionReference[i])
                {
                    if (!IsCxxFunctionDesignatorArgument(elements[i])
                        || elements[i].Storage != nullptr)
                    {
                        error = "cannot bind a C++ function reference to a function pointer value; use a named function";
                        return false;
                    }
                }
                else
                    spelling = targetType;
            }
            else if (type.TypeName.empty() || !CxxSpellingForCflatType(type.TypeName, spelling))
            {
                error = std::format("C++ brace-list element type '{}' has no C++ spelling",
                                    type.TypeName);
                return false;
            }
            if (!targetIsReference && type.Pointer && spelling.find('*') == std::string::npos)
                spelling += " *";
            if (targetIsReference)
            {
                elementParameters[i] = "p" + std::to_string(nextParameter++);
                elementParameterTypes[i] = parameterTypes.size();
                parameterTypes.push_back(std::move(spelling));
                if (forwardedElementIndices != nullptr) forwardedElementIndices->push_back(i);
                continue;
            }
            const bool pointerValue = type.Pointer
                && (!type.IsAlias || type.IsCxxRefToPointer) && !type.IsFunctionPointer;
            const bool pointerXvalue = pointerValue && IsCxxRvalueReferenceArgument(elements[i]);
            if (pointerValue)
            {
                if ((type.PointeeConstBit() || (type.PointeeConstMask() & 1u) != 0)
                    && !spelling.starts_with("const "))
                    spelling = "const " + spelling;
                if (type.IsCxxPointeeVolatile) PrependCv(spelling, "volatile ");
                if (type.IsCxxRefToPointer && type.CxxConstRef() && !pointerXvalue)
                    spelling += " const";
                spelling += pointerXvalue ? " &&" : " &";
            }
            // A class element binds by reference: clang copies an lvalue, moves an rvalue.
            else if (!type.Pointer && IsCxxRecord(type.TypeName))
            {
                const bool xvalue = elements[i].IsExplicitMove
                    || IsCxxRvalueReferenceArgument(elements[i]);
                const bool isConst = CxxConstReceiverKind(elements[i]) != 0;
                spelling += xvalue ? " &&" : isConst ? " const &" : " &";
            }
            elementParameters[i] = "p" + std::to_string(nextParameter++);
            elementParameterTypes[i] = parameterTypes.size();
            parameterTypes.push_back(std::move(spelling));
            if (forwardedElementIndices != nullptr) forwardedElementIndices->push_back(i);
        }
        for (size_t i = 0; i < elements.size(); ++i)
        {
            if (elements[i].IsDefaultArgument || isBareNullptrLiteral(elements[i])) continue;
            const std::string marker = "__cflat_arg_" + std::to_string(i) + "__";
            const std::string& param = elementParameters[i];
            const std::string targetType = elementTargetTypes != nullptr
                && i < elementTargetTypes->size() ? (*elementTargetTypes)[i] : std::string();
            if (CxxBraceFieldIsReference(targetType))
            {
                std::string replacement = param;
                if (elementIsFunctionReference[i])
                    replacement = "(*" + param + ")";
                else if (CxxBraceFieldIsRvalueReference(targetType))
                {
                    const size_t last = targetType.find_last_not_of(" \t\r\n");
                    const size_t first = last != std::string::npos && last > 0
                        ? targetType.find_last_not_of(" \t\r\n", last - 1) : std::string::npos;
                    if (last != std::string::npos && first != std::string::npos
                        && targetType[last] == '&' && targetType[first] == '&')
                    {
                        std::string castType = targetType.substr(0, first);
                        while (!castType.empty()
                            && (castType.back() == ' ' || castType.back() == '\t'
                                || castType.back() == '\r' || castType.back() == '\n'))
                            castType.pop_back();
                        replacement = "static_cast<" + castType + " &&>(" + param + ")";
                    }
                    else
                        replacement = "static_cast<decltype(" + param + ")>(" + param + ")";
                }
                size_t at = 0;
                while ((at = init.find(marker, at)) != std::string::npos)
                {
                    init.replace(at, marker.size(), replacement);
                    at += replacement.size();
                }
                continue;
            }
            const auto& type = elements[i].TypeAndValue;
            const bool pointerValue = type.Pointer
                && (!type.IsAlias || type.IsCxxRefToPointer) && !type.IsFunctionPointer;
            const bool pointerXvalue = pointerValue && IsCxxRvalueReferenceArgument(elements[i]);
            if (!elements[i].TypeAndValue.Pointer && IsCxxRecord(elements[i].TypeAndValue.TypeName))
            {
                const std::string replacement = elements[i].IsRvalue || elements[i].IsExplicitMove
                    ? "std::move(" + param + ")" : param;
                size_t at = 0;
                while ((at = init.find(marker, at)) != std::string::npos)
                {
                    init.replace(at, marker.size(), replacement);
                    at += replacement.size();
                }
                continue;
            }
            // A compile-time value element goes in as a typed constant so clang applies list
            // narrowing rules. Reference elements above must use the caller-owned parameter.
            std::string constant = elements[i].CxxImportedLiteral;
            if (constant.empty())
                constant = DirectBraceConstantText(elements[i], parameterTypes[elementParameterTypes[i]],
                    IsNullPointerConstantArgument(elements[i], true));
            if (!constant.empty())
            {
                size_t at = 0;
                while ((at = init.find(marker, at)) != std::string::npos)
                {
                    init.replace(at, marker.size(), constant);
                    at += constant.size();
                }
                continue;
            }
            size_t at = 0;
            std::string replacement = param;
            if (pointerXvalue)
            {
                std::string castType = parameterTypes[i + 1];
                castType.resize(castType.size() - 3);
                replacement = "static_cast<" + castType + " &&>(" + param + ")";
            }
            while ((at = init.find(marker, at)) != std::string::npos)
            {
                init.replace(at, marker.size(), replacement);
                at += replacement.size();
            }
        }
        // `T v = {..}` is copy-list-init: a never-called lambda makes clang refuse an explicit
        // selected constructor there, while the placement new below stays direct-list-init.
        const std::string copyCheck = copyListInit
            ? "(void)[&]() -> " + ownerSpelling + " { return " + init + "; }; " : std::string();
        const std::string key = typeName + "|direct-brace|"
            + (copyListInit ? "copy|" : "direct|") + init
            + [&] { std::string k; for (const auto& p : parameterTypes) k += "|" + p; return k; }();
        wrapperName = std::format("__cflat_dbrace_{:016x}", HashWrapperKey(key));
        std::string source = "#include <new>\n#include <utility>\n"
            "#pragma clang diagnostic error \"-Wc++11-narrowing\"\n"
            "extern \"C\" void "
            + wrapperName + "(";
        for (size_t i = 0; i < parameterTypes.size(); ++i)
        {
            if (i != 0) source += ", ";
            source += CxxNamedParameter(parameterTypes[i], "p" + std::to_string(i));
        }
        // `::new`: a class-scope operator new would hide the global placement form.
        source += ") { " + copyCheck + "::new ((void*)p0) " + ownerSpelling + init
            + "; }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry bound;
        std::string wrapperError;
        if (!RequestGeneratedCxxWrapper(group, source, wrapperName, "DIRECT_BRACE_CTOR",
                                        bound, wrapperError, /*persistOnSuccess*/ true,
                                        allowIncremental))
        {
            error = std::format("C++ brace constructor call '{}' does not match (clang: {})",
                                DisplayCxxClassName(typeName), FirstCxxErrorLine(wrapperError));
            return false;
        }
        bound.name = wrapperName;
        RegisterCSignatures({ bound }, group.headers.front());
        bool registered = false;
        if (auto it = functionTable.find(wrapperName); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                { registered = true; break; }
        if (!registered)
        {
            error = "the generated C++ direct-brace constructor wrapper could not be registered";
            return false;
        }
        return true;
}

bool LLVMBackend::CxxCtorForwardsScalarReference(const std::string& typeName,
                                                 const std::vector<TypeAndValue>& argTypes) const
{
        const CxxClassInfo* info = GetCxxClassInfoWithConstructors(typeName);
        if (info == nullptr || argTypes.empty()) return false;
        auto scalar = [&](const TypeAndValue& t) {
            return !t.ElemPointer && !IsCxxRecord(t.TypeName) && !t.IsScopedEnum
                && (t.IsInteger() != -1 || t.IsFloatingPoint() != -1);
        };
        for (const auto& c : info->constructors)
        {
            if (c.params.size() < argTypes.size() + 1) continue;
            for (size_t i = 0; i < argTypes.size(); ++i)
            {
                const TypeAndValue& want = c.params[i + 1];
                const TypeAndValue& got = argTypes[i];
                if (got.Pointer || !scalar(got)) continue;
                if (want.Pointer && !want.IsCxxRefToPointer
                    && (want.IsAlias || want.IsRvalueRef || want.CxxConstRef()) && scalar(want))
                    return true;
            }
        }
        return false;
}

/*
 * Ruling 2026-09-04: no implicit narrowing at a call argument (int -> bool the one exception), also
 * where clang accepts. A clang-resolved constructor call is judged on the constructor C++ selects:
 * the listed candidates are ranked by [over.match.best] (the free-function ranking), and when one
 * wins outright, each scalar argument's conversion into its parameter (for a const T& parameter,
 * the materialized temporary's) goes through the scalar table. Empty when nothing narrows or the
 * ranking cannot decide (a template, variadic, inherited or unranked candidate, a tie); clang then
 * resolves and CxxCtorSetRequiresNarrowing stays the guard.
 */
std::string LLVMBackend::CxxCtorSelectedNarrowingError(const std::string& typeName,
                                                       const std::vector<NamedVariable>& argVars,
                                                       bool floatToIntegerOnly)
{
        if (argVars.empty()) return {};
        const CxxClassInfo* info = GetCxxClassInfoWithConstructors(typeName);
        if (info == nullptr || info->hasCtorTemplate) return {};
        const auto raw = cxxRecordEntries_.find(typeName);
        if (raw == cxxRecordEntries_.end() || !raw->second.ctorTemplates.empty()) return {};
        for (const auto& member : raw->second.members)
            if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                && (member.variadic || member.requiresConstructorWrapper))
                return {};
        const auto isPointerArg = [](const NamedVariable& arg) {
            return arg.TypeAndValue.Pointer || arg.TypeAndValue.ElemPointer || arg.IsStringLiteral;
        };
        // The value a scalar reference parameter binds, `const T&` included.
        const auto referent = [](TypeAndValue param) {
            if (param.Pointer && !param.ElemPointer && !param.IsCxxRefToPointer && param.CxxConstRef())
                param.IsAlias = true;
            return ScalarArgumentReferent(param);
        };
        struct Ranked
        {
            const CxxClassInfo::Structor* ctor = nullptr;
            std::vector<CxxConversionRank> ranks;
        };
        std::vector<Ranked> viable;
        for (const CxxClassInfo::Structor& ctor : info->constructors)
        {
            // A copy/move constructor takes a scalar only through a user-defined conversion,
            // which never outranks a standard one. A deleted candidate still competes.
            if (ctor.params.empty() || ctor.isCopyCtor || ctor.isMoveCtor) continue;
            const size_t paramCount = ctor.params.size() - 1;
            if (argVars.size() > paramCount) continue;
            bool fits = true;
            for (size_t i = argVars.size(); fits && i < paramCount; ++i)
                fits = i + 1 < ctor.defaultArgs.size() && !ctor.defaultArgs[i + 1].kind.empty();
            for (size_t i = 0; fits && i < argVars.size(); ++i)
            {
                const bool pointerParam = referent(ctor.params[i + 1]).Pointer;
                fits = pointerParam == isPointerArg(argVars[i])
                    || (pointerParam && IsNullPointerConstantArgument(argVars[i], true));
            }
            if (!fits) continue;
            FunctionSymbol sym;
            sym.IsCxx = true;
            sym.IsMethod = true;
            sym.UniqueName = ctor.linkageName;
            sym.Parameters = ctor.params;
            sym.Parameters[0].VariableName = "this";
            sym.Parameters[0].TypeName = typeName;
            // A scalar `const T&` binds the temporary the argument converts into: its sequence
            // ranks as that conversion ([over.ics.ref]), so the by-value referent stands in.
            for (size_t i = 1; i < sym.Parameters.size(); ++i)
            {
                const TypeAndValue& p = sym.Parameters[i];
                if (!p.Pointer || p.ElemPointer || p.IsCxxRefToPointer || !p.CxxConstRef()
                    || p.IsRvalueRef)
                    continue;
                TypeAndValue value = referent(p);
                if (!value.Pointer && (value.IsInteger() != -1 || value.IsFloatingPoint() != -1
                                       || value.TypeName == "bool"))
                    sym.Parameters[i] = std::move(value);
            }
            std::vector<NamedVariable> args(1);
            args[0].TypeAndValue.TypeName = typeName;
            args.insert(args.end(), argVars.begin(), argVars.end());
            std::vector<CxxConversionRank> ranks = RankCxxConversionSequences(args, sym);
            if (ranks.empty()) return {};
            ranks.erase(ranks.begin());   // the implicit object is common to every candidate
            if (ranks.size() != argVars.size()) return {};
            for (const CxxConversionRank& r : ranks)
                if (r.rank < 0 || r.rank > 2) return {};
            viable.push_back({ &ctor, std::move(ranks) });
        }
        const Ranked* best = nullptr;
        for (const Ranked& x : viable)
        {
            bool wins = true;
            for (const Ranked& y : viable)
            {
                bool crossing = false;
                if (&x != &y && CompareCxxConversionRanks(x.ranks, y.ranks, crossing) != -1)
                    wins = false;
            }
            if (wins) { best = &x; break; }
        }
        if (best == nullptr || best->ctor->isDeleted) return {};
        for (size_t i = 0; i < argVars.size(); ++i)
        {
            const TypeAndValue param = referent(best->ctor->params[i + 1]);
            if (floatToIntegerOnly)
            {
                const bool floatingSource = argVars[i].TypeAndValue.IsFloatingPoint() != -1
                    || (argVars[i].BaseType != nullptr && argVars[i].BaseType->isFloatingPointTy());
                if (!floatingSource || param.TypeName == "bool" || param.IsInteger() == -1)
                    continue;
            }
            const ScalarArgConversion conv = ClassifyScalarArgConversion(argVars[i], param);
            if (conv != ScalarArgConversion::Narrow && conv != ScalarArgConversion::EnumMismatch)
                continue;
            const std::string from = !best->ranks[i].from.empty() ? best->ranks[i].from
                : !argVars[i].TypeAndValue.TypeName.empty() ? argVars[i].TypeAndValue.TypeName
                : argVars[i].LiteralIdentity;
            return std::format("constructor that C++ selects for these arguments narrows argument {} "
                               "from '{}' to '{}' (no implicit narrowing at a call); cast it explicitly",
                               i + 1, from, param.TypeName);
        }
        return {};
}

bool LLVMBackend::HasHiddenScalarCxxConstructor(const std::string& typeName,
        const std::vector<TypeAndValue>& argTypes) const
{
        bool anyScalar = false;
        for (const TypeAndValue& tv : argTypes)
            anyScalar = anyScalar || (!tv.Pointer && (tv.IsInteger() != -1
                || tv.IsFloatingPoint() != -1 || tv.TypeName == "bool"));
        if (!anyScalar) return false;
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return false;
        // A tie needs a visible constructor to tie with: a class whose only fitting constructor is
        // deleted keeps CFlat's own "no constructor" refusal.
        auto fitsArity = [&](const cflat_cinterop::RawCxxMember& member) {
            return member.kind == cflat_cinterop::RawCxxMember::Constructor
                && member.name == "__ctor" && !member.isCopyCtor && !member.isMoveCtor
                && member.paramTypes.size() >= argTypes.size() + 1;
        };
        bool hidden = false;
        bool visible = false;
        for (const auto& member : record->second.members)
        {
            if (!fitsArity(member)) continue;
            // Protected is hidden too: a CFlat call site is never inside a derived class.
            if (member.isDeleted || member.access != cflat_cinterop::AccessPublic) hidden = true;
            else visible = true;
        }
        return hidden && visible;
}

bool LLVMBackend::CxxConstructorNeedsClangResolution(
        const std::string& typeName, const CxxClassInfo::Structor* selected,
        const std::vector<TypeAndValue>& argTypes,
        const std::vector<NamedVariable>* argVars, bool allowExplicit,
        bool* nullPointerCandidate) const
{
        if (nullPointerCandidate != nullptr) *nullPointerCandidate = false;
        // An unnamed enum constant reaches here as int: only clang ranks the enum constructor template.
        if (argVars != nullptr
            && std::any_of(argVars->begin(), argVars->end(), [&](const NamedVariable& arg) {
                return CxxUnnamedEnumTemplateArgument(typeName, arg);
            }))
            return true;
        const bool addressValue = argVars != nullptr
            && std::any_of(argVars->begin(), argVars->end(), [](const NamedVariable& arg) {
                return arg.IsCxxAddressOfPointerValue && arg.Storage == nullptr;
            });
        if (!addressValue && CxxCtorForwardsScalarReference(typeName, argTypes)) return true;
        if (argVars != nullptr && argVars->size() == argTypes.size())
            if (const CxxClassInfo* listed = GetCxxClassInfoWithConstructors(typeName);
                listed != nullptr)
            {
                if (addressValue)
                {
                    auto raw = cxxRecordEntries_.find(typeName);
                    if (raw != cxxRecordEntries_.end()
                        && !raw->second.ctorTemplates.empty())
                    {
                        for (const cflat_cinterop::RawCxxCtorTemplate& ctorTemplate
                             : raw->second.ctorTemplates)
                        {
                            const size_t arity = argVars->size();
                            size_t packIndex = ctorTemplate.paramTypes.size();
                            if (ctorTemplate.hasParameterPack)
                                for (size_t i = 0; i < ctorTemplate.paramTypes.size(); ++i)
                                    if (ctorTemplate.paramTypes[i].ends_with("..."))
                                    { packIndex = i; break; }
                            const bool trailingPack = ctorTemplate.hasParameterPack
                                && packIndex + 1 == ctorTemplate.paramTypes.size();
                            const size_t fixedCount = ctorTemplate.hasParameterPack
                                && packIndex < ctorTemplate.paramTypes.size()
                                ? ctorTemplate.paramTypes.size() - 1
                                : ctorTemplate.paramTypes.size();
                            size_t required = fixedCount;
                            for (size_t i = 0; i < fixedCount; ++i)
                                if (i < ctorTemplate.defaulted.size() && ctorTemplate.defaulted[i])
                                { required = i; break; }
                            bool viable = arity >= required
                                && (trailingPack || ctorTemplate.variadic || arity <= fixedCount);
                            for (size_t i = arity; viable
                                 && i < fixedCount; ++i)
                                viable = i < ctorTemplate.defaulted.size()
                                    && ctorTemplate.defaulted[i] != 0;
                            bool acceptsAddressValue = true;
                            for (size_t i = 0; i < argVars->size(); ++i)
                            {
                                if (!(*argVars)[i].IsCxxAddressOfPointerValue
                                    || (*argVars)[i].Storage != nullptr)
                                    continue;
                                if (i >= ctorTemplate.paramTypes.size())
                                {
                                    if (!ctorTemplate.variadic && !ctorTemplate.hasParameterPack)
                                        acceptsAddressValue = false;
                                    continue;
                                }
                                const size_t parameterIndex = trailingPack && i >= packIndex
                                    ? packIndex : i;
                                const std::string& param = ctorTemplate.paramTypes[parameterIndex];
                                const size_t ref = param.find('&');
                                size_t afterRef = ref == std::string::npos ? ref : ref + 1;
                                while (afterRef != std::string::npos && afterRef < param.size()
                                       && std::isspace(static_cast<unsigned char>(param[afterRef])))
                                    ++afterRef;
                                if (ref != std::string::npos
                                    && (afterRef == param.size() || param[afterRef] != '&')
                                    && param.find("const") == std::string::npos)
                                    acceptsAddressValue = false;
                            }
                            if (viable && acceptsAddressValue) return true;
                        }
                    }
                    else if (listed->hasCtorTemplate)
                    {
                        // The template's arity is unavailable, so keep clang as the oracle.
                        return true;
                    }
                    auto* self = const_cast<LLVMBackend*>(this);
                    for (const CxxClassInfo::Structor& ctor : listed->constructors)
                    {
                        if (ctor.params.empty() || argVars->size() > ctor.params.size() - 1)
                            continue;
                        bool defaults = true;
                        for (size_t i = argVars->size(); defaults && i < ctor.params.size() - 1; ++i)
                            defaults = i + 1 < ctor.defaultArgs.size()
                                && !ctor.defaultArgs[i + 1].kind.empty();
                        if (!defaults) continue;
                        FunctionSymbol candidate;
                        candidate.IsCxx = true;
                        candidate.IsMethod = true;
                        candidate.UniqueName = ctor.linkageName;
                        candidate.Parameters = ctor.params;
                        candidate.Parameters[0].VariableName = "this";
                        candidate.Parameters[0].TypeName = typeName;
                        std::vector<NamedVariable> arguments(1);
                        arguments[0].TypeAndValue.TypeName = typeName;
                        arguments.insert(arguments.end(), argVars->begin(), argVars->end());
                        auto ranks = self->RankCxxConversionSequences(arguments, candidate);
                        if (ranks.size() != arguments.size()) continue;
                        bool viable = true;
                        bool readsAddressValue = false;
                        for (size_t i = 0; i < argVars->size(); ++i)
                        {
                            // Every argument must be viable, not only the address-of ones (Multi(&p, 7)).
                            viable = viable && ranks[i + 1].cxxViable;
                            if (!(*argVars)[i].IsCxxAddressOfPointerValue
                                || (*argVars)[i].Storage != nullptr)
                                continue;
                            const TypeAndValue& param = ctor.params[i + 1];
                            readsAddressValue = readsAddressValue
                                || !param.IsCxxRefToPointer || param.CxxConstRef()
                                || param.IsRvalueRef;
                        }
                        if (viable && readsAddressValue) return true;
                    }
                }
            }
        if (addressValue) return false;
        if (selected != nullptr && !selected->isCopyCtor && !selected->isMoveCtor
            && selected->params.size() >= argTypes.size() + 1)
            for (size_t i = 0; i < argTypes.size(); ++i)
            {
                const TypeAndValue& param = selected->params[i + 1];
                // Only a reference TO A POINTER needs the slot forwarded through clang; a record
                // or scalar reference keeps the direct ABI ctor (T131 r3).
                if (param.IsCxxRefToPointer && !param.IsFunctionPointer)
                    return true;
            }
        // A deleted/private/protected ctor is dropped at harvest yet still ranks in a C++ tie, so
        // such a call goes to clang, which decides the tie and the access/deleted refusal.
        if (selected != nullptr && HasHiddenScalarCxxConstructor(typeName, argTypes))
        {
            // Clang-only set, like a null-pointer tie: its refusal must reach the caller.
            if (nullPointerCandidate != nullptr) *nullPointerCandidate = true;
            return true;
        }
        // CFlat drops a narrowing sibling C++ still ranks (SCL(char) beside SCL(long), int 5).
        // Only a widening pick can tie with it, so only that pick is checked here.
        if (selected != nullptr && argVars != nullptr && argVars->size() == argTypes.size()
            && selected->params.size() >= argTypes.size() + 1)
        {
            // An unsuffixed literal lowers at its smallest width; its C++ type is LiteralIdentity.
            std::vector<NamedVariable> cxxArgs = *argVars;
            for (NamedVariable& arg : cxxArgs)
                if (arg.TypeAndValue.TypeName.empty() && !arg.LiteralIdentity.empty())
                    arg.TypeAndValue.TypeName = arg.LiteralIdentity;
            // Exact by width is still a conversion when C++ `long` (or `ulong`) meets another
            // integer type: LLP64 Windows makes long 32 bits, LP64 macOS makes long long 64 bits.
            // Likewise short/unsigned short, int/unsigned int and an unscoped enum beside an
            // integer: a C++ conversion of equal rank, which the narrowing sibling ties with.
            auto sameWidthIdentityDiffers = [&](const NamedVariable& arg, size_t index) {
                const TypeAndValue& want = selected->params[index];
                if (!arg.TypeAndValue.IsScopedEnum && !ResolveEnumTypeName(arg.TypeAndValue.TypeName).empty())
                    return ResolveEnumTypeName(want.TypeName).empty();
                FunctionSymbol symbol;
                symbol.IsCxx = true;
                symbol.IsMethod = true;
                symbol.UniqueName = selected->linkageName;
                symbol.Parameters = selected->params;
                // A literal's lowered width is not its C++ type (`5` is int even when it lowers as i8).
                const std::string have = !arg.LiteralIdentity.empty() ? arg.LiteralIdentity
                                                                       : IntegerArgumentIdentity(arg, true);
                const std::string wanted = ResolveTypeAlias(CxxIntegerParameterIdentity(symbol, index, want));
                return !have.empty() && !wanted.empty()
                    && CanonicalPrimitiveTypeName(have) != CanonicalPrimitiveTypeName(wanted);
            };
            bool selectedWidens = false;
            for (size_t i = 0; i < cxxArgs.size(); ++i)
            {
                const TypeAndValue& want = selected->params[i + 1];
                const std::string& have = cxxArgs[i].TypeAndValue.TypeName;
                const ScalarArgConversion conv = ClassifyScalarArgConversion(cxxArgs[i], want);
                const bool longMeetsOther = !have.empty() && have != want.TypeName
                    && (have == "long" || have == "ulong" || want.TypeName == "long"
                        || want.TypeName == "ulong");
                selectedWidens |= conv == ScalarArgConversion::Widen
                    || (conv == ScalarArgConversion::Exact
                        && (longMeetsOther || sameWidthIdentityDiffers(cxxArgs[i], i + 1)));
            }
            const CxxClassInfo* listed = selectedWidens ? GetCxxClassInfoWithConstructors(typeName)
                                                        : nullptr;
            if (listed != nullptr)
                for (const CxxClassInfo::Structor& candidate : listed->constructors)
                {
                    if (candidate.params.empty() || candidate.linkageName == selected->linkageName
                        || (!allowExplicit && candidate.isExplicit)
                        || cxxArgs.size() > candidate.params.size() - 1)
                        continue;
                    for (size_t i = 0; i < cxxArgs.size(); ++i)
                        if (ClassifyScalarArgConversion(cxxArgs[i], candidate.params[i + 1])
                            == ScalarArgConversion::Narrow)
                        {
                            // Clang-only set: the listed pick is not C++'s; surface its refusal.
                            if (nullPointerCandidate != nullptr) *nullPointerCandidate = true;
                            return true;
                        }
                }
        }
        if (selected != nullptr && argVars != nullptr && argVars->size() == argTypes.size()
            && std::any_of(argVars->begin(), argVars->end(), [&](const NamedVariable& arg) {
                   return IsNullPointerConstantArgument(arg, true);
               }))
        {
            const CxxClassInfo* listed = GetCxxClassInfoWithConstructors(typeName);
            if (listed != nullptr)
            {
                auto* self = const_cast<LLVMBackend*>(this);
                auto rankCandidate = [&](const CxxClassInfo::Structor& candidate) {
                    FunctionSymbol symbol;
                    symbol.IsCxx = true;
                    symbol.IsMethod = true;
                    symbol.UniqueName = candidate.linkageName;
                    symbol.Parameters = candidate.params;
                    symbol.Parameters[0].VariableName = "this";
                    symbol.Parameters[0].TypeName = typeName;
                    std::vector<NamedVariable> args(1);
                    args[0].TypeAndValue.TypeName = typeName;
                    args.insert(args.end(), argVars->begin(), argVars->end());
                    std::vector<CxxConversionRank> ranks = self->RankCxxConversionSequences(args, symbol);
                    if (ranks.size() != args.size()) return std::vector<CxxConversionRank>{};
                    ranks.erase(ranks.begin());
                    for (size_t i = 0; i < argVars->size(); ++i)
                    {
                        const TypeAndValue& param = candidate.params[i + 1];
                        if (!IsNullPointerConstantArgument((*argVars)[i], true)
                            || param.Pointer || param.IsAlias || param.IsRvalueRef)
                            continue;
                        const std::string target = self->CxxIntegerParameterIdentity(symbol, i + 1, param);
                        TypeAndValue integralTarget;
                        integralTarget.TypeName = target;
                        if (target.empty() || (target != "bool" && integralTarget.IsInteger() == -1))
                            continue;
                        // This is C++ viability for null-pointer overload ties; it does not relax
                        // CFlat's separate no-implicit-narrowing rule for ordinary call arguments.
                        if (!ranks[i].cxxViable)
                        {
                            ranks[i].rank = 2;
                            ranks[i].cxxViable = true;
                        }
                    }
                    return ranks;
                };
                std::vector<CxxConversionRank> selectedRanks = rankCandidate(*selected);
                const bool selectedCxxViable = selectedRanks.size() == argVars->size()
                    && std::all_of(selectedRanks.begin(), selectedRanks.end(),
                        [](const CxxConversionRank& rank) { return rank.cxxViable; });
                bool selectedHasNullPointerParam = false;
                for (size_t i = 0; i < argVars->size(); ++i)
                    selectedHasNullPointerParam |= IsNullPointerConstantArgument((*argVars)[i], true)
                        && (selected->params[i + 1].Pointer || selected->params[i + 1].IsFunctionPointer);
                for (const CxxClassInfo::Structor& candidate : listed->constructors)
                {
                    if (!allowExplicit && candidate.isExplicit) continue;
                    if (candidate.params.empty()) continue;
                    const size_t parameterCount = candidate.params.size() - 1;
                    if (argVars->size() > parameterCount) continue;
                    bool defaults = true;
                    for (size_t i = argVars->size(); defaults && i < parameterCount; ++i)
                        defaults = i + 1 < candidate.defaultArgs.size()
                            && !candidate.defaultArgs[i + 1].kind.empty();
                    if (!defaults) continue;
                    bool hasNullPointerParam = false;
                    for (size_t i = 0; i < argVars->size(); ++i)
                        hasNullPointerParam |= IsNullPointerConstantArgument((*argVars)[i], true)
                            && (candidate.params[i + 1].Pointer || candidate.params[i + 1].IsFunctionPointer);
                    if (!hasNullPointerParam && !selectedHasNullPointerParam) continue;
                    if (candidate.linkageName == selected->linkageName) continue;
                    std::vector<CxxConversionRank> ranks = rankCandidate(candidate);
                    if (ranks.size() != argVars->size()
                        || !std::all_of(ranks.begin(), ranks.end(),
                            [](const CxxConversionRank& rank) { return rank.cxxViable; }))
                        continue;
                    if (hasNullPointerParam && !selectedCxxViable)
                    {
                        if (nullPointerCandidate != nullptr) *nullPointerCandidate = true;
                        return true;
                    }
                    if (!selectedCxxViable) continue;
                    bool crossing = false;
                    if (CompareCxxConversionRanks(ranks, selectedRanks, crossing) != 1)
                    {
                        if (nullPointerCandidate != nullptr) *nullPointerCandidate = true;
                        return true;
                    }
                }
            }
        }
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || !info->hasCtorTemplate) return false;
        if (selected == nullptr) return true;
        if (selected->isCopyCtor || selected->isMoveCtor) return false;
        if (selected->params.size() < argTypes.size() + 1) return true;
        for (size_t i = 0; i < argTypes.size(); ++i)
        {
            const TypeAndValue& want = selected->params[i + 1];
            const TypeAndValue& got = argTypes[i];
            if (want.TypeName != got.TypeName) return true;
            if (want.Pointer == got.Pointer && want.ElemPointer == got.ElemPointer) continue;
            // A reference parameter is an indirect pointer here; binding it to a value of
            // exactly its referent type is still an identity conversion.
            const bool reference = want.IsAlias || want.IsRvalueRef || want.CxxConstRef();
            if (reference && want.Pointer && !want.ElemPointer && !got.Pointer) continue;
            return true;
        }
        return false;
}

bool LLVMBackend::CxxCtorSetRequiresFloatToInteger(
        const std::string& typeName, const std::vector<TypeAndValue>& argTypes) const
{
        if (argTypes.size() != 1 || argTypes.front().IsFloatingPoint() == -1) return false;

        const CxxClassInfo* info = GetCxxClassInfoWithConstructors(typeName);
        if (info == nullptr || info->hasCtorTemplate) return false;
        bool sawCandidate = false;
        for (const CxxClassInfo::Structor& ctor : info->constructors)
        {
            if (ctor.isDeleted || ctor.params.empty()) continue;
            if (ctor.isCopyCtor || ctor.isMoveCtor) continue;
            const size_t paramCount = ctor.params.size() - 1;
            if (argTypes.size() > paramCount) continue;
            bool defaultsAvailable = true;
            for (size_t i = argTypes.size(); i < paramCount; ++i)
                if (i + 1 >= ctor.defaultArgs.size() || ctor.defaultArgs[i + 1].kind.empty())
                    defaultsAvailable = false;
            if (!defaultsAvailable) continue;

            const TypeAndValue& param = ctor.params[1];
            const bool reference = param.Pointer && !param.ElemPointer
                && (param.IsAlias || param.IsRvalueRef || param.CxxConstRef());
            // A floating argument never reaches a pointer parameter, nor a non-const lvalue
            // reference; `const T&` and `T&&` bind a converted temporary like `T`.
            if (param.Pointer && !reference) continue;
            if (reference && !param.CxxConstRef() && !param.IsRvalueRef) continue;
            TypeAndValue arithmetic = param;
            arithmetic.DropCxxRef();
            arithmetic.Pointer = false;
            arithmetic.IsAlias = arithmetic.IsRvalueRef = false;
            if (arithmetic.IsInteger() == -1 && arithmetic.IsFloatingPoint() == -1) continue;
            if (arithmetic.IsFloatingPoint() != -1) return false;
            sawCandidate = true;
        }
        return sawCandidate;
}

void LLVMBackend::ConvertCxxCtorThunkArguments(const std::string& wrapperName,
                                               std::vector<NamedVariable>& arguments)
{
        auto it = functionTable.find(wrapperName);
        if (it == functionTable.end() || it->second.size() != 1) return;
        const std::vector<TypeAndValue>& params = it->second.front().Parameters;
        for (size_t i = 1; i < arguments.size() && i < params.size(); ++i)
        {
            NamedVariable& arg = arguments[i];
            const TypeAndValue& param = params[i];
            if (arg.TypeAndValue.Pointer || arg.Primary == nullptr) continue;
            llvm::Type* have = arg.Primary->getType();
            if (!have->isIntegerTy() && !have->isFloatingPointTy()) continue;
            // A reference parameter is a pointer to the scalar; a by-value one is the scalar.
            const bool reference = param.Pointer && !param.ElemPointer
                && (param.IsAlias || param.IsRvalueRef || param.CxxConstRef());
            if (param.Pointer && !reference) continue;
            TypeAndValue scalar = param;
            scalar.DropCxxRef();
            scalar.Pointer = false;
            scalar.IsAlias = scalar.IsRvalueRef = false;
            llvm::Type* want = GetType(scalar);
            if (want == nullptr || want == have
                || (!want->isIntegerTy() && !want->isFloatingPointTy())) continue;
            // bool converts as 0/1, and a floating source into an unsigned integer is fptoui.
            if (have->isFloatingPointTy() && want->isIntegerTy() && !want->isIntegerTy(1)
                && scalar.IsUnsignedInteger() != -1)
                arg.Primary = builder->CreateFPToUI(arg.Primary, want);
            else
                arg.Primary = CreateCast(arg.Primary, want, !have->isIntegerTy(1)
                    && arg.TypeAndValue.IsUnsignedInteger() == -1);
            arg.Storage = nullptr;
            arg.CallerName.clear();
            arg.LiteralIdentity.clear();
            arg.TypeAndValue.TypeName = scalar.TypeName;
            arg.TypeAndValue.Pointer = false;
        }
}

/*
 * T416: may any constructor of this arity bind argument `position` by reference? A reference binding
 * reads the caller's slot at the thunk call, after later arguments ran; a by-value position is copied
 * when its argument is evaluated.
 */
static bool CtorMayBindScalarByReference(const std::vector<cflat_cinterop::RawCxxMember>& members,
                                         const std::vector<cflat_cinterop::RawCxxCtorTemplate>& templates,
                                         size_t position, size_t argCount)
{
        // Arity viability: a candidate takes this call's argument count when it declares enough
        // parameters without defaults, and either no more than it declares or a trailing `...`.
        // `defaulted` is the parameter list's default flags; `first` is the parameters before it.
        const auto viable = [argCount](size_t paramCount, const auto& defaulted,
                                       bool variadic, bool hasParameterPack) {
            const size_t fixedCount = hasParameterPack && paramCount > 0
                ? paramCount - 1 : paramCount;
            size_t required = fixedCount;
            for (size_t i = 0; i < fixedCount; ++i)
                if (i < defaulted.size() && defaulted[i]) { required = i; break; }
            return argCount >= required
                && (hasParameterPack || variadic || argCount <= fixedCount);
        };
        // A template's parameter list has no `this` entry; a binding by reference keeps the slot.
        // A parameter pack ("A &...") absorbs any count, so positions at or past it take its spelling.
        for (const auto& ctorTemplate : templates)
        {
            const auto& params = ctorTemplate.paramTypes;
            size_t pack = params.size();
            for (size_t i = 0; i < params.size(); ++i)
                if (params[i].ends_with("...")) { pack = i; break; }
            if (!ctorTemplate.hasParameterPack || pack == params.size())
            {
                if (position < params.size() && viable(params.size(), ctorTemplate.defaulted,
                        ctorTemplate.variadic, false)
                    && params[position].ends_with("&")) return true;
                continue;
            }
            // A non-final pack may deduce empty, shifting later parameters: keep the slot if any may bind it.
            if (pack + 1 < params.size())
            {
                for (size_t i = pack; i < params.size(); ++i)
                    if (params[i].find('&') != std::string::npos) return true;
                continue;
            }
            if (!viable(params.size(), ctorTemplate.defaulted, ctorTemplate.variadic,
                        ctorTemplate.hasParameterPack)) continue;
            std::string spelling = params[position < pack ? position : pack];
            if (spelling.ends_with("...")) spelling.resize(spelling.size() - 3);
            while (!spelling.empty() && spelling.back() == ' ') spelling.pop_back();
            if (spelling.ends_with("&")) return true;
        }
        for (const auto& member : members)
        {
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                || position + 1 >= member.paramTypes.size()) continue;
            // Entry 0 of a member's parameter list is `this`; its defaults align with the same list.
            std::vector<bool> defaulted;
            for (size_t i = 1; i < member.paramTypes.size(); ++i)
                defaulted.push_back(i < member.defaultArgs.size() && !member.defaultArgs[i].kind.empty());
            if (!viable(member.paramTypes.size() - 1, defaulted, member.variadic, false)) continue;
            const std::string& parameter = member.paramTypes[position + 1];
            if (parameter.ends_with("...")) continue;
            if (parameter.ends_with("&")) return true;
        }
        return false;
}

bool LLVMBackend::RequestCxxVariadicConstructor(
        const std::string& typeName, const std::vector<NamedVariable>& arguments,
        std::string& wrapperName, std::string& error, bool copyInit,
        bool* noViableCandidate)
{
        if (noViableCandidate != nullptr) *noViableCandidate = false;
        error.clear();
        wrapperName.clear();

        // An implicit conversion can recurse through overload ranking back into the same ctor
        // request. Direct-init and copy-init share this key: only one may resolve at a time.
        std::string shapeKey;
        auto appendKeyString = [&](std::string_view value) {
            shapeKey += std::to_string(value.size());
            shapeKey += ':';
            shapeKey.append(value);
            shapeKey += ';';
        };
        auto appendKeyInt = [&](int value) {
            shapeKey += std::to_string(value);
            shapeKey += ';';
        };
        appendKeyString(typeName);
        appendKeyInt(static_cast<int>(arguments.size()));
        for (const NamedVariable& arg : arguments)
        {
            const TypeAndValue& type = arg.TypeAndValue;
            appendKeyString(type.TypeName);
            appendKeyString(arg.InferSourceTypeName);
            appendKeyString(arg.LiteralIdentity);
            appendKeyString(arg.CxxArithIdentity);
            appendKeyInt(type.Pointer);
            appendKeyInt(type.ElemPointer);
            appendKeyInt(type.PointerDepth);
            appendKeyInt(type.IsAlias);
            appendKeyInt(type.IsRvalueRef);
            appendKeyInt(type.IsCxxRefToPointer);
            appendKeyInt(type.ConstMask);
            appendKeyInt(type.CxxRefLevel);
            appendKeyInt(type.IsFunctionPointer);
            appendKeyInt(type.IsArrayView);
            appendKeyInt(type.IsInterface);
            appendKeyInt(arg.IsCxxAddressOfPointerValue);
            appendKeyInt(arg.IsTopLevelConst);
            appendKeyInt(arg.IsRvalue);
            appendKeyInt(arg.CxxLvalueKind);
            appendKeyInt(arg.Storage != nullptr);
            appendKeyInt(arg.BaseType == nullptr ? -1 : static_cast<int>(arg.BaseType->getTypeID()));
            appendKeyInt(arg.BaseType != nullptr && arg.BaseType->isIntegerTy()
                ? static_cast<int>(arg.BaseType->getIntegerBitWidth()) : 0);
            if (const auto* structure = llvm::dyn_cast_or_null<llvm::StructType>(arg.BaseType))
                appendKeyString(structure->getName().str());
            else
                appendKeyString({});
        }
        if (!cxxVariadicCtorRequestsInProgress_.insert(shapeKey).second)
        {
            // Re-entry cycle: report no cause so the outer request does not quote a stale one.
            lastCxxWrapperCause_.clear();
            lastCxxWrapperNoViableAtRequest_ = false;
            return false;
        }
        struct RequestGuard
        {
            std::unordered_set<std::string>& requests;
            std::string key;
            ~RequestGuard() { requests.erase(key); }
        } requestGuard{ cxxVariadicCtorRequestsInProgress_, std::move(shapeKey) };

        // A using-declaration can inherit a constructor template without putting a concrete
        // constructor in the derived record's member list. Look through public bases for the
        // variadic constructor that can accept the concrete call we are about to wrap.
        std::function<bool(const std::string&, std::set<std::string>&)> hasVariadicCtor;
        hasVariadicCtor = [&](const std::string& name, std::set<std::string>& seen) {
            if (!seen.insert(name).second) return false;
            auto record = cxxRecordEntries_.find(name);
            if (record == cxxRecordEntries_.end())
            {
                const size_t specialization = name.find('$');
                if (specialization != std::string::npos)
                    record = cxxRecordEntries_.find(name.substr(0, specialization));
            }
            if (record != cxxRecordEntries_.end())
                for (const auto& member : record->second.members)
                    if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                        && (member.variadic || member.requiresConstructorWrapper)
                        && member.access == cflat_cinterop::AccessPublic)
                        return true;
            auto info = cxxClasses_.find(name);
            if (info == cxxClasses_.end()) return false;
            for (const auto& base : info->second.bases)
                if (base.access == cflat_cinterop::AccessPublic
                    && hasVariadicCtor(base.name, seen))
                    return true;
            return false;
        };
        std::set<std::string> seen;
        // No variadic/inherited constructor in the record still leaves the TEMPLATE constructors
        // clang never lists (std::optional's converting ctor). Try the wrapper anyway, but report
        // nothing when it fails so the caller keeps its own overload diagnostic.
        bool declaredWrapperCtor = hasVariadicCtor(typeName, seen);
        // A hidden deleted or private sibling tie is decided by clang: its refusal must surface.
        if (!declaredWrapperCtor)
        {
            std::vector<TypeAndValue> argTypes;
            for (const NamedVariable& arg : arguments)
            {
                TypeAndValue tv = CxxFixedArrayDecayType(arg);
                if (tv.TypeName.empty() && !arg.LiteralIdentity.empty())
                    tv.TypeName = arg.LiteralIdentity;
                argTypes.push_back(tv);
            }
            declaredWrapperCtor = HasHiddenScalarCxxConstructor(typeName, argTypes);
        }
        if (!declaredWrapperCtor && copyInit && arguments.size() == 1
            && !arguments.front().TypeAndValue.Pointer
            && IsCxxRecord(arguments.front().TypeAndValue.TypeName)
            && HasImplicitClassConversionOperator(arguments.front().TypeAndValue.TypeName))
            declaredWrapperCtor = true;
        if (!declaredWrapperCtor && copyInit && arguments.size() == 1
            && !arguments.front().TypeAndValue.Pointer
            && IsCxxRecord(arguments.front().TypeAndValue.TypeName)
            && ResolveTypeAlias(arguments.front().TypeAndValue.TypeName)
                == ResolveTypeAlias(typeName))
            declaredWrapperCtor = true;
        // A deleted converting constructor is clang's refusal for this source: report it.
        if (!declaredWrapperCtor && copyInit && arguments.size() == 1
            && !arguments.front().TypeAndValue.Pointer
            && IsCxxRecord(arguments.front().TypeAndValue.TypeName)
            && HasDeletedConvertingCtor(typeName))
            declaredWrapperCtor = true;
        if (!declaredWrapperCtor && IsForeignCxxClassWithConstructors(typeName))
            declaredWrapperCtor = std::any_of(arguments.begin(), arguments.end(),
                [](const NamedVariable& arg) { return IsCxxFixedArrayArgument(arg); });
        bool forwardsScalarReference = false;
        // Mark only this generated top-level declarator; nested tuple types may contain (&).
        static constexpr std::string_view kArrayReferenceParameterMarker =
            "(&cflat_array_argument_marker)";
        auto giveUp = [&](const char* why) {
            if (declaredWrapperCtor) error = why;
            return false;
        };

        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(typeName, ownerSpelling))
        {
            return giveUp("the C++ class type is not registered");
        }

        auto cflatTypeOf = [&](const NamedVariable& arg) {
            std::string type = !arg.LiteralIdentity.empty()
                ? arg.LiteralIdentity : arg.TypeAndValue.TypeName;
            bool pointer = arg.TypeAndValue.Pointer;
            const bool fixedArray = IsCxxFixedArrayArgument(arg);
            llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType;
            // An arithmetic result keeps its C++ type (`l + 1` is `long`, not the i64 TypeName).
            if (std::string arith = CxxArithIdentityForArgument(arg, valueType); !arith.empty())
                type = arith;
            // The declared primitive identity outranks the machine-type guess below, which
            // cannot tell `char` from `i8` or `long` from `i64`.
            if (type.empty()) type = DeclaredPrimitiveIdentityForCxxArgument(arg, valueType);
            if (type.empty())
            {
                if (auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                    constant != nullptr && IsStringLiteralConstant(constant))
                {
                    type = "char";
                    pointer = true;
                }
                else if (valueType != nullptr && valueType->isFloatTy()) type = "float";
                else if (valueType != nullptr && valueType->isDoubleTy()) type = "double";
                else if (valueType != nullptr && valueType->isIntegerTy())
                {
                    const unsigned bits = valueType->getIntegerBitWidth();
                    type = bits == 1 ? "bool" : bits <= 8 ? "i8" : bits <= 16 ? "short"
                         : bits <= 32 ? "int" : "i64";
                }
                else if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(valueType))
                    type = st->getName().str();
                if (type.empty()) type = arg.InferSourceTypeName;
            }
            if (pointer)
            {
                type += "*";
                // `&p` over a `T*` is a `T**` prvalue: its recorded depth is the second star.
                if (arg.TypeAndValue.ElemPointer
                    || (arg.IsCxxAddressOfPointerValue && arg.TypeAndValue.PointerDepth >= 2))
                    type += "*";
            }
            if (fixedArray) type += "*";
            return type;
        };

        auto enumSpelling = [&](const TypeAndValue& type, std::string& spelling) {
            std::string key = ResolveEnumTypeName(type.TypeName);
            if (key.empty() && enumBackingTypes.count(type.TypeName) != 0)
                key = type.TypeName;
            if (key.empty() && type.IsScopedEnum) key = type.TypeName;
            if (key.empty()) return false;
            if (!type.Pointer && !type.ElemPointer && CxxCflatEnumBackingSpelling(key, spelling))
                return true;
            if (auto it = cxxCflatToCxxSpelling_.find(key); it != cxxCflatToCxxSpelling_.end())
                spelling = it->second;
            else
            {
                spelling = key;
                for (size_t pos = 0; (pos = spelling.find('.', pos)) != std::string::npos; pos += 2)
                    spelling.replace(pos, 1, "::");
            }
            return true;
        };

        // A scalar-reference overload set is resolved by clang alone: its refusal is the answer,
        // so it is reported like a declared wrapper's. A blanked primitive TypeName is respelled.
        std::vector<TypeAndValue> argumentTypes;
        for (const NamedVariable& argument : arguments)
        {
            TypeAndValue type = argument.TypeAndValue;
            if (type.TypeName.empty() && !type.Pointer) type.TypeName = cflatTypeOf(argument);
            argumentTypes.push_back(std::move(type));
        }
        forwardsScalarReference = CxxCtorForwardsScalarReference(typeName, argumentTypes);
        declaredWrapperCtor = declaredWrapperCtor || forwardsScalarReference;
        if (std::any_of(arguments.begin(), arguments.end(), [](const NamedVariable& arg) {
                return arg.IsCxxAddressOfPointerValue && arg.Storage == nullptr;
            }))
            declaredWrapperCtor = declaredWrapperCtor || CxxConstructorNeedsClangResolution(
                typeName, nullptr, argumentTypes, &arguments, /*allowExplicit*/ true, nullptr);

        std::vector<std::string> parameterSpellings;
        std::vector<std::string> callArguments;
        parameterSpellings.push_back(ownerSpelling + " *");
        // The C++ spelling of a null pointer constant: the integer literal 0 in the suffix of its
        // type (a zero macro folds to a constant, so its declared type is the key), else empty.
        const auto nullConstantSpelling = [&](const NamedVariable& arg) -> std::string {
            if (!IsNullPointerConstantArgument(arg, true)) return {};
            return CxxZeroLiteralSpelling(arg.IsCxxMacroGlobal ? arg.TypeAndValue.TypeName
                                                               : arg.LiteralIdentity);
        };
        // The pointee const a named pointer argument was DECLARED with: its own TypeAndValue
        // loses it on the identifier path, so a local or a global is read back from its owner.
        const auto declaredPointeeConst = [&](const NamedVariable& arg) {
            const auto spelledConst = [](const TypeAndValue& t) {
                return t.PointeeConstBit() || (t.PointeeConstMask() & 1u) != 0;
            };
            if (spelledConst(arg.TypeAndValue)) return true;
            if (!arg.CallerName.empty())
                if (const NamedVariable* live = FindLiveNamedVariable(arg.CallerName))
                    return spelledConst(live->TypeAndValue);
            if (auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(arg.Storage))
                for (const auto& [name, owner] : globalNamedVariable)
                    if (owner == global)
                        if (auto typed = globalVariableTypes.find(name);
                            typed != globalVariableTypes.end())
                            return spelledConst(typed->second);
            return false;
        };
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const NamedVariable& arg = arguments[i];
            const bool stringLiteral = [&] {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                return constant != nullptr && IsStringLiteralConstant(constant);
            }();
            if (IsUnwrappedFatClosure(arg))
                return giveUp("a capturing closure argument was not prepared for C++");
            std::string cflatType = cflatTypeOf(arg);
            if (cflatType.empty())
            {
                return giveUp("a constructor argument type cannot be spelled in C++");
            }
            std::string spelling;
            if (!arg.CxxClosureSignature.empty())
            {
                // A CFlat closure: the class is constructed from the owning __cflat_closure<>.
                parameterSpellings.push_back("void *");
                callArguments.push_back(CxxClosureCallArgument(arg, "p" + std::to_string(i + 1)));
                continue;
            }
            const bool fixedArray = IsCxxFixedArrayArgument(arg);
            std::vector<uint64_t> arrayDimensions;
            if (fixedArray && arg.TypeAndValue.ConstArraySize != 0)
            {
                arrayDimensions.push_back(arg.TypeAndValue.ConstArraySize);
                arrayDimensions.insert(arrayDimensions.end(),
                    arg.TypeAndValue.ConstInnerDimensions.begin(),
                    arg.TypeAndValue.ConstInnerDimensions.end());
            }
            else if (fixedArray)
            {
                llvm::Type* arrayType = arg.BaseType;
                while (auto* array = llvm::dyn_cast_or_null<llvm::ArrayType>(arrayType))
                {
                    arrayDimensions.push_back(array->getNumElements());
                    arrayType = array->getElementType();
                }
            }
            const bool preserveArrayShape = fixedArray && !arrayDimensions.empty()
                && std::all_of(arrayDimensions.begin(), arrayDimensions.end(),
                               [](uint64_t size) { return size != 0; });
            if (preserveArrayShape)
            {
                std::string elementType = cflatType;
                if (elementType.empty() || elementType.back() != '*')
                    return giveUp("a fixed-array constructor argument has no element type");
                elementType.pop_back();
                if (!CxxSpellingForCflatType(elementType, spelling))
                    return giveUp("a fixed-array constructor argument type has no C++ spelling");
                if (declaredPointeeConst(arg) && !spelling.starts_with("const "))
                    spelling = "const " + spelling;
                spelling += " " + std::string(kArrayReferenceParameterMarker);
                for (uint64_t size : arrayDimensions)
                    spelling += std::format("[{}]", size);
            }
            else if ((arg.TypeAndValue.TypeName == "char" && arg.TypeAndValue.Pointer
                    && arg.IsRvalue) || stringLiteral)
                spelling = "const char *";
            else if (!CxxThinFunctionPointerSpelling(arg.TypeAndValue, spelling)
                     && !enumSpelling(arg.TypeAndValue, spelling)
                     && !CxxSpellingForCflatType(cflatType, spelling))
            {
                return giveUp("a constructor argument type has no C++ spelling");
            }
            const bool namedLvalue = arg.Storage != nullptr
                || (!arg.CallerName.empty() && FindVariableStorage(arg.CallerName).Storage != nullptr);
            std::string callArgument = "p" + std::to_string(i + 1);
            const std::string* unnamedEnumExpression = CxxUnnamedEnumExpression(arg.Storage);
            if (unnamedEnumExpression != nullptr)
                callArgument = *unnamedEnumExpression;
            // An operator result can arrive with only its struct type: key on the resolved name.
            // A record that may not cross by value (an Eigen expression template has bases and
            // a user copy constructor) is forwarded by reference like a nontrivial one.
            const std::string& recordName = arg.TypeAndValue.TypeName.empty()
                ? cflatType : arg.TypeAndValue.TypeName;
            if (!arg.TypeAndValue.Pointer && IsCxxRecord(recordName)
                     && (namedLvalue || IsForeignNontrivialCxxClass(recordName)
                         || cxxTriviallyCopyableRecords_.count(recordName) == 0))
            {
                // A temporary stays an rvalue through the wrapper (`Seq(Lin(2, 3))` into a
                // forwarding `H&&`); an lvalue `T &` could not bind it at the call. An explicit
                // `move` of a named object is an xvalue for clang exactly as at the source site;
                // only a NAMED object is an lvalue when the front end flagged the value rvalue.
                const bool namedByName = !arg.CallerName.empty()
                    && FindVariableStorage(arg.CallerName).Storage != nullptr;
                if (arg.IsExplicitMove || (arg.IsRvalue && !namedByName)
                    || (!namedLvalue && IsCxxRvalueReferenceArgument(arg)))
                {
                    callArgument = "static_cast<" + spelling + " &&>(" + callArgument + ")";
                    spelling += " &&";
                }
                else
                {
                    // A volatile lvalue keeps its volatile in the wrapper: clang copies or binds it
                    // as the source site does (a copy ctor taking `const S&` refuses it).
                    if ((CxxReceiverCv(arg) & 2) != 0) PrependCv(spelling, "volatile ");
                    if (CxxConstReceiverKind(arg) != 0 && !spelling.starts_with("const "))
                        spelling = "const " + spelling;
                    spelling += " &";
                }
            }
            else if (!arg.TypeAndValue.Pointer && IsCxxRecord(arg.TypeAndValue.TypeName)
                     && IsCxxRvalueReferenceArgument(arg))
            {
                // The by-value copy of a temporary is still a temporary for clang.
                callArgument = "static_cast<" + spelling + " &&>(" + callArgument + ")";
            }
            else if (unnamedEnumExpression == nullptr && !arg.TypeAndValue.Pointer && !stringLiteral
                     && !IsCxxRecord(arg.TypeAndValue.TypeName))
            {
                // A scalar is forwarded with the caller's exact C++ type and value category, so
                // clang resolves the overload set as at the source site; `T&` never binds a
                // thunk local, and a converting temp is clang's own.
                llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType()
                                                               : arg.BaseType;
                const bool scalar = valueType != nullptr
                    && (valueType->isIntegerTy() || valueType->isFloatingPointTy());
                TypeAndValue spelled{ .TypeName = cflatType };
                // An lvalue borrows its slot only when that slot holds exactly the spelled type;
                // a converted value has no slot of its own and travels as an xvalue.
                if (scalar && !IsCxxRvalueReferenceArgument(arg) && GetType(spelled) == valueType)
                {
                    // T416: no constructor binds this position by reference, so it travels by value.
                    const auto recordEntry = cxxRecordEntries_.find(typeName);
                    if (recordEntry == cxxRecordEntries_.end()
                        || CtorMayBindScalarByReference(recordEntry->second.members,
                            recordEntry->second.ctorTemplates, i, arguments.size()))
                        spelling += " &";
                }
                else if (scalar)
                {
                    callArgument = "static_cast<" + spelling + " &&>(" + callArgument + ")";
                    spelling += " &&";
                }
            }
            else if ((arg.TypeAndValue.Pointer
                      && (!arg.TypeAndValue.IsAlias || arg.TypeAndValue.IsCxxRefToPointer)
                      && !arg.TypeAndValue.IsFunctionPointer
                      && !IsCxxFixedArrayArgument(arg))
                     || stringLiteral)
            {
                // Keep a pointer's caller slot or xvalue category across the ctor wrapper.
                // A named argument's own TypeAndValue drops `const int *` (the identifier path
                // keeps no pointee const), so its declared variable is the source of truth.
                if (declaredPointeeConst(arg) && !spelling.starts_with("const "))
                    spelling = "const " + spelling;
                if (arg.TypeAndValue.IsCxxPointeeVolatile) PrependCv(spelling, "volatile ");
                const bool xvalue = stringLiteral || IsCxxRvalueReferenceArgument(arg);
                if (arg.TypeAndValue.IsCxxRefToPointer && arg.TypeAndValue.CxxConstRef() && !xvalue)
                    spelling += " const";
                if (xvalue)
                {
                    callArgument = "static_cast<" + spelling + " &&>(" + callArgument + ")";
                    spelling += " &&";
                }
                else
                    spelling += " &";
            }
            // A null pointer constant keeps its literal in the target call: a forwarded `int` is an
            // integral conversion to clang, so `(int*, double)` would lose to `(short, int)`.
            if (unnamedEnumExpression == nullptr)
            {
                if (std::string literal = nullConstantSpelling(arg); !literal.empty())
                    callArgument = std::move(literal);
            }
            parameterSpellings.push_back(std::move(spelling));
            callArguments.push_back(std::move(callArgument));
        }

        // A scalar reaching a reference parameter must be converted in the CALLER, or the
        // temporary dies with the thunk while the object still points at it. Which type that is
        // depends on clang's overload pick, so a mirror of the listed constructors (same parameter
        // lists, returning the parameter at the scalar's position) is resolved against the exact
        // caller types, and the wrapper parameter takes that type; the caller converts into it.
        // A position no mirror candidate can take keeps the exact forwarding type.
        std::string mirror;
        const std::vector<std::string> exactParameterSpellings = parameterSpellings;
        const std::vector<std::string> exactCallArguments = callArguments;
        {
            // The mirror is FAITHFUL to clang's candidate set or it does not exist: every
            // constructor of the right arity (templates, defaults and `...` included) is
            // declared with its access and deleted-ness; a candidate that cannot be spelled
            // (or a parameter pack) drops the whole mirror, and the wrapper then forwards the
            // exact types so clang's diagnostic names the real problem.
            struct MirrorCandidate
            {
                std::string head;                  // template head, empty for a plain constructor
                std::vector<std::string> params;
                std::vector<bool> defaulted;
                bool variadic = false;
                bool deleted = false;
                int access = 0;
            };
            std::vector<MirrorCandidate> mirrorCandidates;
            bool mirrorOk = true;
            // An unnamed type has no spelling outside its own declaration.
            const auto unnamed = [](const std::string& spelling) {
                return spelling.find("(unnamed") != std::string::npos
                    || spelling.find("(anonymous") != std::string::npos
                    || spelling.find("(lambda") != std::string::npos;
            };
            std::set<std::string> visited;
            std::function<void(const std::string&)> collect = [&](const std::string& name) {
                if (!visited.insert(name).second || !mirrorOk) return;
                const CxxClassInfo* info = GetCxxClassInfo(name);
                if (info == nullptr) return;
                // The RAW record lists every constructor with clang's canonical parameter
                // spellings; the mapped list drops deleted, non-public and variadic ones.
                auto raw = cxxRecordEntries_.find(name);
                if (raw == cxxRecordEntries_.end())
                {
                    const size_t specialization = name.find('$');
                    if (specialization != std::string::npos)
                        raw = cxxRecordEntries_.find(name.substr(0, specialization));
                }
                if (raw == cxxRecordEntries_.end()) { mirrorOk = false; return; }
                for (const auto& c : raw->second.members)
                {
                    if (c.kind != cflat_cinterop::RawCxxMember::Constructor) continue;
                    // A copy/move constructor only takes a scalar through a user-defined
                    // conversion, which never outranks a viable listed candidate.
                    if (c.isCopyCtor || c.isMoveCtor || (copyInit && c.isExplicit)) continue;
                    const size_t named = c.paramTypes.size() - 1;
                    MirrorCandidate candidate;
                    candidate.variadic = c.variadic;
                    candidate.deleted = c.isDeleted;
                    candidate.access = c.access;
                    // Arity: a trailing default fills a missing argument, `...` takes extras.
                    bool viable = c.variadic ? named <= arguments.size() : named >= arguments.size();
                    bool spellable = true;
                    for (size_t i = 1; i < c.paramTypes.size(); ++i)
                    {
                        const bool defaulted = i < c.defaultArgs.size() && !c.defaultArgs[i].kind.empty();
                        candidate.defaulted.push_back(defaulted);
                        if (i > arguments.size() && !defaulted) viable = false;
                        const std::string& one = c.paramTypes[i];
                        spellable = spellable && !one.empty() && !unnamed(one);
                        // A listed template PATTERN (dependent spelling): the template list
                        // below declares it with its own head.
                        if (one.find("-parameter-") != std::string::npos) viable = false;
                        candidate.params.push_back(one);
                    }
                    if (!viable) continue;
                    if (!spellable) { mirrorOk = false; return; }
                    mirrorCandidates.push_back(std::move(candidate));
                }
                // A constructor template is redeclared with its own head; its deduction and
                // SFINAE then run in the mirror as at the source site. A parameter pack has
                // no single position to answer for, so such a set is not mirrored.
                for (const auto& t : raw->second.ctorTemplates)
                {
                    if (copyInit && t.isExplicit) continue;
                    MirrorCandidate candidate;
                    candidate.head = t.head;
                    candidate.variadic = t.variadic;
                    candidate.deleted = t.isDeleted;
                    candidate.access = t.access;
                    candidate.params = t.paramTypes;
                    bool viable = t.variadic ? t.paramTypes.size() <= arguments.size()
                                             : t.paramTypes.size() >= arguments.size();
                    for (size_t i = 0; i < t.paramTypes.size(); ++i)
                    {
                        const bool defaulted = i < t.defaulted.size() && t.defaulted[i] != 0;
                        candidate.defaulted.push_back(defaulted);
                        if (i >= arguments.size() && !defaulted) viable = false;
                        if (t.paramTypes[i].find("...") != std::string::npos
                            || unnamed(t.paramTypes[i]))
                            viable = false, mirrorOk = false;
                    }
                    if (!mirrorOk) return;
                    if (viable) mirrorCandidates.push_back(std::move(candidate));
                }
                // An inherited `using Base::Base` constructor is listed on the base only.
                if (std::none_of(raw->second.members.begin(), raw->second.members.end(),
                                 [](const cflat_cinterop::RawCxxMember& member) {
                                     return member.kind == cflat_cinterop::RawCxxMember::Constructor
                                         && member.requiresConstructorWrapper && !member.variadic;
                                 }))
                    return;
                for (const auto& base : info->bases)
                    if (base.access == cflat_cinterop::AccessPublic) collect(base.name);
            };
            std::vector<size_t> scalarPositions;
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                const NamedVariable& arg = arguments[i];
                llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType()
                                                               : arg.BaseType;
                if (!arg.TypeAndValue.Pointer && !IsCxxRecord(arg.TypeAndValue.TypeName)
                    && valueType != nullptr
                    && (valueType->isIntegerTy() || valueType->isFloatingPointTy())
                    && parameterSpellings[i + 1].ends_with("&")
                    && nullConstantSpelling(arg).empty())
                    scalarPositions.push_back(i);
            }
            if (!scalarPositions.empty()) collect(typeName);
            if (!mirrorOk) mirrorCandidates.clear();
            // Only a REFERENCE pick needs the caller-side conversion; a by-value parameter
            // converts inside the wrapper as before, so the mirror exists only for a set that
            // can bind a scalar position by reference.
            bool referenceAtScalar = false;
            for (const auto& candidate : mirrorCandidates)
                for (size_t pos : scalarPositions)
                    referenceAtScalar = referenceAtScalar
                        || (pos < candidate.params.size() && candidate.params[pos].ends_with("&"));
            if (!scalarPositions.empty() && referenceAtScalar)
            {
                // Exact caller types: `U &` for an lvalue, `U` for an rvalue (declval gives `U &&`).
                std::vector<std::string> exact;
                for (size_t i = 0; i < arguments.size(); ++i)
                {
                    std::string spelling = parameterSpellings[i + 1];
                    const bool xvalue = callArguments[i].starts_with("static_cast<")
                        || !nullConstantSpelling(arguments[i]).empty();
                    if (spelling.ends_with(" &&")) spelling.resize(spelling.size() - 3);
                    else if (spelling.ends_with(" &")) spelling.resize(spelling.size() - 2);
                    exact.push_back(xvalue ? spelling : spelling + " &");
                }
                // One mirror per wrapper: the same exact types can reach two wrappers.
                std::string key = "mirror" + typeName + (copyInit ? "|copy" : "|direct");
                for (const auto& e : exact) key += "|" + e;
                for (const auto& p : parameterSpellings) key += "|" + p;
                for (const auto& c : callArguments) key += "|" + c;
                const std::string ns = std::format("__cflat_sel_{:016x}", HashWrapperKey(key));
                // `arg<P, E>`: P is the pick, E the exact caller type. A by-value pick takes the
                // caller's scalar by value at the boundary and converts to P inside as a prvalue
                // (one construction, no copy); `X &` binds as is; `X &&` takes
                // `const X &` at the boundary (the caller materializes the converted temporary)
                // and is cast back to `X &&` inside.
                mirror = "namespace " + ns + " {\ntemplate <class U> U && declval();\n"
                    "template <class T> struct rr { using t = T; };\n"
                    "template <class T> struct rr<T &> { using t = T; };\n"
                    "template <class T> struct rr<T &&> { using t = T; };\n"
                    "template <class P, class E> struct arg { using param = typename rr<E>::t; using mutref = typename rr<E>::t &; using cast = P; };\n"
                    "template <class X, class E> struct arg<X &, E> { using param = X &; using mutref = X &; using cast = X &; };\n"
                    "template <class X, class E> struct arg<X &&, E> { using param = const X &; using mutref = X &; using cast = X &&; };\n";
                std::string declvals;
                for (size_t i = 0; i < exact.size(); ++i)
                    declvals += (i != 0 ? ", " : "") + std::string("declval<") + exact[i] + ">()";
                for (size_t pos : scalarPositions)
                {
                    const std::string fallback = callArguments[pos].starts_with("static_cast<")
                        ? exact[pos] + " &&" : exact[pos];
                    // The candidates live in a class so a private or protected constructor
                    // keeps its access; access failure and `= delete` are substitution
                    // failures, so such a pick falls back to exact forwarding and the real
                    // constructor call carries clang's diagnostic. A deleted or inaccessible
                    // pick is never silently replaced by the next-best candidate.
                    const std::string sel = std::format("sel_{}", pos);
                    mirror += "struct " + sel + " {\n";
                    std::set<std::string> declared;
                    for (const auto& candidate : mirrorCandidates)
                    {
                        // A `...` position converts by value: its exact type is the pick.
                        // Parameters stay abstract declarators (`const T (&)[N]` names no
                        // parameter); the trailing return repeats the position's type-id.
                        std::string decl = candidate.head + (candidate.head.empty() ? "" : " ")
                                         + "static auto sel(";
                        for (size_t i = 0; i < candidate.params.size(); ++i)
                        {
                            decl += (i != 0 ? ", " : "") + candidate.params[i];
                            if (candidate.defaulted[i])
                                decl += " = declval<" + candidate.params[i] + ">()";
                        }
                        if (candidate.variadic) decl += candidate.params.empty() ? "..." : ", ...";
                        decl += ") -> " + (pos < candidate.params.size() ? candidate.params[pos]
                                                                          : fallback);
                        decl += candidate.deleted ? " = delete;\n" : ";\n";
                        if (!declared.insert(decl).second) continue;
                        mirror += candidate.access == cflat_cinterop::AccessPrivate ? "private: "
                                : candidate.access == cflat_cinterop::AccessProtected ? "protected: "
                                : "public: ";
                        mirror += decl;
                    }
                    mirror += "};\n";
                    // A null pointer constant is spelled as its literal in both pick overloads, so clang
                    // ranks it as a null pointer conversion (a declval<int> prvalue would not be).
                    std::string pickTemplate;
                    std::string pickParams;
                    std::string selArguments;
                    for (size_t k = 0; k < exact.size(); ++k)
                    {
                        const std::string literal = nullConstantSpelling(arguments[k]);
                        pickTemplate += (k != 0 ? ", " : "") + std::string("class A")
                            + std::to_string(k);
                        pickParams += ", A" + std::to_string(k) + "&& a" + std::to_string(k);
                        selArguments += (k != 0 ? ", " : "") + (literal.empty()
                            ? "static_cast<A" + std::to_string(k) + "&&>(a" + std::to_string(k) + ")"
                            : literal);
                    }
                    mirror += std::format(
                        "template <{0}> auto pick_{1}(int{2}) -> decltype({3}::sel({4}));\n"
                        "template <{0}> auto pick_{1}(long{2}) -> {5};\n"
                        "using P_{1} = decltype(pick_{1}(0, {6}));\n",
                        pickTemplate, pos, pickParams, sel, selArguments, fallback, declvals);
                    const std::string pick = ns + "::arg<" + ns + "::P_" + std::to_string(pos)
                                           + ", " + fallback + ">";
                    parameterSpellings[pos + 1] = pick + "::param";
                    callArguments[pos] = "static_cast<" + pick + "::cast>(const_cast<" + pick
                                       + "::mutref>(p" + std::to_string(pos + 1) + "))";
                }
                mirror += "}\n";
            }
        }

        std::string wrapperSource;
        const auto namedParameter = [](std::string spelling, const std::string& name) {
            const size_t arrayReference = spelling.find(kArrayReferenceParameterMarker);
            if (arrayReference != std::string::npos)
            {
                spelling.replace(arrayReference, kArrayReferenceParameterMarker.size(),
                                 "(&" + name + ")");
                return spelling;
            }
            return spelling + " " + name;
        };
        auto compose = [&](const std::vector<std::string>& params,
                           const std::vector<std::string>& args, const std::string& sel) {
            std::string targetCall = ownerSpelling + "(";
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (i != 0) targetCall += ", ";
                targetCall += args[i];
            }
            targetCall += ")";
            // Copy-initialization through a returning lambda: `return p1;` refuses an explicit
            // constructor, and the prvalue initializes *p0 directly (guaranteed elision).
            if (copyInit && args.size() == 1)
                targetCall = ownerSpelling + "([&]() -> " + ownerSpelling + " { return "
                           + args[0] + "; }())";
            std::string hashKey = "variadic_ctor" + typeName + targetCall + sel;
            for (const auto& param : params) hashKey += param;
            wrapperName = std::format("__cflat_ctor_{:016x}", HashWrapperKey(hashKey));
            wrapperSource = "extern \"C\" __attribute__((weak)) void " + wrapperName + "(";
            for (size_t i = 0; i < params.size(); ++i)
            {
                if (i != 0) wrapperSource += ", ";
                wrapperSource += namedParameter(params[i], "p" + std::to_string(i));
            }
            // `::new`: a class-scope operator new hides the global placement form (T247).
            wrapperSource += ") { ::new (p0) " + targetCall + "; }\n";
            if (verbose) std::cout << "[verbose]   generated ctor wrapper: " << sel << wrapperSource;
        };
        compose(parameterSpellings, callArguments, mirror);
        std::vector<std::string> generatedTypeArguments{ typeName };
        for (const NamedVariable& argument : arguments)
            generatedTypeArguments.push_back(cflatTypeOf(argument));
        std::string generatedTypeSource;
        std::set<size_t> generatedDependencyGroups;
        std::unordered_set<std::string> incompleteTypes;
        GeneratedCxxDefinitionsFor(generatedTypeArguments, generatedTypeSource,
                                   generatedDependencyGroups, incompleteTypes);
        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        if (groupIt == cxxTypeOwnerGroup_.end())
        {
            return giveUp("the C++ class's import group is unavailable");
        }
        // C++ argument types (std::function from <functional>) bring their own import group:
        // the wrapper names them, and the class's header need not include theirs.
        auto collectArgumentGroups = [&](auto&& self, const std::string& type) -> void {
            std::string base = type;
            while (!base.empty() && base.back() == '*') base.pop_back();
            if (auto owner = cxxTypeOwnerGroup_.find(base);
                owner != cxxTypeOwnerGroup_.end() && owner->second != groupIt->second
                && owner->second < cxxImportGroups_.size()
                && !cxxImportGroups_[owner->second].headers.empty())
                generatedDependencyGroups.insert(owner->second);
            TypeSpelling parsed;
            if (!DemangleType(*this, base, parsed)) return;
            for (const auto& arg : parsed.args)
                self(self, MangleType(*this, arg));
        };
        for (const NamedVariable& argument : arguments)
            collectArgumentGroups(collectArgumentGroups, cflatTypeOf(argument));
        std::vector<size_t> dependencyGroups(generatedDependencyGroups.begin(),
                                             generatedDependencyGroups.end());
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, typeName,
                                                          dependencyGroups);
        if (group.headers.empty())
        {
            return giveUp("the C++ class's import group is unavailable");
        }
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry bound;
        std::string wrapperError;
        const bool allowIncremental = true;
        bool requested = RequestGeneratedCxxWrapper(group, generatedTypeSource + mirror + wrapperSource,
                                                    wrapperName, "VARIADIC_CTOR", bound, wrapperError,
                                                    incompleteTypes.empty(), allowIncremental);
        if (!requested && !mirror.empty())
        {
            // A mirror clang cannot compile (a spelling that does not round-trip) must not
            // refuse the call: the exact forwarding wrapper is the answer then. A real
            // refusal (deleted, inaccessible, no match) repeats on it with clang's diagnostic.
            compose(exactParameterSpellings, exactCallArguments, "");
            std::string retryError;
            requested = RequestGeneratedCxxWrapper(group, generatedTypeSource + wrapperSource,
                                                   wrapperName, "VARIADIC_CTOR", bound, retryError,
                                                   incompleteTypes.empty(), allowIncremental);
            if (!requested) wrapperError = retryError;
        }
        if (!requested)
        {
            if (noViableCandidate != nullptr)
                *noViableCandidate = lastCxxWrapperNoViableAtRequest_;
            // Clang selected a constructor whose body failed to instantiate: its refusal stands.
            if (wrapperError.starts_with("clang: "))
            {
                error = wrapperError;
                return false;
            }
            if (forwardsScalarReference)
                error = std::format("has no constructor that binds these arguments (clang: {})",
                                    FirstCxxErrorLine(wrapperError));
            else if (declaredWrapperCtor || wrapperError.find("ambiguous") != std::string::npos)
                error = std::format("C++ variadic constructor call '{}' does not match (clang: {})",
                                    typeName, FirstCxxErrorLine(wrapperError));
            return false;
        }
        bound.name = wrapperName;
        RegisterCSignatures({ bound }, group.headers.front());
        if (auto it = functionTable.find(wrapperName); it != functionTable.end())
            for (auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                {
                    symbol.IsCxxConstructorForwardingWrapper = true;
                    return true;
                }
        return giveUp("the generated C++ variadic constructor wrapper could not be registered");
}

bool LLVMBackend::RequestCxxOperatorArrow(const std::string& typeName, std::string& error)
{
        error.clear();
        if (HasArrowOverloadFor(typeName)) return true;

        std::function<const cflat_cinterop::RawCxxMember*(
            const std::string&, std::set<std::string>&)> findArrow;
        findArrow = [&](const std::string& name, std::set<std::string>& seen)
            -> const cflat_cinterop::RawCxxMember* {
            if (!seen.insert(name).second) return nullptr;
            auto record = cxxRecordEntries_.find(name);
            auto findMember = [&](const auto& candidate)
                -> const cflat_cinterop::RawCxxMember* {
                if (candidate == cxxRecordEntries_.end()) return nullptr;
                for (const auto& member : candidate->second.members)
                    if (member.kind == cflat_cinterop::RawCxxMember::Instance
                        && member.name == "operator->"
                        && member.access == cflat_cinterop::AccessPublic)
                        return &member;
                return nullptr;
            };
            if (const auto* member = findMember(record)) return member;
            if (record == cxxRecordEntries_.end()
                || findMember(record) == nullptr)
            {
                const size_t specialization = name.find('$');
                if (specialization != std::string::npos)
                    if (const auto* member = findMember(
                            cxxRecordEntries_.find(name.substr(0, specialization))))
                        return member;
            }
            auto info = cxxClasses_.find(name);
            if (info == cxxClasses_.end()) return nullptr;
            for (const auto& base : info->second.bases)
                if (base.access == cflat_cinterop::AccessPublic)
                    if (const auto* member = findArrow(base.name, seen)) return member;
            return nullptr;
        };

        std::set<std::string> seen;
        if (findArrow(typeName, seen) == nullptr)
            return false;
        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(typeName, ownerSpelling))
        {
            error = "the C++ class type is not registered";
            return false;
        }

        const uint64_t hash = HashWrapperKey(typeName + "|operator->");
        const std::string wrapperName = std::format("__cflat_arrow_{:016x}", hash);
        const std::string wrapperSource =
            "extern \"C\" __attribute__((weak)) auto " + wrapperName + "(" + ownerSpelling
            + " * p0) { return p0->operator->(); }\n";

        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        if (groupIt == cxxTypeOwnerGroup_.end())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, typeName);
        if (group.headers.empty())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry bound;
        std::string wrapperError;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName, "OPERATOR_ARROW",
                                         bound, wrapperError))
        {
            error = std::format("C++ operator-> wrapper for '{}' could not be generated: {}",
                                typeName, FirstCxxErrorLine(wrapperError));
            return false;
        }
        bound.name = "operator->";
        RegisterCSignatures({ bound }, group.headers.front());
        if (auto it = functionTable.find("operator->"); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                    return true;
        error = "the generated C++ operator-> wrapper could not be registered";
        return false;
}

/*
 * Import groups that could own a C++ base name, best first: groups that published the name,
 * then a system group whose header IS the name
 * (`std::vector` from `import cpp "vector"`), then groups that seeded the leading namespace, then
 * every other C++ group; import order within each tier. Only the first candidate that actually
 * declares the name is used.
 */
std::vector<size_t> LLVMBackend::CandidateCxxGroupsFor(const std::string& cxxBase)
{
        if (auto known = cxxTemplateOwnerGroup_.find(cxxBase);
            known != cxxTemplateOwnerGroup_.end())
            return { known->second };
        const size_t lastSep = cxxBase.rfind("::");
        const std::string leaf = lastSep == std::string::npos ? cxxBase : cxxBase.substr(lastSep + 2);
        const size_t firstSep = cxxBase.find("::");
        const std::string lead = firstSep == std::string::npos ? cxxBase : cxxBase.substr(0, firstSep);
        std::vector<size_t> published, byName, byNamespace, rest;
        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
        {
            const CxxImportGroup& g = cxxImportGroups_[i];
            if (g.publishedNames.count(cxxBase) != 0) { published.push_back(i); continue; }
            bool headerIsName = false;
            for (const auto& h : g.headers)
                headerIsName = headerIsName
                    || std::filesystem::path(h).filename().string() == leaf
                    || std::filesystem::path(h).stem().string() == leaf;
            if (headerIsName) { byName.push_back(i); continue; }
            if (g.namespaces.count(lead) != 0) { byNamespace.push_back(i); continue; }
            rest.push_back(i);
        }
        std::vector<size_t> order = std::move(published);
        order.insert(order.end(), byName.begin(), byName.end());
        order.insert(order.end(), byNamespace.begin(), byNamespace.end());
        order.insert(order.end(), rest.begin(), rest.end());
        return order;
    }

/*
 * On-demand counterpart of the deferred signature binding above: a C++ free function is bound the
 * first time CFlat code looks its name up, requesting only the types ITS signature names. One
 * attempt per name - a signature that cannot be mapped stays unbound and the caller falls back to
 * the ordinary "undefined function" path.
 */
bool LLVMBackend::TryBindCxxFunction(const std::string& functionName)
{
        // Not gated on the name being unbound: one overload may have bound eagerly (its types
        // were already registered) while a sibling still needs its type requested, and an
        // incomplete overload set turns a precise diagnostic into "no overload matches".
        auto sigIt = cxxFunctionSignatures_.find(functionName);
        if (sigIt == cxxFunctionSignatures_.end()) return false;
        if (!cxxFunctionBindAttempts_.insert(functionName).second) return false;

        auto groupIt = cxxFunctionOwnerGroup_.find(functionName);
        if (groupIt == cxxFunctionOwnerGroup_.end()) return false;
        CxxRequestGroup group = MakeCxxRequestGroup(groupIt->second, {});
        if (group.headers.empty()) return false;
        CxxRequestGroupScope groupScope(*this, &group);

        std::vector<CSigEntry> bound;
        for (const CSigEntry& stored : sigIt->second)
        {
            cflat_cinterop::RawSig raw;
            raw.name = stored.name;
            raw.linkageName = stored.linkageName;
            raw.retType = stored.retSpelling;
            raw.paramTypes = stored.paramSpellings;
            raw.paramTemporaryTypes = stored.paramTemporarySpellings;
            raw.resultBorrowsBraceList = stored.resultBorrowsBraceList;
            raw.braceListElementTypes = stored.braceListElementSpellings;
            raw.calleeIdentity = stored.calleeIdentity;
            raw.ctorPick = stored.ctorPick;
            raw.paramBindKinds = stored.paramBindKinds;
            raw.conditionalArgumentKinds = stored.conditionalArgumentKinds;
            raw.closureKeptParams = stored.closureKeptParams;
            raw.selectedCalleeType = stored.selectedCalleeType;
            raw.selectedCalleeTemplateArgs = stored.selectedCalleeTemplateArgs;
            raw.selectedCalleeDefaults = stored.selectedCalleeDefaults;
            for (const TypeAndValue& param : stored.params)
                raw.paramNames.push_back(param.VariableName);
            raw.defaultArgs = stored.defaultArgs;
            raw.variadic = stored.variadic;
            raw.isCxx = stored.isCxx;
            raw.isDeleted = stored.isDeleted;
            raw.isTemplateSpecialization = stored.isTemplateSpecialization;
            raw.isNoexcept = stored.isNoexcept;
            // The EXTRACTOR's own refusal (a 'consteval' function, an uninstantiated
            // specialization) is a fact about the declaration, not about type mapping, so the
            // deferred remap must not drop it - same rule the cache-replay remap follows.
            raw.bindRefusal = stored.sourceBindRefusal;
            raw.abi = stored.abi;
            raw.file = stored.file;
            raw.line = stored.line;
            raw.col = stored.col;
            RequestCxxSignatureTypes(raw);
            CSigEntry mapped;
            if (!MapRawSig(raw, mapped)) continue;
            // A signature that is still unmappable keeps the wording of the refusal recorded at
            // import time: that one was built from the extractor's own parameter names, which a
            // remap from the stored spellings cannot reproduce.
            if (!mapped.bindRefusal.empty() && !stored.bindRefusal.empty())
                mapped.bindRefusal = stored.bindRefusal;
            if (!stored.sourceBindRefusal.empty())
                mapped.bindRefusal = stored.sourceBindRefusal;
            bound.push_back(std::move(mapped));
        }
        if (!bound.empty())
        {
            auto projectByValueType = [&](const TypeAndValue& type) {
                if (!type.TypeName.empty() && !type.Pointer && !type.IsAlias
                    && !type.IsInterface && !type.IsFunctionPointer
                    && IsCxxRecord(type.TypeName))
                    EnsureCxxRecordProjected(type.TypeName, false);
            };
            for (const CSigEntry& signature : bound)
            {
                projectByValueType(signature.ret);
                for (const TypeAndValue& param : signature.params) projectByValueType(param);
            }
            RegisterCSignatures(bound, group.headers.front());
        }
        return functionTable.find(functionName) != functionTable.end();
}

/*
 * A foreign identity is assembled directly from a C++ spelling (CxxForeignIdentity), not
 * through MangleGenericInstance, so the demangler has no argument count for it and every
 * diagnostic would print the raw mangled name (`std.array$int$.4`). Record the count here.
 */
void LLVMBackend::RememberCxxMangledArity(const std::string& cflatName,
                                          const std::string& cxxSpelling) const
{
        const size_t open = cxxSpelling.find('<');
        if (open == std::string::npos || cflatName.find('$') == std::string::npos) return;
        size_t depth = 0;
        size_t args = 1;
        std::vector<std::pair<size_t, size_t>> topLevelArgs;   // [begin, end) of each argument
        size_t argBegin = std::string::npos;
        for (size_t i = open; i < cxxSpelling.size(); ++i)
        {
            const char c = cxxSpelling[i];
            if (c == '<' || c == '(')
            {
                if (++depth == 1) argBegin = i + 1;
            }
            else if (c == '>' || c == ')')
            {
                if (--depth == 0)
                {
                    if (argBegin != std::string::npos) topLevelArgs.emplace_back(argBegin, i);
                    break;
                }
            }
            else if (c == ',' && depth == 1)
            {
                ++args;
                if (argBegin != std::string::npos) topLevelArgs.emplace_back(argBegin, i);
                argBegin = i + 1;
            }
        }
        /*
         * A mapped identity drops defaulted trailing arguments (`std.set$cplv.Key` for the full
         * `std::set<cplv::Key, std::less<..>, std::allocator<..>>`). Record the count the NAME
         * carries: the full spelling's count would make the demangler reject the name.
         */
        const std::string fullIdentity = CanonicalCxxForeignIdentity(cxxSpelling);
        if (!fullIdentity.empty() && fullIdentity != cflatName)
        {
            size_t named = 0;
            for (size_t k = 1; k < topLevelArgs.size() && named == 0; ++k)
                if (CanonicalCxxForeignIdentity(
                        cxxSpelling.substr(0, topLevelArgs[k - 1].second) + ">") == cflatName)
                    named = k;
            if (named != 0) RememberMangledArity(*this, cflatName, named);
        }
        else
            RememberMangledArity(*this, cflatName, args);

        /*
         * A NESTED template-id inside the argument list becomes its own mangled identity
         * (`std.vector<bool, std.allocator<bool>>` inside `std.__bit_reference<...>`), and the
         * hint lookup takes the LONGEST matching prefix - so without the nested count a shorter
         * hint for the same base (`std.vector$bool`, arity 1) wins and the demangler renders the
         * comma as nesting. Record every nesting level the spelling names.
         */
        for (const auto& [begin, end] : topLevelArgs)
        {
            size_t b = begin;
            size_t e = end;
            while (b < e && std::isspace((unsigned char)cxxSpelling[b])) ++b;
            while (e > b && std::isspace((unsigned char)cxxSpelling[e - 1])) --e;
            if (b >= e) continue;
            const std::string argSpelling = cxxSpelling.substr(b, e - b);
            if (argSpelling.find('<') == std::string::npos) continue;
            const std::string argName = CanonicalCxxForeignIdentity(argSpelling);
            if (argName.empty() || argName.find('$') == std::string::npos) continue;
            RememberCxxMangledArity(argName, argSpelling);
        }
}

std::string LLVMBackend::GeneratedCxxPrefixForSpelling(const std::string& cxxSpelling) const
{
        if (cxxSpelling.find("__cflat_user::") == std::string::npos) return {};
        std::vector<std::string> generatedNames;
        for (const auto& entry : generatedCxxRecords_)
        {
            const std::string& typeName = entry.first;
            const std::string cxxName =
                cflat_cinterop::GeneratedCflatStructCxxName(typeName);
            if (cxxSpelling.find(cxxName) != std::string::npos)
                generatedNames.push_back(typeName);
        }
        std::sort(generatedNames.begin(), generatedNames.end());
        std::string source;
        for (const std::string& typeName : generatedNames)
            source += generatedCxxRecords_.at(typeName).source + "\n";
        return source;
}


// The group's own headers and everything they transitively include, recorded when the group
// is bound (cold: clang's include list; warm: the cache entry's dependency list).
void LLVMBackend::RememberCxxGroupReachableFiles(size_t group,
                                                 const std::vector<std::string>& files)
{
        // No include list (a system header bound without a parse) leaves the closure unknown.
        if (group >= cxxImportGroups_.size() || files.empty()) return;
        CxxImportGroup& g = cxxImportGroups_[group];
        for (const std::string& header : g.headers)
            g.reachableFiles.insert(NormalizeCxxOwnershipPath(header));
        for (const std::string& file : files)
            g.reachableFiles.insert(NormalizeCxxOwnershipPath(file));
}

// The closure's per-file content hashes (see CxxGroupClosureHash); same files as above.
void LLVMBackend::RememberCxxGroupClosureContent(size_t group, const std::vector<CHeaderDep>& deps)
{
        if (group >= cxxImportGroups_.size()) return;
        CxxImportGroup& g = cxxImportGroups_[group];
        for (const CHeaderDep& dep : deps)
            g.closureContent[NormalizeCxxOwnershipPath(dep.path)] = dep.hash;
        g.closureHashFiles = static_cast<size_t>(-1);
}

/*
 * An owning-group probe compiles against its candidate's headers PLUS the groups owning the
 * template arguments, and its incremental TU keeps every header an earlier request fed in. So a
 * probe can succeed in a group that never declares the type. A candidate owns the record only
 * when the record's file is reachable from the candidate's own headers; a record reachable from
 * some other group alone is left for that group's probe. A file no group can place is accepted,
 * and so is everything when the candidate's own closure is unknown (see RememberCxxGroupReachableFiles).
 */
bool LLVMBackend::CxxDeclaringFileBelongsToOtherImportGroup(
    const std::string& declaringFile) const
{
        if (declaringFile.empty() || activeCxxRequestGroup_ == nullptr
            || activeCxxRequestGroup_->primary >= cxxImportGroups_.size())
            return false;
        const std::string normalizedFile = NormalizeCxxOwnershipPath(declaringFile);
        auto reaches = [&](const CxxImportGroup& g) {
            if (g.reachableFiles.count(normalizedFile) != 0) return true;
            for (const std::string& header : g.headers)
                if (NormalizeCxxOwnershipPath(header) == normalizedFile) return true;
            return false;
        };
        const CxxImportGroup& candidate = cxxImportGroups_[activeCxxRequestGroup_->primary];
        if (candidate.reachableFiles.empty() || reaches(candidate)) return false;
        for (size_t group : activeCxxRequestGroup_->specializationGroups)
            if (group != activeCxxRequestGroup_->primary && group < cxxImportGroups_.size()
                && reaches(cxxImportGroups_[group]))
                return false;
        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
        {
            if (i == activeCxxRequestGroup_->primary || !reaches(cxxImportGroups_[i])) continue;
            if (verbose)
                std::cout << std::format("[verbose] C++ ownership probe: '{}' is not reachable "
                                         "from '{}' ({} files), only from '{}'\n",
                                         declaringFile, activeCxxRequestGroup_->label,
                                         cxxImportGroups_[activeCxxRequestGroup_->primary]
                                             .reachableFiles.size(),
                                         cxxImportGroups_[i].headers.front());
            return true;
        }
        return false;
}

/*
 * The incremental executor keeps ONE clang TU per primary group, and a request that brings in
 * dependency headers leaves them there. A later request of that primary group then sees
 * declarations its own headers never include, and its result is cached under ITS headers only - a
 * compile that never imported the other header replays them (a `<map>` request answering
 * `cppt::Tagged<long>`, member signatures naming another header's `std::function`). Drop every
 * record, signature, function template and enum whose file no header of the request reaches in
 * the TU's include graph: what a TU of the request's own headers would not have declared.
 */
void LLVMBackend::DropCxxDeclarationsOutsideRequestGroup(const CxxRequestGroup& group,
                                                          const std::vector<CxxRequestItem>& items,
                                                          const CxxIncrementalGroup& incremental,
                                                          cflat_cinterop::ExtractResult& raw) const
{
        // A `#line` renames `file`; reachability is a property of the real file.
        auto fileOf = [](const auto& entry) -> const std::string& {
            return entry.physicalFile.empty() ? entry.file : entry.physicalFile;
        };
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        auto collect = [&](const auto& list) {
            for (const auto& entry : list)
                if (!fileOf(entry).empty() && seen.insert(fileOf(entry)).second)
                    files.push_back(fileOf(entry));
        };
        collect(raw.records);
        collect(raw.sigs);
        collect(raw.functionTemplates);
        collect(raw.enums);
        const std::unordered_set<std::string> foreign =
            incremental.UnreachableFiles(CxxRequestIncludeRoots(group.headers), files);
        if (foreign.empty()) return;
        const std::string identity = items.size() == 1
            ? CanonicalCxxForeignIdentity(items.front().cxxSpelling) : std::string();
        auto isRequested = [&](const cflat_cinterop::RawRecord& r) {
            return r.name == items.front().cflatName || r.qualifiedName == items.front().cflatName
                || (!identity.empty() && (r.name == identity
                    || (!r.canonicalCtype.empty()
                        && CanonicalCxxForeignIdentity(r.canonicalCtype) == identity)));
        };
        // The requested type itself is foreign: answer as the group's own TU would, with nothing,
        // so the caller's cached negative fires however much else the shared TU had instantiated.
        if (items.size() == 1
            && std::any_of(raw.records.begin(), raw.records.end(), [&](const auto& r) {
                   return isRequested(r) && foreign.count(fileOf(r)) != 0;
               }))
        {
            if (verbose)
                std::cout << std::format("[verbose] C++ request in '{}': '{}' is declared only by "
                                         "headers outside its import groups\n",
                                         group.label, items.front().cxxSpelling);
            cflat_cinterop::ExtractResult none;
            none.longDoubleWidth = raw.longDoubleWidth;
            none.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
            none.targetTriple = std::move(raw.targetTriple);
            none.firstError = std::move(raw.firstError);
            raw = std::move(none);
            return;
        }
        size_t dropped = 0;
        auto drop = [&](auto& list) {
            const size_t before = list.size();
            std::erase_if(list, [&](const auto& entry) { return foreign.count(fileOf(entry)) != 0; });
            dropped += before - list.size();
        };
        drop(raw.records);
        drop(raw.sigs);
        drop(raw.functionTemplates);
        drop(raw.enums);
        if (verbose)
            std::cout << std::format("[verbose] C++ request in '{}' dropped {} declaration(s) "
                                     "from headers outside its import groups\n",
                                     group.label, dropped);
}

bool LLVMBackend::RequestCxxForeignType(const std::string& cflatName, const std::string& cxxSpelling,
                                        std::string& error, bool needDefinitions,
                                        bool explicitInstantiation, bool tentative,
                                        const std::string& extraSource,
                                        const std::string& prefixSource,
                                        bool persistOnSuccess,
                                        bool ownershipProbe,
                                        bool deferIncompleteCodegen)
{
        RememberCxxMangledArity(cflatName, cxxSpelling);
        std::string effectivePrefixSource = prefixSource.empty()
            ? GeneratedCxxPrefixForSpelling(cxxSpelling) : prefixSource;
        const bool incompleteTentative = tentative && !effectivePrefixSource.empty();
        const bool incompletePrefix = tentative && !effectivePrefixSource.empty()
            && !persistOnSuccess;
        const bool upgradingTentative = retryingTentativeCxxType_
            && cxxTentativeTypes_.count(cflatName) != 0;
        const bool uniquePtrSpelling = cxxSpelling.starts_with("std::unique_ptr<")
            || cxxSpelling.starts_with("std::vector<std::unique_ptr<");
        const bool incompleteUniquePtrTentative = incompleteTentative && uniquePtrSpelling;
        // A generated record in the spelling is still incomplete (not the upgrade after completion).
        const bool incompleteUniquePtrPrefix = incompletePrefix && uniquePtrSpelling;
        // Clang reports "no matching function for call to '__construct_at'" for vector<unique_ptr>
        // and "invalid application of 'sizeof' to an incomplete type '__cflat_user::BP'" directly.
        const auto allowsIncompleteUniquePtrDiagnostic = [&](const std::string& text) {
            const std::string first = FirstCxxErrorLine(text);
            constexpr std::string_view prefix =
                "invalid application of 'sizeof' to an incomplete type '";
            const bool incompleteSize = first.starts_with(prefix) && first.size() > prefix.size() + 1
                && first.back() == '\'';
            return incompleteUniquePtrTentative
                && (incompleteSize || first == "no matching function for call to '__construct_at'");
        };
        const bool rejectClangErrors = !effectivePrefixSource.empty()
            && cxxSpelling.find('<') != std::string::npos
            && !cxxSpelling.starts_with("std::shared_ptr<");
        // One attempt per CFlat identity per analysis; the outcome (including the diagnostic text)
        // is replayed so a second use site reports the same reason without re-parsing libc++.
        if (auto it = cxxForeignRequests_.find(cflatName); it != cxxForeignRequests_.end()
            && (!needDefinitions || cxxForeignDefinitions_.count(cflatName) != 0
                || !it->second.empty()))
        {
            error = it->second;
            return error.empty();
        }
        // "Not declared here" is a verdict on one import group: another group that includes the
        // declaring header (std::__iom_t6 lives in <iomanip>) must still get its own attempt.
        // A `decltype(ns::v<T>)` value request keeps its own verdict: `ns.v<T>` failing as a TYPE
        // first (the `ns.v<T>.f()` probe order) says nothing about the variable.
        const std::string groupScopedName = activeCxxRequestGroup_ == nullptr ? std::string()
            : cflatName + (cxxSpelling.starts_with("decltype(") ? "|V" : "")
                + "|G" + CxxIncrementalGroupKey(*activeCxxRequestGroup_);
        if (auto it = cxxForeignRequests_.find(groupScopedName);
            !groupScopedName.empty() && it != cxxForeignRequests_.end())
        {
            error = it->second;
            return false;
        }
        auto fail = [&](std::string why) {
            error = std::move(why);
            const bool groupLocal = !groupScopedName.empty()
                && error.find("does not name a C++ class type in the imported headers")
                       != std::string::npos;
            if (!tentative) cxxForeignRequests_[groupLocal ? groupScopedName : cflatName] = error;
            return false;
        };
        // A request compiles against ONE import group. Callers inside a group's extraction run
        // under that group's scope; a CFlat-spelled type resolves its group first.
        if (activeCxxRequestGroup_ == nullptr
            || (activeCxxRequestGroup_->headers.empty() && effectivePrefixSource.empty()))
            return fail(std::format("C++ type '{}' needs a C++ header in scope - "
                                    "import one with 'import cpp \"<header>\";'", cxxSpelling));
        const CxxRequestGroup& group = *activeCxxRequestGroup_;
        const std::string fileForCxxRequest = group.headers.empty()
            ? "cflat_cpp_struct.cpp" : group.headers.front();
        // The initial header walk can expose several class-template specializations under the
        // unparameterized C++ record name (clang drops specialization arguments from that name):
        // the reverse spelling map sends every one of them to that placeholder, whose layout and
        // members are the FIRST specialization's. A requested '$' specialization may alias onto
        // the placeholder only when the placeholder's own recorded spelling is that same
        // specialization (`std.vector` = std::vector<at::Tensor>, so signatures returning it and
        // a `std.vector<at.Tensor>` local name one class); any other specialization keeps its own.
        auto stripRecordTag = [](const std::string& spelling) { return StripCxxRecordTag(spelling); };
        auto isUnparameterizedTemplateHit = [&](const std::string& knownName,
                                                const std::string& canonical) {
            const size_t separator = cflatName.find('$');
            if (separator == std::string::npos || knownName != cflatName.substr(0, separator))
                return false;
            auto own = cxxCflatToCxxSpelling_.find(knownName);
            return own == cxxCflatToCxxSpelling_.end()
                || stripRecordTag(own->second) != stripRecordTag(canonical);
        };

        /*
         * A request parses the whole C++ TU TWICE (stage 1 without CodeGen, stage 2 with), which for
         * a libc++ specialization is the most expensive thing this compiler does. The result is a
         * pure function of the import group's headers, its defines, the instantiation spelling and
         * the emit mode, so it is cached process-wide next to the header bindings: a second file in
         * the same invocation, and every LSP reanalysis of the same file, replay it instead.
         * Cleared by nothing that ResetForReanalysis touches - the cache is static and validated by
         * header mtime/content hash, like the header cache.
         */
        CxxRequestItem item;
        item.cflatName = cflatName;
        item.cxxSpelling = cxxSpelling;
        item.needDefinitions = needDefinitions;
        item.explicitInstantiation = explicitInstantiation;
        // Instantiate members to check incomplete arguments when required. Base requests defer
        // this until their derived [cpp] struct is complete.
        item.instantiateMembers = !cxxSpelling.empty() && cxxSpelling == cxxIncompleteArgumentSpelling_
            && !uniquePtrSpelling && !deferIncompleteCodegen;
        const bool incrementalRequests = UseCxxIncrementalRequests();
        const bool incrementalSpellingSafe = CxxIncrementalSpellingSafe(cxxSpelling);
        const std::vector<CxxRequestItem> single{ item };
        const std::vector<std::string> requestArgs = BuildCxxRequestClangArgs(group);
        const std::string probeSource = BuildCxxRequestIncludes(group) + effectivePrefixSource
            + BuildCxxRequestMarkers(single, /*instantiateAll*/ true);
        const std::string probeKey =
            CxxTypeRequestCacheKey(group, item, probeSource, requestArgs, /*emitDefinitions*/ false);
        std::string requestKey = probeKey;
        /*
         * A probe clang rejects (a namespace asked for as a class, a spelling that does not
         * compile) is as deterministic as a success, so its reason is cached too; a warm compile
         * then answers it without parsing the header.
         */
        const std::string negativeKey = probeKey + "|NEG";
        const std::string scalarKey = probeKey + "|TYPEDEF";
        auto bindScalarTypedef = [&](const std::string& spelling) {
            TypeAndValue mapped;
            if (!MapCTypeToTypeAndValue(spelling, mapped, /*cxxBoundary*/ true)
                || mapped.TypeName.empty() || (mapped.IsFunctionPointer && mapped.Pointer))
                return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                        cxxSpelling));
            // A function-pointer typedef (std::new_handler) binds like `using F = function<...>`.
            if (mapped.IsFunctionPointer)
            {
                functionTypeAliases[cflatName] = mapped;
                cxxForeignRequests_[cflatName] = "";
                return true;
            }
            std::string spelled = mapped.TypeName;
            if (mapped.Pointer) spelled += "*";
            if (mapped.ElemPointer) spelled += "*";
            typeAliases[cflatName] = spelled;
            // `using Name = const char*` keeps its pointee const (a `(const char)*` alias).
            if (mapped.Pointer && !mapped.ElemPointer && CxxSpellingHasConstPointee(spelling))
                typeAliasConstMasks[cflatName] = 1;
            if (mapped.IsCxxNullptrT) cxxNullptrTAliases_.insert(cflatName);
            cxxForeignRequests_[cflatName] = "";
            return true;
        };
        /*
         * The spelling names an enum (std::byte, std::endian). entries[0] is the type head, the
         * rest its enumerators as "<marker>.<name>"; both are renamed onto cflatName, so an inline
         * namespace (std::__1) never leaks into the CFlat spelling. Unscoped enumerators are also
         * injected into the enclosing scope, as C++ does.
         */
        const std::string enumKey = probeKey + "|ENUM";
        auto bindRequestEnum = [&](const std::vector<CEnumEntry>& entries) {
            if (entries.empty())
                return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                        cxxSpelling));
            const CEnumEntry& head = entries.front();
            TypeAndValue backing;
            if (head.underlyingType.empty()
                || !MapCTypeToTypeAndValue(head.underlyingType, backing, /*cxxBoundary*/ true))
                return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                        cxxSpelling));
            if (head.isScoped) RegisterScopedEnumType(cflatName);
            importedEnumTypes_.insert(cflatName);
            RegisterEnumBackingType(cflatName, backing.TypeName);
            if (TypeAndValue promoted; !head.promotedType.empty()
                && MapCTypeToTypeAndValue(head.promotedType, promoted, /*cxxBoundary*/ true))
                enumPromotedTypes_[cflatName] = promoted.TypeName;
            for (size_t pos = 0; (pos = cflatName.find('.', pos)) != std::string::npos; ++pos)
                RegisterNamespace(cflatName.substr(0, pos));
            const size_t lastDot = cflatName.rfind('.');
            const std::string parent = lastDot == std::string::npos
                ? std::string{} : cflatName.substr(0, lastDot);
            std::vector<CEnumEntry> constants;
            for (size_t i = 1; i < entries.size(); ++i)
            {
                const size_t dot = entries[i].name.rfind('.');
                if (dot == std::string::npos) continue;
                const std::string bare = entries[i].name.substr(dot + 1);
                CEnumEntry e = entries[i];
                e.enumType = cflatName;
                e.name = cflatName + "." + bare;
                constants.push_back(e);
                if (!head.isScoped)
                {
                    e.name = parent.empty() ? bare : parent + "." + bare;
                    constants.push_back(std::move(e));
                }
            }
            RegisterCEnums(constants, group.headers.empty() ? cflatName : group.headers.front(),
                           /*cxxBoundary*/ true);
            cxxForeignRequests_[cflatName] = "";
            return true;
        };
        {
            CFileSigCacheEntry positive;
            std::string positiveMiss;
            if (TryLoadCxxTypeRequestCache(group, scalarKey, /*emitDefinitions*/ false, positive,
                                           positiveMiss, /*allowDisk*/ true)
                && positive.macros.size() == 1 && positive.macros.front().name == kScalarCxxRequestMarker)
                return bindScalarTypedef(positive.macros.front().file);
        }
        {
            CFileSigCacheEntry positive;
            std::string positiveMiss;
            if (TryLoadCxxTypeRequestCache(group, enumKey, /*emitDefinitions*/ false, positive,
                                           positiveMiss, /*allowDisk*/ true)
                && !positive.enums.empty())
                return bindRequestEnum(positive.enums);
        }
        // A `decltype(ns::v)` spelling (TryBindCxxNamespaceConstant): the folded value, bound as
        // a constexpr global under cflatName.
        const std::string constKey = probeKey + "|CONST";
        // A class-typed constant binds its class first; header-owned storage (isCxxConstexpr
        // without a folded value) comes with the companion bitcode its definitions stage emitted.
        auto bindRequestClass = [&](const std::string& classSpelling) {
            cflat_cinterop::RawSig sig;
            sig.isCxx = true;
            sig.retType = classSpelling;
            return RequestCxxSignatureTypes(sig);
        };
        auto classConstantSpelling = [&](const CFileSigCacheEntry& entry) {
            for (const auto& m : entry.macros)
                if (m.name == kClassConstantCxxRequestMarker) return m.file;
            return std::string();
        };
        auto bindRequestConstant = [&](CGlobalEntry g, const std::string& classSpelling = {},
                                       const std::string& bitcode = {}) {
            if (!classSpelling.empty())
            {
                if (!bindRequestClass(classSpelling))
                    return fail(std::format("'{}' has no bindable C++ class type '{}'",
                                            cxxSpelling, classSpelling));
                if (!bitcode.empty() && symbolSink_ == nullptr) AdoptCxxCompanionBitcode(bitcode);
            }
            g.name = cflatName;
            g.qualifiedName = cflatName;
            g.type.VariableName = cflatName;
            RegisterCGlobals({ g }, group.headers.empty() ? cflatName : group.headers.front());
            cxxForeignRequests_[cflatName] = "";
            return GetGlobalVariableNV(cflatName).Storage != nullptr
                || fail(std::format("'{}' is not a constant of a supported type", cxxSpelling));
        };
        /*
         * An unscoped enumerator through its parent scope (std.regex_constants.format_first_only,
         * std.ios_base.beg): bind its enum type, whose registration publishes the parent-scope
         * name; a path that registration does not cover gets the enumerator under cflatName.
         */
        auto bindRequestEnumerator = [&](const std::string& enumSpelling, long long value) {
            std::string spelled = enumSpelling;
            if (spelled.starts_with("enum ")) spelled.erase(0, 5);
            const std::string identity = CanonicalCxxForeignIdentity(spelled);
            if (identity.empty())
                return fail(std::format("'{}' has no bindable C++ enum type '{}'", cxxSpelling, spelled));
            if (ResolveEnumTypeName(identity).empty() && GetEnumBackingType(identity).empty())
            {
                std::string enumError;
                RequestCxxForeignType(identity, spelled, enumError);
            }
            if (GetEnumBackingType(identity).empty())
                return fail(std::format("'{}' has no bindable C++ enum type '{}'", cxxSpelling, spelled));
            if (GetGlobalVariableNV(cflatName).Storage == nullptr)
            {
                CEnumEntry e;
                e.name = cflatName;
                e.enumType = identity;
                e.value = value;
                RegisterCEnums({ e }, group.headers.empty() ? cflatName : group.headers.front(),
                               /*cxxBoundary*/ true);
            }
            cxxForeignRequests_[cflatName] = "";
            return GetGlobalVariableNV(cflatName).Storage != nullptr
                || fail(std::format("'{}' is not a constant of a supported type", cxxSpelling));
        };
        {
            CFileSigCacheEntry positive;
            std::string positiveMiss;
            if (TryLoadCxxTypeRequestCache(group, constKey, /*emitDefinitions*/ false, positive,
                                           positiveMiss, /*allowDisk*/ true))
            {
                if (positive.macros.size() == 1 && positive.enums.size() == 1
                    && positive.macros.front().name == kEnumeratorCxxRequestMarker)
                    return bindRequestEnumerator(positive.macros.front().file,
                                                 positive.enums.front().value);
                if (positive.globals.size() == 1)
                    return bindRequestConstant(positive.globals.front(),
                                               classConstantSpelling(positive), positive.cxxBitcode);
            }
        }
        {
            CFileSigCacheEntry negative;
            std::string negativeMiss;
            if (TryLoadCxxTypeRequestCache(group, negativeKey, /*emitDefinitions*/ false, negative,
                                           negativeMiss, /*allowDisk*/ true)
                && negative.macros.size() == 1 && negative.macros.front().name == kNegativeCxxRequestMarker)
            {
                if (negative.macros.front().file == kCxxAllDefaultTemplateRetry)
                    return RequestCxxForeignType(cflatName, cxxSpelling + "<>", error,
                                                 needDefinitions, explicitInstantiation, tentative,
                                                 extraSource, prefixSource, persistOnSuccess,
                                                 ownershipProbe);
                return fail(negative.macros.front().file);
            }
        }
        auto failDeterministic = [&](std::string why) {
            CFileSigCacheEntry negative;
            CMacroEntry marker;
            marker.name = kNegativeCxxRequestMarker;
            marker.file = why;
            negative.macros.push_back(std::move(marker));
            StoreCxxTypeRequestCache(group, negativeKey, /*emitDefinitions*/ false,
                                     std::move(negative), /*allowDisk*/ true, nullptr);
            return fail(std::move(why));
        };

        std::vector<CRecordEntry> records;
        std::vector<CSigEntry> requestSigs;
        // Free operator TEMPLATES over the requested specialization, harvested by the same walk
        // that collects its concrete free operators. Cached with the signatures.
        std::vector<cflat_cinterop::RawFunctionTemplate> requestTemplates;
        std::string requestBitcode;
        cflat_cinterop::ExtractResult probe;
        bool cached = false;
        bool finalCached = false;
        {
            CFileSigCacheEntry cachedEntry;
            std::string missReason;
            if (TryLoadCxxTypeRequestCache(group, probeKey, /*emitDefinitions*/ false,
                                           cachedEntry, missReason,
                                           /*allowDisk*/ true)
                && !cachedEntry.records.empty())
            {
                records = cachedEntry.records;
                requestSigs = cachedEntry.sigs;
                requestTemplates = cachedEntry.functionTemplates;
                requestBitcode = cachedEntry.cxxBitcode;
                probe.functionTemplates = requestTemplates;
                SetCInteropTargetFacts(cachedEntry.longDoubleWidth,
                                       cachedEntry.longDoubleIsIEEEDouble,
                                       cachedEntry.targetTriple);
                for (const CRecordEntry& record : records)
                    probe.records.push_back(RawRecordFromCxxCache(record));
                cached = true;
            }
        }

        auto findRequestedRecord = [&](const std::vector<cflat_cinterop::RawRecord>& list)
            -> const cflat_cinterop::RawRecord* {
            for (const auto& r : list)
                if (r.name == cflatName || r.qualifiedName == cflatName) return &r;
            const std::string requestedIdentity =
                CanonicalCxxForeignIdentity(cxxSpelling);
            for (const auto& r : list)
                if ((!requestedIdentity.empty() && r.name == requestedIdentity)
                    || (!r.canonicalCtype.empty()
                        && CanonicalCxxForeignIdentity(r.canonicalCtype)
                           == requestedIdentity))
                    return &r;
            return nullptr;
        };
        auto findRegisteredRecord = [&](const std::vector<CRecordEntry>& list)
            -> const CRecordEntry* {
            for (const auto& r : list)
                if (r.name == cflatName || r.qualifiedName == cflatName) return &r;
            const std::string requestedIdentity =
                CanonicalCxxForeignIdentity(cxxSpelling);
            for (const auto& r : list)
                if ((!requestedIdentity.empty() && r.name == requestedIdentity)
                    || (!r.canonicalCtype.empty()
                        && CanonicalCxxForeignIdentity(r.canonicalCtype)
                           == requestedIdentity))
                    return &r;
            return nullptr;
        };
        auto publishRequestSpelling = [&](const std::string& canonical) {
            auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(canonical));
            const std::string mappedName = known == cxxForeignTypeSpellings_.end()
                || isUnparameterizedTemplateHit(known->second, canonical)
                ? cflatName : known->second;
            if (!canonical.empty())
                cxxForeignTypeSpellings_[SqueezeCxxSpelling(canonical)] = mappedName;
            cxxForeignTypeSpellings_[SqueezeCxxSpelling(cxxSpelling)] = mappedName;
            cxxCflatToCxxSpelling_[cflatName] = cxxSpelling;
        };

        // Counted per spelling in the trace, cache-hit replays included, so request counts stay
        // comparable across runs; a replay is marked so the two are told apart.
        llvm::TimeTraceScope scope("CxxTypeRequest",
                                   cached ? cxxSpelling + " (cache)" : cxxSpelling);

        if (!cached || needDefinitions)
        {
            if (!cached)
            {
                // Stage 1: the member list and their canonical signatures. No CodeGen.
                {
                    llvm::TimeTraceScope stage1("CxxRequestStage1", cxxSpelling);
                    if (!RunCxxTypeRequests(group, single, /*extraSource*/ {},
                                            /*emitDefinitions*/ false, probe, error,
                                            effectivePrefixSource))
                        return fail(std::format("C++ type '{}' could not be parsed: {}",
                                                cxxSpelling, error));
                    if (!probe.invalidCxxTypeRequestError.empty())
                    {
                        if (probe.invalidCxxTypeRequestError.find(
                                cflat_cinterop::kCxxNotConstantVariableRefusal) != std::string::npos)
                            cxxNotConstantVariables_.insert(cflatName);
                        return fail(probe.invalidCxxTypeRequestError);
                    }
                }
                const bool generatedRequestMissing = generatedCxxRecords_.count(cflatName) != 0
                    && probe.records.empty();
                if (rejectClangErrors && !probe.firstError.empty()
                    && !allowsIncompleteUniquePtrDiagnostic(probe.firstError)
                    && !generatedRequestMissing)
                    return failDeterministic(std::format("C++ type '{}' could not be parsed: {}",
                                            cxxSpelling, FirstCxxErrorLine(probe.firstError)));
                // A one-shot request can harvest owner records with a requested scalar member
                // typedef. Bind the marker before the record-only class request path.
                if (!incrementalRequests && cxxSpelling.find(">::") != std::string::npos)
                    for (const auto& t : probe.typedefs)
                        if (t.name == "__cflat_req_0" && !t.underlying.empty())
                            return bindScalarTypedef(t.underlying);
                if (probe.records.empty())
                {
                    if (generatedRequestMissing && !probe.firstError.empty())
                        return fail("clang: " + FirstCxxErrorLine(probe.firstError));
                    std::vector<CEnumEntry> requestEnums;
                    for (const auto& re : probe.enums)
                    {
                        const size_t markerAt = re.name.find("req_0");
                        if (markerAt == std::string::npos
                            || (markerAt + 5 != re.name.size() && re.name[markerAt + 5] != '.'))
                            continue;
                        CEnumEntry e;
                        e.name = re.name; e.enumType = re.enumType;
                        e.underlyingType = re.underlyingType; e.promotedType = re.promotedType;
                        e.isScoped = re.isScoped; e.value = re.value;
                        e.line = re.line ? re.line : 1; e.col = re.col < 0 ? 0 : re.col;
                        // The head (no '.' after the marker) leads the list.
                        if (markerAt + 5 == re.name.size())
                            requestEnums.insert(requestEnums.begin(), std::move(e));
                        else
                            requestEnums.push_back(std::move(e));
                    }
                    for (const auto& rg : probe.globals)
                    {
                        CGlobalEntry g;
                        if (!rg.name.ends_with("req_0")) continue;
                        if (rg.isEnumerator)
                        {
                            CFileSigCacheEntry positive;
                            CMacroEntry marker;
                            marker.name = kEnumeratorCxxRequestMarker;
                            marker.file = rg.ctype;
                            positive.macros.push_back(std::move(marker));
                            CEnumEntry e;
                            e.name = cflatName;
                            e.value = rg.constantValue;
                            positive.enums.push_back(std::move(e));
                            StoreCxxTypeRequestCache(group, constKey, /*emitDefinitions*/ false,
                                                     std::move(positive), /*allowDisk*/ true, nullptr);
                            return bindRequestEnumerator(rg.ctype, rg.constantValue);
                        }
                        // A class-typed constant carries its symbol, never a folded value.
                        const bool classConstant = !rg.isCompileTimeConstant && !rg.linkageName.empty();
                        if (classConstant && !bindRequestClass(rg.ctype))
                            return fail(std::format("'{}' has no bindable C++ class type '{}'",
                                                    cxxSpelling, rg.ctype));
                        if (!MapRawGlobal(rg, g, /*cxxBoundary*/ true)) continue;
                        if (!g.bindRefusal.empty()) return fail(g.bindRefusal);
                        CFileSigCacheEntry positive;
                        bool withDefinitions = false;
                        if (classConstant)
                        {
                            CMacroEntry marker;
                            marker.name = kClassConstantCxxRequestMarker;
                            marker.file = rg.ctype;
                            positive.macros.push_back(std::move(marker));
                        }
                        // Header-owned storage (inline / constexpr): the definitions stage of
                        // this same request emits it, so a use links to the one real object.
                        if (classConstant && rg.isCxxConstexpr)
                        {
                            llvm::TimeTraceScope stage2("CxxRequestStage2", cxxSpelling);
                            cflat_cinterop::ExtractResult emitted;
                            std::string err2;
                            if (!RunCxxTypeRequests(group, single, /*extraSource*/ {},
                                                    /*emitDefinitions*/ true, emitted, err2,
                                                    effectivePrefixSource))
                                return fail(std::format("C++ variable '{}' could not be emitted: {}",
                                                        cxxSpelling, FirstCxxErrorLine(err2)));
                            SetCInteropTargetFacts(emitted);
                            positive.longDoubleWidth = emitted.longDoubleWidth;
                            positive.longDoubleIsIEEEDouble = emitted.longDoubleIsIEEEDouble;
                            positive.targetTriple = emitted.targetTriple;
                            positive.cxxBitcode = emitted.bitcode;
                            if (emitted.demandRecorded)
                            {
                                positive.cxxDemandGroupKey = CxxIncrementalGroupKey(group);
                                positive.cxxDemandReplayChunk = emitted.demandReplayChunk;
                            }
                            withDefinitions = true;
                        }
                        const std::string bitcode = positive.cxxBitcode;
                        positive.globals.push_back(g);
                        StoreCxxTypeRequestCache(group, constKey, withDefinitions,
                                                 std::move(positive), /*allowDisk*/ true, nullptr);
                        return bindRequestConstant(std::move(g), classConstant ? rg.ctype : "",
                                                   bitcode);
                    }
                    if (!requestEnums.empty() && requestEnums.front().name.ends_with("req_0"))
                    {
                        CFileSigCacheEntry positive;
                        positive.enums = requestEnums;
                        StoreCxxTypeRequestCache(group, enumKey, /*emitDefinitions*/ false,
                                                 std::move(positive), /*allowDisk*/ true, nullptr);
                        return bindRequestEnum(requestEnums);
                    }
                    // The spelling names a typedef of a builtin (::uint32_t): an alias, cached as one.
                    for (const auto& t : probe.typedefs)
                        if (t.name.ends_with("req_0") && !t.underlying.empty())
                        {
                            CFileSigCacheEntry positive;
                            CMacroEntry marker;
                            marker.name = kScalarCxxRequestMarker;
                            marker.file = t.underlying;
                            positive.macros.push_back(std::move(marker));
                            StoreCxxTypeRequestCache(group, scalarKey, /*emitDefinitions*/ false,
                                                     std::move(positive), /*allowDisk*/ true, nullptr);
                            return bindScalarTypedef(t.underlying);
                        }
                    /*
                     * A bare class-template name (`std.barrier`) whose every parameter has a
                     * default names the `<>` specialization, as in C++. Cached as a redirect so
                     * a warm compile goes straight to the `<>` request.
                     */
                    if (!cxxSpelling.ends_with(">")
                        && FirstCxxErrorLine(probe.firstError).find("requires template arguments")
                            != std::string::npos)
                    {
                        CFileSigCacheEntry redirect;
                        CMacroEntry marker;
                        marker.name = kNegativeCxxRequestMarker;
                        marker.file = kCxxAllDefaultTemplateRetry;
                        redirect.macros.push_back(std::move(marker));
                        StoreCxxTypeRequestCache(group, negativeKey, /*emitDefinitions*/ false,
                                                 std::move(redirect), /*allowDisk*/ true, nullptr);
                        return RequestCxxForeignType(cflatName, cxxSpelling + "<>", error,
                                                     needDefinitions, explicitInstantiation,
                                                     tentative, extraSource, prefixSource,
                                                     persistOnSuccess, ownershipProbe);
                    }
                    return failDeterministic(std::format(
                        "'{}' does not name a C++ class type in the imported headers", cxxSpelling));
                }
            }
            const auto* probeTarget = findRequestedRecord(probe.records);
            if (probeTarget == nullptr)
                return fail(std::format("C++ type '{}' was not present in its request result",
                                        cxxSpelling));
            // Decided per analysis from the import set; the clang probe itself is still cached
            // (memory only), so a re-analysis rejects this candidate without re-parsing its group.
            const bool ownershipRejected = ownershipProbe
                && CxxDeclaringFileBelongsToOtherImportGroup(probeTarget->file);

            // The member-signature types exposed by this class are known before its stage 2.
            // Batch the owner and those nested requests so one frontend supplies all ODR-uses.
            std::vector<CRecordEntry> probeRecords;
            MapRawRecords(probe, probeRecords);
            if (!cached)
            {
                CFileSigCacheEntry entry;
                entry.longDoubleWidth = probe.longDoubleWidth;
                entry.longDoubleIsIEEEDouble = probe.longDoubleIsIEEEDouble;
                entry.targetTriple = probe.targetTriple;
                entry.records = probeRecords;
                entry.functionTemplates = probe.functionTemplates;
                requestTemplates = probe.functionTemplates;
                for (const auto& rawSig : probe.sigs)
                {
                    CSigEntry sig;
                    if (MapRawSig(rawSig, sig)) entry.sigs.push_back(std::move(sig));
                }
                if (!probeRecords.empty())
                    StoreCxxTypeRequestCache(group, probeKey, /*emitDefinitions*/ false,
                                             std::move(entry),
                                             /*allowDisk*/ !ownershipRejected
                                                 && (!rejectClangErrors || probe.firstError.empty())
                                                 && !probe.records.empty(),
                                             /*allowDiskReason*/
                                             ownershipRejected ? "ownership probe rejected"
                                             : rejectClangErrors && !probe.firstError.empty()
                                                 ? "rejected clang error" : nullptr);
            }
            if (ownershipRejected) return fail(kCxxOwnershipProbeRejected);

            cflat_cinterop::ExtractResult raw;
            if (needDefinitions)
            {
            /*
             * Stage 2: same TU, plus an ODR-USE per member, with CodeGen. Stage 1 alone reports
             * member TEMPLATES only as uninstantiated patterns (no linkage name), so the member
             * surface it sees is SMALLER - `std::string("literal")` would be missing. LSP analysis
             * therefore runs this stage too and only throws the module away, so the editor's
             * diagnostics match the compiler's.
             */
            {
                std::string freeTemplateUses;
                const size_t templateEnd = cxxSpelling.find('<');
                const std::string requestedBase = templateEnd == std::string::npos
                    ? cxxSpelling : cxxSpelling.substr(0, templateEnd);
                const size_t namespaceEnd = requestedBase.rfind("::");
                const std::string requestedShort = namespaceEnd == std::string::npos
                    ? requestedBase : requestedBase.substr(namespaceEnd + 2);
                const std::string requestedNamespace = namespaceEnd == std::string::npos
                    ? std::string() : requestedBase.substr(0, namespaceEnd);
                for (const auto& functionTemplate : probe.functionTemplates)
                {
                    if (requestedBase.starts_with("std::")) continue;
                    if (functionTemplate.kind != cflat_cinterop::RawFunctionTemplate::Free
                        || functionTemplate.cxxSpelling.find("operator<=>") == std::string::npos
                        || (!requestedNamespace.empty()
                            && functionTemplate.cxxSpelling.find(requestedNamespace)
                                   == std::string::npos))
                        continue;
                    const bool mentionsRequestedType = std::any_of(
                        functionTemplate.parameterTypes.begin(),
                        functionTemplate.parameterTypes.end(), [&](const std::string& parameter) {
                            return parameter.find(requestedShort) != std::string::npos;
                        });
                    if (!mentionsRequestedType) continue;
                    freeTemplateUses += "__attribute__((used, noinline)) static void "
                        "__cflat_use_free_cmp___cflat_req_0(" + cxxSpelling
                        + "* a, " + cxxSpelling + "* b) { (void)(*a <=> *b); }\n";
                }
                const std::string stage2Extra =
                    CxxRequestOdrUsePreamble()
                    + BuildCxxRequestOdrUses(*probeTarget, "__cflat_req_0", "",
                                             incrementalRequests && incrementalSpellingSafe)
                    + BuildCxxRequestInheritedOdrUses(probe.records, *probeTarget,
                                                      "__cflat_req_0", "",
                                                      incrementalRequests && incrementalSpellingSafe)
                    + BuildStdFunctionCtorUse(cxxSpelling, "__cflat_req_0")
                    + BuildCxxVirtualThunks(probe.records)
                    + freeTemplateUses;
                const std::string stage2CacheExtra =
                    CxxRequestOdrUsePreamble()
                    + BuildCxxRequestOdrUses(*probeTarget, "__cflat_req_0", "", false)
                    + BuildCxxRequestInheritedOdrUses(probe.records, *probeTarget,
                                                      "__cflat_req_0", "", false)
                    + BuildStdFunctionCtorUse(cxxSpelling, "__cflat_req_0")
                    + BuildCxxVirtualThunks(probe.records)
                    + freeTemplateUses;
                const std::string stage2Source =
                    BuildCxxRequestIncludes(group) + effectivePrefixSource
                    + BuildCxxRequestMarkers(single, /*instantiateAll*/ false)
                    + stage2Extra;
                const std::string stage2CacheSource =
                    BuildCxxRequestIncludes(group) + effectivePrefixSource
                    + BuildCxxRequestMarkers(single, /*instantiateAll*/ false)
                    + stage2CacheExtra;
                requestKey =
                    CxxTypeRequestCacheKey(group, item, stage2CacheSource, requestArgs,
                                           /*emitDefinitions*/ true);
                if (verbose)
                    llvm::errs() << std::format(
                        "[verbose] C++ type request stage-2 key for {}: {}\n",
                        cxxSpelling, requestKey);
                CFileSigCacheEntry cachedEntry;
                std::string missReason;
                /*
                 * A tentative unique_ptr over a not-yet-complete CFlat record cannot instantiate
                 * its deleter, so stage 2 can only fail. Worse, in an incremental group the failed
                 * default_delete::operator() survives the rollback as an invalid instantiation, and
                 * the upgrade after completion never emits its body. Keep the stage-1 surface.
                 */
                if (incompleteUniquePtrPrefix || deferIncompleteCodegen)
                    raw = probe;
                else if (TryLoadCxxTypeRequestCache(group, requestKey, /*emitDefinitions*/ true,
                                                    cachedEntry, missReason,
                                                    /*allowDisk*/ true))
                {
                    records = std::move(cachedEntry.records);
                    requestSigs = std::move(cachedEntry.sigs);
                    requestTemplates = std::move(cachedEntry.functionTemplates);
                    requestBitcode = std::move(cachedEntry.cxxBitcode);
                    SetCInteropTargetFacts(cachedEntry.longDoubleWidth,
                                           cachedEntry.longDoubleIsIEEEDouble,
                                           cachedEntry.targetTriple);
                    finalCached = true;
                }
                if (!finalCached && !incompleteUniquePtrPrefix && !deferIncompleteCodegen)
                {
                records.clear();
                requestSigs.clear();
                requestBitcode.clear();
                llvm::TimeTraceScope stage2("CxxRequestStage2", cxxSpelling);
                std::string err2;
                cflat_cinterop::ExtractResult emitted;
                const bool emittedOk = RunCxxTypeRequests(group, single,
                                      stage2Extra,
                                      /*emitDefinitions*/ true, emitted, err2,
                                      effectivePrefixSource);
                if (!emittedOk && incrementalRequests && incrementalSpellingSafe)
                    return fail(std::format("C++ type '{}' could not be parsed: {}",
                                            cxxSpelling, FirstCxxErrorLine(err2)));
                if (emittedOk && !emitted.invalidCxxTypeRequestError.empty())
                    return fail(emitted.invalidCxxTypeRequestError);
                if (emittedOk && !emitted.records.empty())
                    raw = std::move(emitted);
                }
            }
            }
            else
                raw = std::move(probe);
            const bool generatedRequestMissing = !finalCached
                && generatedCxxRecords_.count(cflatName) != 0 && raw.records.empty();
            if (!finalCached && rejectClangErrors && !raw.firstError.empty()
                && !allowsIncompleteUniquePtrDiagnostic(raw.firstError)
                && !generatedRequestMissing)
                return fail(std::format("C++ type '{}' could not be parsed: {}",
                                        cxxSpelling, FirstCxxErrorLine(raw.firstError)));
            if (!finalCached && raw.records.empty())
            {
                if (generatedCxxRecords_.count(cflatName) != 0 && !raw.firstError.empty())
                    return fail("clang: " + FirstCxxErrorLine(raw.firstError));
                return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                        cxxSpelling));
            }
            if (!finalCached)
            {
                for (const std::string& dropped : raw.droppedCxxDefaultWrappers)
                    for (auto& record : raw.records)
                        for (auto& member : record.members)
                            for (size_t n = 0; n < member.defaultArgs.size(); ++n)
                                if (CxxDefaultWrapperName(member.linkageName, n) == dropped)
                                {
                                    member.defaultArgs[n].kind = "unsupported";
                                    member.defaultArgs[n].value.clear();
                                }
            }
            if (!finalCached && !raw.records.empty())
            {
                const auto* rawTarget = findRequestedRecord(raw.records);
                if (rawTarget == nullptr)
                    return fail(std::format("C++ type '{}' was not present in its request result",
                                            cxxSpelling));
                publishRequestSpelling(rawTarget->canonicalCtype);
            }
            if (!finalCached)
            {
                SetCInteropTargetFacts(raw);
                MapRawRecords(raw, records);
                if (!raw.functionTemplates.empty()) requestTemplates = raw.functionTemplates;
                for (const auto& rawSig : raw.sigs)
                {
                    CSigEntry sig;
                    if (MapRawSig(rawSig, sig)) requestSigs.push_back(std::move(sig));
                }
            }
            if (!finalCached)
            {
                requestBitcode = raw.bitcode;
                CFileSigCacheEntry entry;
                entry.longDoubleWidth = raw.longDoubleWidth;
                entry.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
                entry.targetTriple = raw.targetTriple;
                entry.sigs = requestSigs;
                entry.records = records;
                entry.functionTemplates = requestTemplates;
                entry.cxxBitcode = requestBitcode;
                if (raw.demandRecorded)
                {
                    entry.cxxDemandGroupKey = CxxIncrementalGroupKey(group);
                    entry.cxxDemandReplayChunk = raw.demandReplayChunk;
                }
                StoreCxxTypeRequestCache(group, requestKey, needDefinitions,
                                         std::move(entry),
                                         /*allowDisk*/ (!rejectClangErrors || raw.firstError.empty())
                                             && !raw.records.empty(),
                                         /*allowDiskReason*/
                                         rejectClangErrors && !raw.firstError.empty()
                                             ? "rejected clang error" : nullptr);
            }
        }
        if (records.empty())
            return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                    cxxSpelling));

        if (finalCached && ownershipProbe)
        {
            const auto* cachedTarget = findRegisteredRecord(records);
            if (cachedTarget != nullptr
                && CxxDeclaringFileBelongsToOtherImportGroup(cachedTarget->file))
                return fail(kCxxOwnershipProbeRejected);
        }

        const bool requestCacheReplay = finalCached || (cached && !needDefinitions);
        std::optional<llvm::TimeTraceScope> replayScope;
        if (requestCacheReplay)
            replayScope.emplace("CxxTypeRequestReplay", cxxSpelling);
        if (requestCacheReplay)
        {
            const auto* cachedTarget = findRegisteredRecord(records);
            if (cachedTarget == nullptr)
                return fail(std::format("C++ type '{}' was not present in its cache result",
                                        cxxSpelling));
            publishRequestSpelling(cachedTarget->canonicalCtype);
        }
        /*
         * A layout-only request ran no CodeGen, so the inline member bodies it bound are ASSUMED:
         * no module carries them. Remember who can emit each one; the companion link upgrades
         * that type to a definition request if the program calls a body nothing else provided.
         */
        if (!needDefinitions && symbolSink_ == nullptr)
            for (const CRecordEntry& record : records)
                for (const auto& member : record.members)
                    if (member.definitionAssumed && !member.linkageName.empty())
                        cxxAssumedBodyOwners_.emplace(member.linkageName, cflatName);
        const auto* registeredTarget = findRegisteredRecord(records);
        if (registeredTarget == nullptr)
            return fail(std::format("C++ type '{}' was not present in its mapped result",
                                    cxxSpelling));
        const std::string canonical = registeredTarget->canonicalCtype;
        if (!canonical.empty())
        {
            // Same canonical specialization under a second CFlat spelling (a typedef, or the same
            // arguments spelled twice): alias onto the ONE registration instead of a second struct.
            auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(canonical));
            if (known != cxxForeignTypeSpellings_.end() && known->second != cflatName
                && !isUnparameterizedTemplateHit(known->second, canonical))
            {
                RegisterTypeAlias(cflatName, known->second);
                cxxCflatToCxxSpelling_[cflatName] = cxxSpelling;
                cxxForeignRequests_[cflatName] = "";
                if (group.primary != static_cast<size_t>(-1))
                    cxxTypeOwnerGroup_[cflatName] = group.primary;
                if (incompleteTentative)
                    cxxTentativeTypes_.insert(cflatName);
                else
                {
                    cxxTentativeTypes_.erase(cflatName);
                    if (needDefinitions) cxxForeignDefinitions_.insert(cflatName);
                }
                /*
                 * The identity this aliases onto may have been registered from a translation unit
                 * that had no body for a member (a .cpp importing an explicit instantiation
                 * registers the specialization before any CFlat line asks for it, and clang
                 * instantiates member bodies lazily). THIS request did instantiate them, so bind
                 * exactly the members that were refused - one filtered registration each, the same
                 * mechanism TryBindRefusedCxxMember uses.
                 */
                // A lazily projected identity has not refused anything yet. Project its members
                // when this request can supply a body the retained record lacked, so the rebind
                // below sees the same refusals the eager registration would have recorded.
                if (pendingCxxMemberProjections_.count(known->second) != 0
                    || pendingCxxRecordProjections_.count(known->second) != 0)
                    if (auto old = cxxRecordEntries_.find(known->second);
                        old != cxxRecordEntries_.end()
                        && std::any_of(registeredTarget->members.begin(),
                                       registeredTarget->members.end(), [&](const auto& fresh) {
                            if (!fresh.bindRefusal.empty() || fresh.isDeleted
                                || fresh.linkageName.empty() || fresh.needsLocalDefinition)
                                return false;
                            return std::any_of(old->second.members.begin(),
                                               old->second.members.end(), [&](const auto& prior) {
                                return prior.name == fresh.name
                                    && (prior.needsLocalDefinition || !prior.bindRefusal.empty()
                                        || prior.linkageName.empty());
                            });
                        }))
                        EnsureCxxRecordProjected(known->second, true);
                if (auto infoIt = cxxClasses_.find(known->second); infoIt != cxxClasses_.end()
                    && !infoIt->second.refusedMembers.empty())
                {
                    CRecordEntry rebound = *registeredTarget;
                    rebound.name = known->second;
                    std::vector<std::string> rebindable;
                    for (const auto& m : rebound.members)
                        if (m.bindRefusal.empty() && !m.isDeleted && !m.linkageName.empty()
                            && infoIt->second.refusedMembers.count(m.name) != 0)
                            rebindable.push_back(m.name);
                    if (!rebindable.empty() && !requestBitcode.empty() && symbolSink_ == nullptr)
                        AdoptCxxCompanionBitcode(requestBitcode);
                    /*
                     * Refusals are kept per member NAME. Clear the stale one BEFORE the rebind so
                     * an overload still refused (a sibling's unrequested return type) records its
                     * fresh, retryable refusal; one bound sibling must not erase it afterwards.
                     */
                    for (const auto& memberName : rebindable)
                    {
                        if (auto updated = cxxClasses_.find(known->second);
                            updated != cxxClasses_.end())
                            updated->second.refusedMembers.erase(memberName);
                        RegisterCxxClassMembers(rebound, fileForCxxRequest, memberName);
                    }
                    cxxRecordEntries_[known->second] = rebound;
                }
                return true;
            }

            cxxForeignTypeSpellings_[SqueezeCxxSpelling(canonical)] = cflatName;
        }
        // A class member may name a second class template specialization by value (for example a
        // vector iterator). Register the outer spelling first so self-parameters and assignment
        // members do not recursively request the same specialization.
        cxxCflatToCxxSpelling_[cflatName] = cxxSpelling;
        cxxForeignRequests_[cflatName] = "";
        if (group.primary != static_cast<size_t>(-1))
            cxxTypeOwnerGroup_[cflatName] = group.primary;
        if (incompleteTentative)
            cxxTentativeTypes_.insert(cflatName);
        else
        {
            cxxTentativeTypes_.erase(cflatName);
            if (needDefinitions) cxxForeignDefinitions_.insert(cflatName);
        }
        // Nested member-type requests can refer back to the requested specialization (for
        // example an iterator's value type). Publish an opaque shell before those requests so
        // their signatures can map the self-reference; RegisterCRecords fills this shell below.
        if (dataStructures.find(cflatName) == dataStructures.end())
            CreateStructType(cflatName, {});
        {
            llvm::TimeTraceScope memberTypesScope("CxxRequestMemberTypes", cxxSpelling);
            RequestCxxMemberTypes(records, effectivePrefixSource, incompletePrefix);
        }
        if (!requestBitcode.empty() && symbolSink_ == nullptr) AdoptCxxCompanionBitcode(requestBitcode);
        // Registered BEFORE the records so a member signature naming the type itself
        // (`operator=(const vector<int>&)`, `push_back` on a nested element) maps to the CFlat name.
        {
            llvm::TimeTraceScope recordScope("CxxRequestRegisterRecords", cxxSpelling);
            RegisterCRecords(records, fileForCxxRequest);
        }
        if (upgradingTentative)
        {
            const CRecordEntry* refreshed = nullptr;
            for (const auto& candidate : records)
                if (candidate.name == cflatName) { refreshed = &candidate; break; }
            if (refreshed != nullptr)
            {
                StoreCxxRecordEntry(cflatName, *refreshed);
                /*
                 * The tentative pass saw the argument class as a forward declaration only, so
                 * every member whose signature named it by VALUE or by REFERENCE (shared_ptr's
                 * operator*, which returns 'T&') could not be mapped and was left unregistered.
                 * A member that DID bind is no better: `get()` returning `element_type*` mapped
                 * to a bare `void*` because the pointee was not a known CFlat type, so
                 * `h.get()->field` finds nothing. Re-register every bindable INSTANCE member
                 * from the complete record - drop the tentative registration for that one name
                 * first, so the refresh replaces rather than duplicates. Structors, static
                 * members and the layout are left alone; only the member signatures that could
                 * name the once-incomplete argument are rebuilt.
                 */
                std::vector<std::string> refreshNames{ "operator->" };
                {
                    std::unordered_set<std::string> queued{ "operator->" };
                    for (const auto& member : refreshed->members)
                    {
                        if (member.kind != cflat_cinterop::RawCxxMember::Instance) continue;
                        if (member.access != cflat_cinterop::AccessPublic) continue;
                        if (!member.bindRefusal.empty() || member.isDeleted) continue;
                        if (member.needsLocalDefinition || member.linkageName.empty()) continue;
                        if (!queued.insert(member.name).second) continue;
                        refreshNames.push_back(member.name);
                    }
                }
                for (const std::string& memberName : refreshNames)
                {
                    if (auto info = cxxClasses_.find(cflatName); info != cxxClasses_.end())
                    {
                        info->second.directMethods.erase(
                            std::remove_if(info->second.directMethods.begin(),
                                           info->second.directMethods.end(),
                                           [&](const auto& method) {
                                               return method.raw.name == memberName;
                                           }),
                            info->second.directMethods.end());
                        info->second.instanceMethodNames.erase(
                            std::remove(info->second.instanceMethodNames.begin(),
                                        info->second.instanceMethodNames.end(), memberName),
                            info->second.instanceMethodNames.end());
                    }
                    if (auto functions = functionTable.find(memberName);
                        functions != functionTable.end())
                    {
                        functions->second.erase(
                            std::remove_if(functions->second.begin(), functions->second.end(),
                                           [&](const auto& symbol) {
                                               return !symbol.Parameters.empty()
                                                   && symbol.Parameters[0].TypeName == cflatName;
                                           }),
                            functions->second.end());
                    }
                    // Clear before re-registering, so a still-refused overload keeps its refusal.
                    if (auto updated = cxxClasses_.find(cflatName); updated != cxxClasses_.end())
                        updated->second.refusedMembers.erase(memberName);
                    RegisterCxxClassMembers(*refreshed, fileForCxxRequest, memberName);
                }
            }
        }
        // A header import may already have laid out this class while refusing an inline or
        // template member whose body was absent from that extraction. The request above has
        // definitions enabled, but RegisterCRecords intentionally does not replace a non-opaque
        // existing layout. Rebind only those previously refused members from the fresh record;
        // this keeps overload registration idempotent while making the on-demand definition
        // request visible to the original CFlat type.
        if (auto existing = cxxClasses_.find(cflatName); existing != cxxClasses_.end())
        {
            const CRecordEntry* refreshed = nullptr;
            for (const auto& candidate : records)
                if (candidate.name == cflatName) { refreshed = &candidate; break; }
            if (refreshed != nullptr)
            {
                std::vector<std::string> rebind;
                for (const auto& [name, refusal] : existing->second.refusedMembers)
                {
                    if (!refusal.starts_with("has no definition cflat can reach:")) continue;
                    auto member = std::find_if(refreshed->members.begin(), refreshed->members.end(),
                        [&](const auto& candidate) {
                            return candidate.name == name && candidate.bindRefusal.empty()
                                && !candidate.needsLocalDefinition && !candidate.linkageName.empty();
                    });
                    if (member != refreshed->members.end()) rebind.push_back(name);
                }
                const auto oldRecord = cxxRecordEntries_.find(cflatName);
                const bool hadMissingCtor = oldRecord != cxxRecordEntries_.end()
                    && std::any_of(oldRecord->second.members.begin(), oldRecord->second.members.end(),
                        [](const auto& member) {
                            return member.kind == cflat_cinterop::RawCxxMember::Constructor
                                && member.needsLocalDefinition;
                        });
                if (hadMissingCtor)
                {
                    auto ctor = std::find_if(refreshed->members.begin(), refreshed->members.end(),
                        [](const auto& member) {
                            return member.kind == cflat_cinterop::RawCxxMember::Constructor
                                && member.bindRefusal.empty() && !member.needsLocalDefinition
                                && !member.linkageName.empty();
                        });
                    if (ctor != refreshed->members.end()) rebind.push_back(ctor->name);
                }
                const bool hadMissingDtor = oldRecord != cxxRecordEntries_.end()
                    && std::any_of(oldRecord->second.members.begin(), oldRecord->second.members.end(),
                        [](const auto& member) {
                            return member.kind == cflat_cinterop::RawCxxMember::Destructor
                                && member.needsLocalDefinition;
                        });
                if (hadMissingDtor)
                {
                    auto dtor = std::find_if(refreshed->members.begin(), refreshed->members.end(),
                        [](const auto& member) {
                            return member.kind == cflat_cinterop::RawCxxMember::Destructor
                                && member.bindRefusal.empty() && !member.needsLocalDefinition
                                && !member.linkageName.empty();
                        });
                    if (dtor != refreshed->members.end()) rebind.push_back(dtor->name);
                }
                // Clear the stale refusal BEFORE the rebind: a member that is refused again for
                // a new reason (an unrequested return type) must keep that retryable refusal.
                for (const std::string& name : rebind)
                {
                    if (auto updated = cxxClasses_.find(cflatName);
                        updated != cxxClasses_.end())
                        updated->second.refusedMembers.erase(name);
                    RegisterCxxClassMembers(*refreshed, fileForCxxRequest, name);
                }
            }
        }
        if (requestCacheReplay) RebindCxxCachedSignatures(requestSigs);
        {
            llvm::TimeTraceScope sigScope("CxxRequestRegisterSignatures",
                                          std::to_string(requestSigs.size()));
            RegisterCSignatures(requestSigs, fileForCxxRequest);
        }
        {
            llvm::TimeTraceScope templateScope("CxxRequestRegisterTemplates",
                                               std::to_string(requestTemplates.size()));
            RegisterCxxFunctionTemplates(requestTemplates, group.primary, fileForCxxRequest);
        }
        if (dataStructures.find(cflatName) == dataStructures.end())
        {
            cxxCflatToCxxSpelling_.erase(cflatName);
            return fail(std::format("C++ type '{}' could not be laid out for cflat", cxxSpelling));
        }
        return true;
    }

bool LLVMBackend::RequestGeneratedCxxType(
    const std::string& cflatName, const std::string& cxxSpelling,
    const std::string& source, llvm::StructType* literalType,
    const std::vector<DeclTypeAndValue>& fields, std::string& error, size_t ownerGroup,
    const std::set<std::string>& overrideNames)
{
        if (cflatName.empty() || cxxSpelling.empty() || source.empty() || literalType == nullptr)
        {
            error = "generated C++ struct request has incomplete source or layout";
            return false;
        }
        // The generated wrapper has C++ special members and uses the C++ global allocator for
        // heap objects, so a standalone [cpp] struct still needs the C++ link/runtime path.
        cppInteropUsed_ = true;
        generatedCxxRecords_[cflatName] = {
            literalType, fields, overrideNames, source, ownerGroup
        };

        // A generated class has no header of its own and contains only its CFlat byte block, so
        // use one synthetic header-free group instead of reparsing an unrelated import group.
        const size_t groupIndex = ownerGroup == static_cast<size_t>(-1)
            ? FindOrAddCxxImportGroup({}, {}) : ownerGroup;
        CxxRequestGroup group = MakeCxxRequestGroup(groupIndex, {});
        if (group.headers.empty()) group.label = "<generated C++ struct>";
        CxxRequestGroupScope groupScope(*this, &group);
        return RequestCxxForeignType(cflatName, cxxSpelling, error,
                                     /*needDefinitions*/ true,
                                     /*explicitInstantiation*/ false,
                                     /*tentative*/ false,
                                     /*extraSource*/ {}, source);
    }

bool LLVMBackend::GetGeneratedCxxFieldBlock(const std::string& typeName,
                                            uint64_t& start, uint64_t& length) const
{
        auto generated = generatedCxxRecords_.find(typeName);
        auto data = dataStructures.find(typeName);
        if (generated == generatedCxxRecords_.end() || data == dataStructures.end()
            || generated->second.literalType == nullptr || data->second.StructType == nullptr)
            return false;
        const auto& fields = data->second.StructFields;
        auto first = std::find_if(fields.begin(), fields.end(),
            [](const DeclTypeAndValue& f) { return f.IsCflatOwned; });
        if (first == fields.end()) return false;
        const unsigned index = static_cast<unsigned>(first - fields.begin());
        const auto& dl = module->getDataLayout();
        start = dl.getStructLayout(data->second.StructType)->getElementOffset(index);
        length = dl.getTypeAllocSize(generated->second.literalType);
        if (start + length > dl.getTypeAllocSize(data->second.StructType))
        {
            LogErrorMessage("generated C++ struct '{}' has an invalid CFlat field block",
                            { DisplayCxxClassName(typeName) });
            return false;
        }
        return true;
    }

/*
 * The nested specializations a record's members expose (a container's iterator, a smart pointer's
 * pointee), in the order RequestCxxMemberTypes requests them.
 */
void LLVMBackend::CollectCxxMemberRequestItems(const std::vector<CRecordEntry>& records,
                                               std::vector<CxxRequestItem>& out)
{
        std::unordered_set<std::string> seen;
        for (const CRecordEntry& record : records)
        {
            // Registration keeps the non-const twin of a const method (CFlat drops const), so the
            // const overload's iterator type (`const_iterator`) would be requested and never bound.
            std::unordered_set<std::string> nonConstNames;
            for (const auto& member : record.members)
                if (!member.isConst) nonConstNames.insert(member.name);
            for (const auto& member : record.members)
            {
                const bool exposesPointee = member.name == "operator->"
                                         || member.name == "operator*";
                const bool exposesIterator = member.name == "begin"
                                          || member.name == "end";
                if (!exposesPointee && !exposesIterator) continue;
                if (member.isConst && nonConstNames.count(member.name) != 0) continue;
                std::vector<std::string> spellings = member.paramTypes;
                spellings.push_back(member.retType);
                for (const std::string& spelling : spellings)
                {
                    std::string named = CxxMemberValueSpelling(spelling);
                    if (named.find('<') == std::string::npos
                        || named.find('(') != std::string::npos
                        || named.find("type-parameter-") != std::string::npos)
                        continue;
                    TypeAndValue mapped;
                    bool mappedForeign = false;
                    if (TryMapCxxForeignSpelling(named, mapped, mappedForeign) && mappedForeign) continue;
                    const std::string identity = CanonicalCxxForeignIdentity(named);
                    if (identity.empty()) continue;
                    if (!seen.insert(SqueezeCxxSpelling(named)).second) continue;
                    CxxRequestItem item;
                    item.cflatName = identity;
                    item.cxxSpelling = named;
                    item.needDefinitions = exposesIterator;
                    item.explicitInstantiation = false;
                    out.push_back(std::move(item));
                }
            }
        }
}

/*
 * A by-value `std::` specialization field of an imported C++ record is laid out as opaque bytes
 * of clang's size and alignment (RegisterCRecords). The first member access through such a field
 * requests the specialization under its resolved CFlat identity (the closure identity for
 * std::function) and returns the field's class type; the owner's layout never changes, so a
 * program that never touches the field issues no request. False when it is not such a field.
 */
