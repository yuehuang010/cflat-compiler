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

bool LLVMBackend::BindLazyCxxStdField(llvm::StructType* owner, const TypeAndValue& stored,
                                      TypeAndValue& bound)
{
        if (owner == nullptr || stored.ConstArraySize == 0 || stored.Pointer
            || stored.VariableName.empty() || !stored.ConstInnerDimensions.empty()
            || (stored.TypeName != "u8" && stored.TypeName != "u16" && stored.TypeName != "u32"
                && stored.TypeName != "u64"))
            return false;
        auto ownerIt0 = cxxOpaqueFieldOwners_.find(owner);
        if (ownerIt0 == cxxOpaqueFieldOwners_.end()) return false;
        const std::string ownerName = ownerIt0->second;
        auto dataIt = dataStructures.find(ownerName);
        if (dataIt == dataStructures.end() || dataIt->second.StructType != owner
            || cxxRecords_.count(ownerName) == 0)
            return false;
        auto recordIt = cxxRecordEntries_.find(ownerName);
        if (recordIt == cxxRecordEntries_.end() || recordIt->second.name.starts_with("std."))
            return false;
        const CRecordFieldEntry* field = nullptr;
        for (const CRecordFieldEntry& f : recordIt->second.fields)
            if (f.name == stored.VariableName) { field = &f; break; }
        if (field == nullptr || field->isBitfield || field->sizeBytes == 0) return false;

        const std::string spelling = CxxMemberValueSpelling(field->ctype);
        const bool isStringAlias = spelling == "std::string";
        if ((spelling.find('<') == std::string::npos && !isStringAlias)
            || spelling.find("__cflat_user::") != std::string::npos
            || spelling.find("type-parameter-") != std::string::npos
            || (!isStringAlias && spelling.rfind("std::", 0) != 0))
            return false;
        int angleDepth = 0;
        for (char ch : field->ctype)
        {
            if (ch == '<') ++angleDepth;
            else if (ch == '>' && angleDepth > 0) --angleDepth;
            else if (angleDepth == 0 && (ch == '*' || ch == '&' || ch == '[')) return false;
        }
        std::string className;
        const bool isStdFunction = IsTopLevelStdFunctionSpelling(spelling);
        std::string identity;
        if (isStdFunction)
        {
            auto ownerIt = cxxTypeOwnerGroup_.find(ownerName);
            if (ownerIt == cxxTypeOwnerGroup_.end())
            {
                LogErrorMessage("cannot bind C++ field '{}' of '{}' with type '{}': "
                                "no owning header request group",
                                { field->name, DisplayCxxClassName(ownerName), spelling });
                return false;
            }
            CxxRequestGroup group = MakeCxxRequestGroup(ownerIt->second, {});
            if (group.headers.empty())
            {
                LogErrorMessage("cannot bind C++ field '{}' of '{}' with type '{}': "
                                "no header is available to request the specialization",
                                { field->name, DisplayCxxClassName(ownerName), spelling });
                return false;
            }
            CxxRequestGroupScope groupScope(*this, &group);
            std::string requestError;
            className = StdFunctionSpecializationForSpelling(spelling, &requestError);
            if (className.empty())
            {
                LogErrorMessage("cannot bind C++ field '{}' of '{}' with type '{}': {}",
                                { field->name, DisplayCxxClassName(ownerName), spelling,
                                  requestError.empty() ? "unrecognized std::function signature"
                                                       : requestError });
                return false;
            }
            identity = className;
        }
        else
        {
            identity = CanonicalCxxForeignIdentity(
                CxxFieldIdentitySpelling(spelling));
            if (identity.empty()) return false;
        }

        if (!isStdFunction
            && (cxxForeignRequests_.count(identity) == 0
                || cxxForeignDefinitions_.count(identity) == 0))
        {
            auto ownerIt = cxxTypeOwnerGroup_.find(ownerName);
            if (ownerIt == cxxTypeOwnerGroup_.end()) return false;
            CxxRequestGroup group = MakeCxxRequestGroup(ownerIt->second, {});
            if (group.headers.empty()) return false;
            CxxRequestGroupScope groupScope(*this, &group);
            std::string error;
            if (!RequestCxxForeignType(identity, spelling, error, /*needDefinitions*/ true,
                                       /*explicitInstantiation*/ true))
                return false;
        }
        else if (!isStdFunction && !cxxForeignRequests_[identity].empty())
        {
            return false;
        }

        TypeAndValue mapped;
        if (isStdFunction)
            mapped.TypeName = className;
        else if (!MapCTypeToTypeAndValue(spelling, mapped, /*cxxBoundary*/ true)) return false;
        if (mapped.Pointer
            || mapped.ConstArraySize != 0 || cxxRecords_.count(mapped.TypeName) == 0)
        {
            if (!isStdFunction) return false;
            LogErrorMessage("cannot bind C++ field '{}' of '{}' with type '{}': "
                            "the requested CFlat specialization is unavailable",
                            { field->name, DisplayCxxClassName(ownerName), spelling });
            return false;
        }
        auto* classTy = GetType(mapped);
        if (classTy == nullptr || !classTy->isSized())
        {
            if (!isStdFunction) return false;
            LogErrorMessage("cannot bind C++ field '{}' of '{}' with type '{}': "
                            "the requested CFlat specialization has no usable layout",
                            { field->name, DisplayCxxClassName(ownerName), spelling });
            return false;
        }
        const llvm::DataLayout& dl = module->getDataLayout();
        const uint64_t size = (uint64_t)dl.getTypeAllocSize(classTy);
        const uint64_t align = (uint64_t)dl.getABITypeAlign(classTy).value();
        if (size != field->sizeBytes || align > field->alignBytes)
        {
            if (!isStdFunction) return false;
            LogErrorMessage("C++ field '{}' of '{}' has type '{}' whose CFlat layout "
                            "({} bytes, align {}) differs from C++ ({} bytes, align {}); "
                            "it stays opaque storage",
                            { field->name, DisplayCxxClassName(ownerName), spelling,
                              std::to_string(size), std::to_string(align),
                              std::to_string(field->sizeBytes), std::to_string(field->alignBytes) });
            return false;
        }
        bound = mapped;
        bound.VariableName = stored.VariableName;
        bound.GuardedBy = stored.GuardedBy;
        return true;
}

void LLVMBackend::RequestCxxMemberTypes(const std::vector<CRecordEntry>& records,
                                        const std::string& prefixSource,
                                        bool incompletePrefix)
{
        std::vector<CxxRequestItem> items;
        CollectCxxMemberRequestItems(records, items);
        CxxExtractionStageTimer requestStage(verbose, "member-signature type requests", items.size());
        for (const CxxRequestItem& item : items)
        {
            if (prefixSource.empty()
                && item.cxxSpelling.find("__cflat_user::") != std::string::npos)
                continue;
            std::string error;
            RequestCxxForeignType(item.cflatName, item.cxxSpelling, error, item.needDefinitions,
                                  item.explicitInstantiation, incompletePrefix, {}, prefixSource,
                                  /*persistOnSuccess*/ !incompletePrefix);
        }
}

bool LLVMBackend::HasCxxClassTemplate(const std::string& baseName) const
{
        if (baseName.empty()) return false;
        if (cxxClassTemplateParameters_.count(baseName) != 0) return true;
        return cxxClassTemplateParameters_.count(ResolveQualifiedName(baseName)) != 0;
}

bool LLVMBackend::CompleteCxxTemplateTypeArgs(const std::string& baseName,
                                               std::vector<std::string>& typeArgs)
{
        if (AnyGenericTypeTemplateNamed(baseName)) return false;
        auto found = cxxClassTemplateParameters_.find(baseName);
        if (found == cxxClassTemplateParameters_.end())
        {
            const std::string qualified = ResolveQualifiedName(baseName);
            found = cxxClassTemplateParameters_.find(qualified);
        }
        if (found == cxxClassTemplateParameters_.end()
            || typeArgs.size() >= found->second.parameters.size())
            return false;

        std::vector<std::string> completed = typeArgs;
        const auto& parameters = found->second.parameters;
        auto substituteTemplateParameter = [](std::string spelling, const std::string& name,
                                              const std::string& replacement) {
            if (name.empty()) return spelling;
            auto isIdentifierChar = [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_';
            };
            size_t pos = 0;
            while ((pos = spelling.find(name, pos)) != std::string::npos)
            {
                const size_t end = pos + name.size();
                const bool leftBoundary = pos == 0
                    || !isIdentifierChar(static_cast<unsigned char>(spelling[pos - 1]));
                const bool rightBoundary = end == spelling.size()
                    || !isIdentifierChar(static_cast<unsigned char>(spelling[end]));
                if (leftBoundary && rightBoundary)
                {
                    spelling.replace(pos, name.size(), replacement);
                    pos += replacement.size();
                }
                else
                    pos = end;
            }
            return spelling;
        };
        for (size_t i = completed.size(); i < parameters.size(); ++i)
        {
            const auto& parameter = parameters[i];
            if (!parameter.hasDefault) return false;
            if (parameter.defaultParameterIndex >= 0)
            {
                const size_t index = (size_t)parameter.defaultParameterIndex;
                if (index >= completed.size()) return false;
                completed.push_back(completed[index]);
            }
            else if (parameter.kind == 'T')
            {
                std::string defaultSpelling = parameter.defaultArgument;
                for (size_t prior = 0; prior < i; ++prior)
                {
                    std::string cxxArgument = completed[prior];
                    if (parameters[prior].kind != 'N'
                        && !CxxTemplateArgSpellingForCflatType(completed[prior], cxxArgument))
                            return false;
                    defaultSpelling = substituteTemplateParameter(
                        std::move(defaultSpelling), parameters[prior].name, cxxArgument);
                }
                TypeAndValue defaultType;
                if (!MapCTypeToTypeAndValue(defaultSpelling, defaultType,
                                            /*cxxBoundary*/ true))
                    return false;
                if (defaultType.ConstMask != 0 || defaultType.IsCxxPointeeVolatile)
                    return false;
                auto defaultSpellingRoundTrips = [&](const TypeAndValue& mappedType,
                                                     const std::string& originalSpelling) {
                    std::string cflatArgument = mappedType.TypeName;
                    cflatArgument.append((size_t)mappedType.ValuePointerDepth(), '*');
                    std::string roundTrip;
                    return CxxTemplateArgSpellingForCflatType(cflatArgument, roundTrip)
                        && CanonicalCxxForeignIdentity(roundTrip)
                            == CanonicalCxxForeignIdentity(originalSpelling);
                };
                if (defaultType.IsFunctionPointer)
                {
                    std::vector<std::pair<std::string, int>> fnParams;
                    std::vector<int64_t> fnParamConsts;
                    std::vector<bool> fnParamAliases;
                    for (const auto& fnParam : defaultType.FuncPtrParams)
                    {
                        fnParams.push_back({ fnParam.TypeName,
                            ReconcilePointerDepth(fnParam.Pointer, fnParam.PointerDepth) });
                        fnParamConsts.push_back(fnParam.ConstMask);
                        fnParamAliases.push_back(fnParam.IsAlias);
                    }
                    std::string encoded = BuildEncodedClosureName(this, true,
                        defaultType.FuncPtrReturnTypeName,
                        ReconcilePointerDepth(defaultType.FuncPtrReturnPointer,
                                              defaultType.FuncPtrReturnPointerDepth),
                        fnParams, defaultType.FuncPtrReturnConstMask, &fnParamConsts, nullptr, &fnParamAliases,
                        defaultType.FuncPtrReturnAlias);
                    RegisterEncodedClosureType(encoded, defaultType);
                    std::string roundTrip;
                    if (!CxxTemplateArgSpellingForCflatType(encoded, roundTrip)
                        || CanonicalCxxForeignIdentity(roundTrip)
                            != CanonicalCxxForeignIdentity(defaultSpelling))
                        return false;
                    completed.push_back(std::move(encoded));
                    continue;
                }
                std::string argument = defaultType.TypeName;
                argument.append((size_t)defaultType.ValuePointerDepth(), '*');
                if (!defaultSpellingRoundTrips(defaultType, defaultSpelling)) return false;
                completed.push_back(std::move(argument));
            }
            else if (parameter.kind == 'N')
            {
                if (parameter.defaultArgument.empty()) return false;
                const std::string& value = parameter.defaultArgument;
                completed.push_back(value == "true" ? "1" : value == "false" ? "0" : value);
            }
            else
                return false;
        }
        typeArgs = std::move(completed);
        return true;
}

LLVM_ATTRIBUTE_NOINLINE bool LLVMBackend::TryResolveDefaultedCxxGenericBaseAlias(
    const std::string& alias, std::string& resolvedType, std::string& error)
{
        resolvedType.clear();
        error.clear();
        if (!HasCxxImportGroup()) return false;
        std::string resolvedAlias = alias;
        if (!IsGenericBaseAlias(resolvedAlias))
        {
            resolvedAlias = ResolveQualifiedName(alias);
            if (!IsGenericBaseAlias(resolvedAlias)) return false;
        }
        std::string baseName = ResolveGenericBaseAlias(resolvedAlias);
        std::vector<std::string> typeArgs;
        if (HasCxxAliasTemplate(baseName))
            ResolveGenericAliasSpelling(baseName, typeArgs, false);
        if (!HasCxxClassTemplate(baseName)) return false;
        const bool completeArgs = CompleteCxxTemplateTypeArgs(baseName, typeArgs);
        if (!completeArgs)
        {
            auto parameters = cxxClassTemplateParameters_.find(baseName);
            if (parameters == cxxClassTemplateParameters_.end())
                parameters = cxxClassTemplateParameters_.find(ResolveQualifiedName(baseName));
            if (parameters != cxxClassTemplateParameters_.end()
                && !AnyGenericTypeTemplateNamed(baseName))
            {
                std::string missing;
                for (size_t i = typeArgs.size(); i < parameters->second.parameters.size(); ++i)
                {
                    const auto& parameter = parameters->second.parameters[i];
                    if (parameter.hasDefault) continue;
                    if (!missing.empty()) missing += ", ";
                    missing += parameter.name.empty()
                        ? std::format("#{}", i + 1) : parameter.name;
                }
                if (!missing.empty())
                    error = std::format("C++ template alias '{}' requires explicit template "
                        "argument(s) for parameter(s) without defaults: {}", alias, missing);
            }
            return false;
        }
        const std::string mangledName = MangleGenericInstance(*this, baseName, typeArgs);
        if (!TryRequestCxxType(baseName, typeArgs, mangledName, error)
            && !IsCxxForeignTypeRegistered(ResolveTypeAlias(mangledName)))
            return false;
        error.clear();
        resolvedType = ResolveTypeAlias(mangledName);
        return !resolvedType.empty();
}

/*
 * Entry point for both ParseDeclarationSpecifiers copies: build the C++ spelling from the CFlat
 * name (`std.vector` + { int } -> `std::vector<int>`) and request it. `typeArgs` empty means a
 * plain dotted name (`std.string`), which may still be a typedef for a specialization.
 */
bool LLVMBackend::TryRequestCxxType(const std::string& baseName,
                                    const std::vector<std::string>& typeArgs,
                                    const std::string& cflatName, std::string& error,
                                    bool deferIncompleteCodegen)
{
        error.clear();
        if (!HasCxxImportGroup()) return false;
        if (typeArgs.empty() && GetGlobalVariableNV(cflatName).Storage != nullptr)
            return false;
        if (!typeArgs.empty() && activeCxxRequestGroup_ == nullptr)
        {
            std::string cxxBase = baseName;
            size_t pos = 0;
            while ((pos = cxxBase.find('.', pos)) != std::string::npos)
            { cxxBase.replace(pos, 1, "::"); pos += 2; }
            if (auto owner = cxxTemplateOwnerGroup_.find(cxxBase);
                owner != cxxTemplateOwnerGroup_.end()
                && cxxCflatToCxxSpelling_.count(cflatName) != 0
                && CxxTemplateDefineMismatch(cxxBase,
                    cxxCflatToCxxSpelling_.at(cflatName),
                    cflatName, owner->second, {}, error))
                return false;
        }
        // Bare names are C++ candidates only when a header published that global template name.
        const bool unqualifiedTemplateRequest = !typeArgs.empty()
            && baseName.find('.') == std::string::npos
            && std::any_of(cxxImportGroups_.begin(), cxxImportGroups_.end(),
                [&](const CxxImportGroup& group) {
                    return group.publishedNames.count(baseName) != 0;
                });
        if (baseName.find('.') == std::string::npos
            && !(typeArgs.empty() && cxxLazyAliasSpecializations_.count(baseName) != 0)
            && !unqualifiedTemplateRequest)
        {
            /*
             * An unknown plain name may be a global C typedef the imported headers reach but do
             * not bind (`uint32_t` through <cstdint>): clang resolves it, so ask. Only a typedef
             * of a builtin binds; anything else is the ordinary unknown-type path.
             */
            if (!typeArgs.empty() || baseName != cflatName || baseName.empty()
                || baseName.find_first_of("<>$*[ ") != std::string::npos
                || IsKnownTypeName(baseName) || FindFunctionTypeAlias(baseName) != nullptr
                || IsTypeArgTypeKey(baseName) || IsCxxLazyAliasSpecialization(baseName))
                return false;
            if (auto seen = cxxForeignRequests_.find(baseName); seen != cxxForeignRequests_.end())
            {
                error = seen->second;
                return error.empty();
            }
            static const std::unordered_set<std::string> keywords = {
                "unsigned", "signed", "const", "volatile", "struct", "union", "enum", "class" };
            // A generic's own type parameter (T, TSelf) is not a C name; asking would parse a header.
            auto isTypeParam = [&](const auto& paramMap) {
                for (const auto& [owner, params] : paramMap)
                    if (std::find(params.begin(), params.end(), baseName) != params.end()) return true;
                return false;
            };
            if (keywords.count(baseName) != 0 || isTypeParam(gts.genericStructTypeParams)
                || isTypeParam(gts.genericFunctionTypeParams) || isTypeParam(gts.genericInterfaceTypeParams))
            {
                cxxForeignRequests_[baseName] = "unresolved";
                return false;
            }
            std::string scalarError;
            const bool bound = RequestCxxTypeInOwningGroup(baseName, baseName, "::" + baseName, {},
                                                           scalarError);
            if (!bound) cxxForeignRequests_[baseName] = scalarError.empty() ? "unresolved" : scalarError;
            return bound;
        }
        if (IsCxxForeignTypeRegistered(cflatName) && !cxxTentativeTypes_.count(cflatName))
        {
            // A spelling recorded without any struct behind it (a member signature published the
            // spelling, but no request ever materialized the class under THIS identity) is not a
            // registration a declaration can use: request it now under its recorded spelling.
            if (dataStructures.count(cflatName) == 0 && ResolveTypeAlias(cflatName) == cflatName)
            {
                auto spellingIt = cxxCflatToCxxSpelling_.find(cflatName);
                auto ownerIt = cxxTypeOwnerGroup_.find(cflatName);
                if (verbose)
                    std::cout << std::format("[verbose]   C++ type '{}' was spelled but never "
                                             "materialized; requesting '{}'\n",
                                             cflatName, spellingIt->second);
                if (ownerIt != cxxTypeOwnerGroup_.end())
                {
                    CxxRequestGroup group = MakeCxxClassRequestGroup(ownerIt->second, cflatName);
                    if (!group.headers.empty())
                    {
                        CxxRequestGroupScope groupScope(*this, &group);
                        cxxForeignRequests_.erase(cflatName);
                        return RequestCxxForeignType(cflatName, spellingIt->second, error,
                                                     /*needDefinitions*/ true,
                                                     /*explicitInstantiation*/ true);
                    }
                }
                if (activeCxxRequestGroup_ != nullptr)
                {
                    cxxForeignRequests_.erase(cflatName);
                    return RequestCxxForeignType(cflatName, spellingIt->second, error,
                                                 /*needDefinitions*/ true,
                                                 /*explicitInstantiation*/ true);
                }
            }
            // A nested type exposed through operator* may have been registered layout-only.
            // A later explicit use can need inline member bodies, so upgrade that registration
            // instead of treating the layout-only request as complete.
            if (cxxForeignDefinitions_.count(cflatName) == 0
                && cxxForeignRequests_.count(cflatName) != 0)
            {
                auto spellingIt = cxxCflatToCxxSpelling_.find(cflatName);
                auto ownerIt = cxxTypeOwnerGroup_.find(cflatName);
                if (spellingIt != cxxCflatToCxxSpelling_.end()
                    && ownerIt != cxxTypeOwnerGroup_.end())
                {
                    CxxRequestGroup group = MakeCxxClassRequestGroup(ownerIt->second, cflatName);
                    if (!group.headers.empty())
                    {
                        CxxRequestGroupScope groupScope(*this, &group);
                        return RequestCxxForeignType(cflatName, spellingIt->second, error,
                                                     /*needDefinitions*/ true,
                                                     /*explicitInstantiation*/ true);
                    }
                }
            }
            return true;
        }
        if (typeArgs.empty())
            if (auto lazy = cxxLazyAliasSpecializations_.find(baseName);
                lazy != cxxLazyAliasSpecializations_.end())
            {
                const std::string& spelling = lazy->second;
                if (activeCxxRequestGroup_ != nullptr)
                    return RequestCxxForeignType(baseName, spelling, error);
                std::string cxxBase = spelling.substr(0, spelling.find('<'));
                while (!cxxBase.empty() && std::isspace((unsigned char)cxxBase.back())) cxxBase.pop_back();
                return RequestCxxTypeInOwningGroup(cxxBase, baseName, spelling, {}, error);
            }
        // A CFlat generic or generic interface owns its own instantiation path and its own
        // mangling; the two identities never mix (`list<std.vector<int>>` is a CFlat instantiation
        // whose element happens to be foreign).
        const StructData existingForeignShell = GetDataStructure(cflatName);
        const bool isOpaqueForeignShell = cflatName.find('$') != std::string::npos
            && existingForeignShell.StructType != nullptr
            && existingForeignShell.StructType->isOpaque();
        const bool cxxRecordTemplate = cxxRecords_.count(baseName) != 0;
        auto skipped = [&](const char* why) {
            if (verbose)
                std::cout << std::format("[verbose]   C++ type request for '{}' skipped: {}\n",
                                         cflatName, why);
            return false;
        };
        if (!isOpaqueForeignShell
            && !cxxRecordTemplate
            && (AnyGenericTypeTemplateNamed(baseName) || IsGenericInterfaceTemplateName(baseName)))
            return skipped("the name is a CFlat generic template");
        if (IsKnownTypeName(typeArgs.empty() ? baseName : cflatName)
            && !(deferIncompleteCodegen && cxxTentativeTypes_.count(cflatName)))
        {
            if (!isOpaqueForeignShell)
                return skipped("the name is already a known non-foreign type");
        }
        /*
         * An unknown dotted name is only a CANDIDATE foreign type when its leading segment came
         * from a C++ import. Without this, every forward reference to a CFlat type declared later
         * in the file (the ForwardRefScanner has not seen it yet) paid two clang parses of the
         * standard library, and a header that happens to declare the same qualified name would
         * capture the identity the CFlat definition is about to claim.
         */
        {
            const std::string lead = baseName.substr(0, baseName.find('.'));
            if (!unqualifiedTemplateRequest && cxxForeignNamespaces_.count(lead) == 0
                && !IsCxxForeignTypeRegistered(lead)
                && cxxCflatToCxxSpelling_.count(lead) == 0)
                return skipped("its leading segment is not an imported C++ namespace");
        }
        /*
         * A dotted name whose every segment was harvested as a C++ NAMESPACE (`std.chrono`) names a
         * namespace path, never a class. Probing it as a type finds nothing in ANY import group, so
         * the request would parse every group's headers once for a guaranteed failure.
         */
        if (typeArgs.empty() && baseName.find('.') != std::string::npos
            && cxxRecords_.count(baseName) == 0
            && cxxCflatToCxxSpelling_.count(baseName) == 0
            && !IsCxxForeignTypeRegistered(baseName))
        {
            if (IsCxxNamespacePath(baseName))
                return skipped("every segment names an imported C++ namespace");
        }
        return RequestCxxType(baseName, typeArgs, cflatName, error, deferIncompleteCodegen);
}

/*
 * A name under an imported C++ namespace that nothing bound (a system header binds nothing at
 * import): ask clang for `decltype(<base<args>>)` and, when it names a constant scalar variable
 * or variable-template specialization (std::numbers::pi, pi_v<double>), bind its folded value as
 * a constexpr global `cflatName`. A class-typed const object (std::chrono::February, std::nullopt,
 * a tag object) binds as the real C++ object instead: its class is requested, and header-owned
 * storage is emitted by the request's definitions stage. Lazy, one request per name, disk-cached
 * like a type request.
 */
/*
 * A bare name nothing else resolved. After `#include <climits>` C++ sees every macro the
 * preprocessor defined, but an import binds only the macros of its own (in-scope) headers, and a
 * libc++ header binds nothing at import. So ask each import group's TU - the one its requests
 * parse anyway, never a second one - for an object-like macro of that name defined in a system
 * header, fold it with a one-line probe chunk, and cache the answer (or its absence) per group so
 * a warm compile parses nothing. Only reached once every CFlat and C++ lookup failed, so a
 * system macro never shadows a CFlat identifier or a C++ declaration.
 */
// An object-like system macro that names an lvalue (`errno` is `(*__error())`): a generated
// wrapper returns its address, and every use reads or writes through it, as C++ does.
static const std::string kCxxLvalueMacroMarker = "\x1f" "CFLAT_LVALUE_MACRO";
// A function-like system macro (`assert`): bound at its call through a generated wrapper.
static const std::string kCxxFunctionMacroMarker = "\x1f" "CFLAT_FUNCTION_MACRO";
static const std::string kCxxImportedFunctionMacroMarker = "\x1f" "CFLAT_IMPORTED_FUNCTION_MACRO";

bool LLVMBackend::BindCxxLvalueMacro(const CxxRequestGroup& group, const std::string& name,
                                     const std::string& file)
{
        cppInteropUsed_ = true;
        const std::string wrapperName = "__cflat_mref_" + name;
        const std::string source = "extern \"C\" __attribute__((weak)) auto* " + wrapperName
            + "() noexcept { return &(" + name + "); }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        std::string error;
        if (!RequestGeneratedCxxWrapper(group, source, wrapperName, "MACRO_LVALUE", signature,
                                        error)
            || !signature.params.empty() || !signature.ret.Pointer
            || signature.ret.IsFunctionPointer || signature.ret.TypeName.empty())
        {
            if (verbose)
                std::cout << std::format("[verbose] C++ system macro '{}' is no lvalue: {}\n", name,
                                         error);
            return false;
        }
        signature.name = wrapperName;
        RegisterCSignatures({ signature }, file.empty() ? group.headers.front() : file);
        if (GetFunction(wrapperName) == nullptr) return false;
        CxxLvalueMacro bound;
        bound.wrapper = wrapperName;
        bound.type = signature.ret;
        if (bound.type.ElemPointer) bound.type.ElemPointer = false;
        else bound.type.Pointer = false;
        cxxLvalueMacros_[name] = std::move(bound);
        return true;
}

/*
 * A call of a function-like macro whose replacement list names no parameter: C++ evaluates none
 * of its arguments, so the call site evaluates none either and this wrapper invokes the macro
 * with `argc` placeholder arguments in its owner group. Returns the wrapper's name, or empty.
 */
std::string LLVMBackend::RequestCxxArgumentFreeMacroCall(const std::string& name, size_t argc,
                                                         std::string& error)
{
        std::string wrapperName = std::format("__cflat_mcall_{}_{}", name, argc);
        if (GetFunction(wrapperName) != nullptr) return wrapperName;
        auto owner = cxxFunctionOwnerGroup_.find(name);
        if (owner == cxxFunctionOwnerGroup_.end() || owner->second >= cxxImportGroups_.size())
        {
            error = std::format("C++ macro '{}' has no import group", name);
            return {};
        }
        CxxRequestGroup group = MakeCxxRequestGroup(owner->second, {});
        std::string call = name + "(";
        for (size_t i = 0; i < argc; ++i) call += i == 0 ? "0" : ", 0";
        call += ")";
        const std::string source = "extern \"C\" __attribute__((weak)) decltype(auto) " + wrapperName
            + "() noexcept(noexcept(" + call + ")) { return " + call + "; }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        if (!RequestGeneratedCxxWrapper(group, source, wrapperName, "MACRO_NOARG", signature, error))
        {
            error = std::format("C++ macro '{}' cannot be called with these arguments (clang: {})",
                                name, FirstCxxErrorLine(error));
            return {};
        }
        signature.name = wrapperName;
        RegisterCSignatures({ signature }, group.headers.front());
        return GetFunction(wrapperName) != nullptr ? wrapperName : std::string();
}

bool LLVMBackend::IsCxxMacroFunctionTemplate(const std::string& name) const
{
        auto templates = gts.genericFunctionTemplates.find(name);
        if (templates == gts.genericFunctionTemplates.end() || templates->second.empty()) return false;
        return std::all_of(templates->second.begin(), templates->second.end(), [&](auto* context) {
            return cxxMacroTemplateContexts_.count(context) != 0;
        });
}

bool LLVMBackend::TryGetCxxMacroExpansionNullZero(const std::string& name,
                                                   const std::string& invocation,
                                                   CMacroEntry& out)
{
        if (!IsImportedCxxFunctionMacro(name) || invocation.empty()) return false;
        auto owner = cxxFunctionOwnerGroup_.find(name);
        if (owner == cxxFunctionOwnerGroup_.end() || owner->second >= cxxImportGroups_.size())
            return false;

        const CxxRequestGroup group = MakeCxxRequestGroup(owner->second, {});
        const std::string memoryKey = std::to_string(owner->second) + "\x1f" + name
            + "\x1f" + invocation;
        if (auto cached = cxxMacroExpansionZeroCache_.find(memoryKey);
            cached != cxxMacroExpansionZeroCache_.end())
        {
            out.name = name;
            out.intTypeName = cached->second.second;
            out.isIntegerLiteralZero = cached->second.first;
            out.isIntegerLiteralToken = cached->second.first;
            return cached->second.first;
        }

        const std::string source = "static const __auto_type __cflat_macro_0_call = "
            + invocation + ";\n";
        const std::vector<std::string> args = BuildCxxRequestClangArgs(group);
        const std::string cacheSource = BuildCxxRequestIncludes(group) + source;
        const std::string cacheKey = CxxTypeRequestCacheKey(
            group, "|function-macro-expansion|" + name + "|" + invocation,
            cacheSource, args, /*emitDefinitions*/ false) + "|MACROEXP";

        CFileSigCacheEntry cachedEntry;
        std::string missReason;
        if (TryLoadCxxTypeRequestCache(group, cacheKey, /*emitDefinitions*/ false,
                                       cachedEntry, missReason, /*allowDisk*/ true)
            && cachedEntry.macros.size() == 1 && cachedEntry.macros.front().name == name)
        {
            out = cachedEntry.macros.front();
            const bool isZero = out.isIntegerLiteralZero && out.isIntegerLiteralToken;
            cxxMacroExpansionZeroCache_.emplace(memoryKey,
                std::pair{ isZero, out.intTypeName });
            if (verbose)
                std::cout << std::format("[verbose] C++ macro expansion cache hit '{}': {}\n",
                    name, isZero ? "integer literal zero" : "not integer literal zero");
            return isZero;
        }

        cflat_cinterop::CxxMacroProbe probe;
        probe.name = name;
        probe.file = group.headers.empty() ? std::string{} : group.headers.front();
        cflat_cinterop::ExtractRequest req;
        req.mainFileName = "cflat_cpp_macro_expansion.cpp";
        req.cxxMode = true;
        req.cxxImportGroupKey = CxxIncrementalGroupKey(group);
        req.args = args;
        req.verbose = verbose;
        req.cxxMacroProbes = { probe };
        req.cxxWrapperBatch = true;
        req.emitDefinitions = false;
        req.demandOnlyDefinitions = false;
        req.assumeInlineDefinitions = true;
        req.skipFunctionBodies = true;
        req.requireInScope = false;

        CMacroEntry result;
        result.name = name;
        result.file = probe.file;
        std::string error;
        CxxIncrementalGroup* incremental = GetCxxIncrementalGroup(group, error);
        cflat_cinterop::ExtractResult raw;
        const bool parsed = incremental != nullptr
            && incremental->ParseRequest(req, source, raw, error);
        if (parsed && raw.macros.size() == 1)
            ClassifyRawMacro(raw.macros.front(), result, /*cxxBoundary*/ true);

        const bool isZero = result.isIntegerLiteralZero && result.isIntegerLiteralToken;
        CFileSigCacheEntry stored;
        stored.macros.push_back(result);
        StoreCxxTypeRequestCache(group, cacheKey, /*emitDefinitions*/ false,
                                 std::move(stored), /*allowDisk*/ true, nullptr);
        cxxMacroExpansionZeroCache_.emplace(memoryKey,
            std::pair{ isZero, result.intTypeName });
        out = std::move(result);
        if (verbose)
            std::cout << std::format("[verbose] C++ macro expansion probe '{}': {}\n",
                name, isZero ? "integer literal zero" : "not integer literal zero");
        return isZero;
}

bool LLVMBackend::IsCFlatVariableBinding(const std::string& name)
{
        // Same lookup order as the postfix path: locals and parameters, then implicit-this fields,
        // then globals. A global that is only a bound C++ macro is not a CFlat binding.
        if (GetScopedLocalOrArgument(name).Storage != nullptr) return true;
        if (GetFunctionArgument(name).GetValue() != nullptr) return true;
        if (HasMemberVariable(name)) return true;
        // A bare name inside `namespace a.b` also binds a.b.name and a.name.
        for (std::string ns = currentNamespace_; !ns.empty();)
        {
            const std::string qualified = ns + "." + name;
            if (GetGlobalVariableNV(qualified).Storage != nullptr && !IsCxxMacroGlobalName(qualified))
                return true;
            const size_t dot = ns.rfind('.');
            ns = dot == std::string::npos ? std::string() : ns.substr(0, dot);
        }
        return GetGlobalVariableNV(name).Storage != nullptr && !IsCxxMacroGlobalName(name);
}

const std::vector<LLVMBackend::FunctionSymbol>* LLVMBackend::FindCFlatCallCandidates(
    const std::string& name) const
{
        for (std::string ns = currentNamespace_; !ns.empty();)
        {
            auto it = functionTable.find(ns + "." + name);
            if (it != functionTable.end() && !it->second.empty()) return &it->second;
            const size_t dot = ns.rfind('.');
            ns = dot == std::string::npos ? std::string() : ns.substr(0, dot);
        }
        auto it = functionTable.find(name);
        return it == functionTable.end() || it->second.empty() ? nullptr : &it->second;
}

bool LLVMBackend::ParameterMayTakeNullConstant(const TypeAndValue& parameter)
{
        if (parameter.Pointer || parameter.ElemPointer || parameter.PointerDepth > 0
            || parameter.IsFunctionPointer || parameter.IsCxxRefToPointer || parameter.IsCxxNullptrT
            || parameter.IsInterface || parameter.IsInterfacePointer || parameter.IsArrayView
            || !parameter.ConstInnerDimensions.empty())
            return true;
        static const std::unordered_set<std::string> kArithmetic = {
            "bool", "char", "signed char", "unsigned char", "short", "unsigned short",
            "int", "unsigned", "unsigned int", "long", "unsigned long", "long long",
            "unsigned long long", "float", "double",
        };
        return kArithmetic.count(parameter.TypeName) == 0;
}

bool LLVMBackend::TryBindCxxSystemMacro(const std::string& name)
{
        if (!HasCxxImportGroup() || name.empty() || name.starts_with("__cflat")) return false;
        if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isalnum(c) != 0 || c == '_'; })
            || std::isdigit((unsigned char)name.front()) != 0)
            return false;
        // True only for the call that binds it, so a caller's retry cannot loop.
        if (GetGlobalVariableNV(name).Storage != nullptr) return false;
        if (!cxxSystemMacroLookups_.insert(name).second) return false;
        struct Candidate
        {
            CxxRequestGroup group;
            std::string key;
            size_t primary = 0;
            bool live = false;
        };
        std::vector<Candidate> pending;
        std::optional<Candidate> hit;
        CMacroEntry hitEntry;
        std::vector<CFunctionMacroEntry> hitFunctionMacros;
        /*
         * Every group's stored answer is loaded, not just up to the first hit: a load notes its
         * entry in the group's demand-companion key, so a warm compile must note exactly what
         * the cold one stored or that companion misses and its group is re-parsed.
         */
        for (size_t index = 0; index < cxxImportGroups_.size(); ++index)
        {
            Candidate c;
            c.primary = index;
            c.group = MakeCxxRequestGroup(index, {});
            c.key = CxxTypeRequestCacheKey(c.group, "|system-macro|" + name,
                                           BuildCxxRequestIncludes(c.group),
                                           BuildCxxRequestClangArgs(c.group), false)
                + "|MACRO";
            CFileSigCacheEntry entry;
            std::string miss;
            if (TryLoadCxxTypeRequestCache(c.group, c.key, /*emitDefinitions*/ false, entry, miss,
                                           /*allowDisk*/ true)
                && entry.macros.size() == 1)
            {
                if (entry.macros.front().name == name && !hit.has_value())
                {
                    hitEntry = entry.macros.front();
                    hitFunctionMacros = entry.funcMacros;
                    hit = std::move(c);
                }
                continue;   // a hit, or a remembered absence
            }
            c.live = cxxIncrementalGroups_.count(CxxIncrementalGroupKey(c.group)) != 0;
            pending.push_back(std::move(c));
        }
        if (hit.has_value())
        {
            if (hitEntry.aliasTarget == kCxxLvalueMacroMarker)
                return BindCxxLvalueMacro(hit->group, name, hitEntry.file);
            if (hitEntry.aliasTarget == kCxxFunctionMacroMarker
                || hitEntry.aliasTarget == kCxxImportedFunctionMacroMarker)
            {
                cxxFunctionOwnerGroup_[name] = hit->primary;
                if (hitEntry.aliasTarget == kCxxImportedFunctionMacroMarker)
                {
                    importedCxxFunctionMacros_.insert(name);
                    auto macro = std::find_if(hitFunctionMacros.begin(), hitFunctionMacros.end(),
                        [&](const CFunctionMacroEntry& entry) { return entry.name == name; });
                    bool hasTranslatedTemplate = false;
                    if (macro != hitFunctionMacros.end())
                    {
                        RegisterCFunctionMacros({ *macro }, macro->file);
                        ProcessPendingMacroSources();
                        hasTranslatedTemplate = gts.genericFunctionTemplates.count(name) != 0;
                    }
                    if (hasTranslatedTemplate) cxxFunctionMacros_.erase(name);
                    else cxxFunctionMacros_.insert(name);
                    if (hitEntry.value == 1 && !hasTranslatedTemplate)
                        cxxArgumentFreeMacros_.insert(name);
                }
                else
                {
                    cxxFunctionMacros_.insert(name);
                    if (hitEntry.value == 1) cxxArgumentFreeMacros_.insert(name);
                }
                return true;
            }
            RegisterCMacros({ hitEntry });
            cxxConstantMacroNames_.insert(name);
            cxxConstantMacroOwnerGroup_[name] = hit->primary;
            return GetGlobalVariableNV(name).Storage != nullptr;
        }
        if (pending.empty() || !UseCxxIncrementalRequests()) return false;
        // A live TU answers without a parse; the others only when it does not.
        std::stable_partition(pending.begin(), pending.end(),
                              [](const Candidate& c) { return c.live; });
        for (const Candidate& c : pending)
        {
            auto remember = [&](CMacroEntry entry) {
                CFileSigCacheEntry stored;
                stored.macros.push_back(std::move(entry));
                StoreCxxTypeRequestCache(c.group, c.key, /*emitDefinitions*/ false,
                                         std::move(stored), /*allowDisk*/ true, nullptr);
            };
            auto rememberAbsent = [&] {
                CMacroEntry marker;
                marker.name = kNegativeCxxRequestMarker;
                remember(std::move(marker));
            };
            std::string error;
            if (verbose)
                std::cout << std::format("[verbose] C++ system macro lookup '{}' in group '{}' ({})\n",
                                         name, c.group.label, c.live ? "live" : "new parse");
            CxxIncrementalGroup* incremental = GetCxxIncrementalGroup(c.group, error);
            if (incremental == nullptr) continue;
            CxxIncrementalGroup::MacroLookup found;
            if (!incremental->LookupMacro(name, found))
            {
                rememberAbsent();
                continue;
            }
            if (found.functionLike && !found.systemHeader)
            {
                const bool usesParameter = [&] {
                    std::vector<std::string> params = found.params;
                    if (found.variadic) params.push_back("__VA_ARGS__");
                    size_t start = 0;
                    while (start < found.body.size())
                    {
                        size_t end = found.body.find(' ', start);
                        if (end == std::string::npos) end = found.body.size();
                        const std::string token = found.body.substr(start, end - start);
                        if (std::find(params.begin(), params.end(), token) != params.end())
                            return true;
                        start = end + 1;
                    }
                    return false;
                }();
                CFunctionMacroEntry imported;
                imported.name = name;
                imported.params = found.params;
                imported.body = found.body;
                imported.file = found.file;
                imported.line = found.line;
                imported.col = found.col;
                RegisterCFunctionMacros({ imported }, found.file);
                ProcessPendingMacroSources();
                const bool hasTranslatedTemplate = gts.genericFunctionTemplates.count(name) != 0;
                CMacroEntry marker;
                marker.name = name;
                marker.aliasTarget = kCxxImportedFunctionMacroMarker;
                marker.file = found.file;
                marker.value = usesParameter ? 0 : 1;
                CFileSigCacheEntry stored;
                stored.macros.push_back(std::move(marker));
                stored.funcMacros.push_back(imported);
                StoreCxxTypeRequestCache(c.group, c.key, /*emitDefinitions*/ false,
                                         std::move(stored), /*allowDisk*/ true, nullptr);
                if (hasTranslatedTemplate) cxxFunctionMacros_.erase(name);
                else cxxFunctionMacros_.insert(name);
                importedCxxFunctionMacros_.insert(name);
                cxxFunctionOwnerGroup_[name] = c.primary;
                if (!usesParameter && !hasTranslatedTemplate) cxxArgumentFreeMacros_.insert(name);
                return true;
            }
            if (found.functionLike)
            {
                // Its arguments' types are known only at the call: RequestCxxFreeFunction binds it.
                // A replacement list naming no parameter (assert under NDEBUG is `((void)0)`)
                // evaluates no argument, so its call must not evaluate them either.
                const bool usesParameter = [&] {
                    std::vector<std::string> params = found.params;
                    if (found.variadic) params.push_back("__VA_ARGS__");
                    size_t start = 0;
                    while (start < found.body.size())
                    {
                        size_t end = found.body.find(' ', start);
                        if (end == std::string::npos) end = found.body.size();
                        const std::string token = found.body.substr(start, end - start);
                        if (std::find(params.begin(), params.end(), token) != params.end())
                            return true;
                        start = end + 1;
                    }
                    return false;
                }();
                CMacroEntry marker;
                marker.name = name;
                marker.aliasTarget = kCxxFunctionMacroMarker;
                marker.file = found.file;
                marker.value = usesParameter ? 0 : 1;
                remember(std::move(marker));
                cxxFunctionMacros_.insert(name);
                cxxFunctionOwnerGroup_[name] = c.primary;
                if (!usesParameter) cxxArgumentFreeMacros_.insert(name);
                return true;
            }
            cflat_cinterop::CxxMacroProbe probe;
            probe.name = name;
            probe.file = found.file;
            probe.line = found.line;
            probe.col = found.col;
            cflat_cinterop::ExtractRequest req;
            req.mainFileName = "cflat_cpp_macro_probe.cpp";
            req.cxxMode = true;
            req.cxxImportGroupKey = CxxIncrementalGroupKey(c.group);
            req.args = BuildCxxRequestClangArgs(c.group);
            req.verbose = verbose;
            req.cxxMacroProbes = { probe };
            req.cxxWrapperBatch = true;
            req.emitDefinitions = false;
            req.demandOnlyDefinitions = false;
            req.assumeInlineDefinitions = true;
            req.skipFunctionBodies = true;
            req.wantMacros = false;
            req.requireInScope = false;
            const std::string source =
                "static const __auto_type __cflat_macro_0_" + name + " = " + name + ";\n";
            cflat_cinterop::ExtractResult raw;
            CMacroEntry entry;
            const bool folded = incremental->ParseRequest(req, source, raw, error)
                && raw.macros.size() == 1 && ClassifyRawMacro(raw.macros.front(), entry, true)
                && entry.aliasTarget.empty();
            if (verbose)
                std::cout << std::format("[verbose] C++ system macro '{}' ({}:{}): {}\n", name,
                                         found.file, found.line,
                                         folded ? "bound" : "did not fold to a constant");
            if (!folded)
            {
                // `errno` is no constant but an lvalue: bind it through `&(errno)`.
                if (BindCxxLvalueMacro(c.group, name, found.file))
                {
                    CMacroEntry marker;
                    marker.name = name;
                    marker.aliasTarget = kCxxLvalueMacroMarker;
                    marker.file = found.file;
                    remember(std::move(marker));
                    return true;
                }
                rememberAbsent();
                return false;
            }
            entry.name = name;
            remember(entry);
            RegisterCMacros({ entry });
            return GetGlobalVariableNV(name).Storage != nullptr;
        }
        return false;
}

bool LLVMBackend::TryBindCHeaderMacro(const std::string& name)
{
        const size_t oldGroupCount = cxxImportGroups_.size();
        auto addDefineName = [](const std::string& define, std::unordered_set<std::string>& names) {
            std::string_view text = define;
            if (text.starts_with("-D")) text.remove_prefix(2);
            size_t end = 0;
            while (end < text.size()
                   && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
                ++end;
            if (end != 0 && (std::isalpha(static_cast<unsigned char>(text.front()))
                             || text.front() == '_'))
                names.emplace(text.substr(0, end));
        };
        for (CHeaderMacroGroup& group : cHeaderMacroGroups_)
        {
            if (!group.macroNamesScanned)
            {
                group.macroNamesScanned = true;
                for (const std::string& define : cDefines_) addDefineName(define, group.macroNames);
                for (const std::string& define : group.defines) addDefineName(define, group.macroNames);
                for (const std::string& path : group.dependencyFiles)
                {
                    std::ifstream input(path);
                    std::string line;
                    while (std::getline(input, line))
                    {
                        size_t at = line.find_first_not_of(" \t\r");
                        if (at == std::string::npos || line[at++] != '#') continue;
                        at = line.find_first_not_of(" \t", at);
                        if (at == std::string::npos || line.compare(at, 6, "define") != 0) continue;
                        at += 6;
                        if (at < line.size()
                            && !std::isspace(static_cast<unsigned char>(line[at]))) continue;
                        at = line.find_first_not_of(" \t", at);
                        if (at == std::string::npos
                            || (!std::isalpha(static_cast<unsigned char>(line[at]))
                                && line[at] != '_')) continue;
                        size_t end = at + 1;
                        while (end < line.size()
                               && (std::isalnum(static_cast<unsigned char>(line[end]))
                                   || line[end] == '_'))
                            ++end;
                        group.macroNames.emplace(line.substr(at, end - at));
                    }
                }
            }
            if (group.macroNames.count(name) != 0)
                FindOrAddCxxImportGroup(group.headers, group.defines);
        }
        if (cxxImportGroups_.size() == oldGroupCount) return false;

        // The first lookup cached misses for the C++ import groups already present. Replay them
        // from cache while asking the newly added C-header groups for this one unresolved name.
        cxxSystemMacroLookups_.erase(name);
        return TryBindCxxSystemMacro(name);
}

bool LLVMBackend::TryBindCxxNamespaceConstant(const std::string& cflatName,
                                              const std::string& baseName,
                                              const std::vector<std::string>& typeArgs,
                                              bool* notConstant)
{
        if (notConstant != nullptr) *notConstant = false;
        if (!HasCxxImportGroup() || cflatName.empty() || baseName.empty()) return false;
        if (GetGlobalVariableNV(cflatName).Storage != nullptr) return true;
        if (auto seen = cxxForeignRequests_.find(cflatName); seen != cxxForeignRequests_.end())
        {
            if (notConstant != nullptr) *notConstant = cxxNotConstantVariables_.count(cflatName) != 0;
            return false;
        }
        std::string cxxBase = baseName;
        for (size_t pos = 0; (pos = cxxBase.find('.', pos)) != std::string::npos; pos += 2)
            cxxBase.replace(pos, 1, "::");
        std::string cxxVariable = cxxBase;
        if (!typeArgs.empty())
        {
            cxxVariable += "<";
            for (size_t i = 0; i < typeArgs.size(); ++i)
            {
                std::string arg;
                if (!CxxSpellingForCflatType(typeArgs[i], arg)) return false;
                cxxVariable += (i ? ", " : "") + arg;
            }
            cxxVariable += ">";
        }
        std::string error;
        const bool bound = RequestCxxTypeInOwningGroup(cxxBase, cflatName,
                                                       "decltype(" + cxxVariable + ")", {}, error);
        if (verbose && !bound)
            std::cout << std::format("[verbose]   C++ namespace constant '{}' not bound: {}\n",
                                     cxxVariable, error);
        cxxTypeOwnerGroup_.erase(cflatName);
        if (notConstant != nullptr) *notConstant = cxxNotConstantVariables_.count(cflatName) != 0;
        return bound && GetGlobalVariableNV(cflatName).Storage != nullptr;
}

bool LLVMBackend::IsCxxNamespaceClassObject(const std::string& qualifiedName)
{
        const size_t dot = qualifiedName.rfind('.');
        if (dot == std::string::npos || dot == 0 || !HasCxxImportGroup()) return false;
        if (GetGlobalVariableNV(qualifiedName).Storage != nullptr)
        {
            const auto global = GetGlobalVariableNV(qualifiedName);
            return !global.TypeAndValue.Pointer && IsDataStructure(global.TypeAndValue.TypeName);
        }
        // Cache the class-or-object decision per name; repeated member uses should not re-request it.
        if (!cxxNamespaceObjectProbed_.insert(qualifiedName).second) return false;
        std::string cxxError;
        TryRequestCxxType(qualifiedName, {}, qualifiedName, cxxError);
        if (IsDataStructure(qualifiedName)
            || IsDataStructure(ResolveTypeAlias(qualifiedName))) return false;

        // A failed type request shares memo keys (plain + per-group) with the object request.
        std::vector<std::pair<std::string, std::string>> failedTypeRequests;
        const std::string groupPrefix = qualifiedName + "|G";
        for (auto it = cxxForeignRequests_.begin(); it != cxxForeignRequests_.end(); )
            if (!it->second.empty()
                && (it->first == qualifiedName || it->first.starts_with(groupPrefix)))
            {
                failedTypeRequests.emplace_back(it->first, it->second);
                it = cxxForeignRequests_.erase(it);
            }
            else
                ++it;
        if (!TryBindCxxNamespaceConstant(qualifiedName, qualifiedName, {}))
        {
            for (auto& [key, why] : failedTypeRequests)
                if (cxxForeignRequests_.count(key) == 0 || key == qualifiedName)
                    cxxForeignRequests_[key] = std::move(why);
            return false;
        }
        const auto global = GetGlobalVariableNV(qualifiedName);
        return global.Storage != nullptr && !global.TypeAndValue.Pointer
            && IsDataStructure(global.TypeAndValue.TypeName);
}

bool LLVMBackend::DecodeCxxIncompleteTemplateError(const std::string& error,
                                                    std::string& spelling,
                                                    std::string& typeName,
                                                    bool* refusedEarlier) const
{
        constexpr std::string_view current = "\x1f" "CFLAT_INCOMPLETE_CPP_TEMPLATE" "\x1f";
        constexpr std::string_view earlier = "\x1f" "CFLAT_REFUSED_INCOMPLETE_CPP_TEMPLATE" "\x1f";
        const bool isEarlier = error.starts_with(earlier);
        if (!isEarlier && !error.starts_with(current)) return false;
        if (refusedEarlier != nullptr) *refusedEarlier = isEarlier;
        const std::string_view marker = isEarlier ? earlier : current;
        const size_t split = error.find('\n', marker.size());
        if (split == std::string::npos) return false;
        spelling = error.substr(marker.size(), split - marker.size());
        typeName = error.substr(split + 1);
        return !spelling.empty() && !typeName.empty();
}

bool LLVMBackend::CxxTemplateDefineMismatch(const std::string& cxxBase,
                                           const std::string& spelling,
                                           const std::string& cflatName,
                                           size_t ownerGroup,
                                           const std::vector<size_t>& deps,
                                           std::string& error)
{
        if (ownerGroup >= cxxImportGroups_.size()) return false;
        auto normalized = [](std::string name) {
            size_t pos = 0;
            while ((pos = name.find('.', pos)) != std::string::npos)
            { name.replace(pos, 1, "::"); pos += 2; }
            return name;
        };
        auto specializesName = [&](const CxxImportGroup& group) {
            return std::any_of(group.specializedTemplates.begin(), group.specializedTemplates.end(),
                [&](const std::string& entry) {
                    return normalized(entry) == cxxBase;
                });
        };
        auto publishes = [&](const CxxImportGroup& group) {
            return std::any_of(group.publishedNames.begin(), group.publishedNames.end(),
                [&](const std::string& name) { return normalized(name) == cxxBase; });
        };
        const auto& owner = cxxImportGroups_[ownerGroup];
        // Common path: no group with other defines, or nobody specializes this template.
        if (std::none_of(cxxImportGroups_.begin(), cxxImportGroups_.end(),
                [&](const CxxImportGroup& group) { return group.defines != owner.defines; })
            || std::none_of(cxxImportGroups_.begin(), cxxImportGroups_.end(), specializesName))
            return false;
        bool ownerSpecialized = false;
        size_t ownerLayoutGroup = ownerGroup;
        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
        {
            if (cxxImportGroups_[i].defines != owner.defines || !publishes(cxxImportGroups_[i]))
                continue;
            if (specializesName(cxxImportGroups_[i]))
            {
                ownerSpecialized = true;
                ownerLayoutGroup = i;
                break;
            }
        }

        auto describeGroup = [](const CxxImportGroup& group) {
            std::string headers;
            for (const std::string& header : group.headers)
                headers += (headers.empty() ? "" : ", ") + std::filesystem::path(header).filename().string();
            std::string defines;
            for (const std::string& define : group.defines)
                defines += (defines.empty() ? "" : ", ") + define;
            return std::format("'{}' (defines: {})", headers.empty() ? "<unknown>" : headers,
                               defines.empty() ? "none" : defines);
        };

        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
        {
            const auto& other = cxxImportGroups_[i];
            if (other.defines == owner.defines || !publishes(other))
                continue;
            if (!ownerSpecialized && !specializesName(other)) continue;

            auto requestLayout = [&](size_t primary, cflat_cinterop::RawRecord& record) {
                std::vector<size_t> groupDeps = deps;
                for (size_t group = 0; group < cxxImportGroups_.size(); ++group)
                    if (cxxImportGroups_[group].defines == cxxImportGroups_[primary].defines
                        && specializesName(cxxImportGroups_[group]))
                        groupDeps.push_back(group);
                CxxRequestGroup requestGroup = MakeCxxRequestGroup(primary, groupDeps);
                CxxRequestItem item;
                item.cflatName = cflatName;
                item.cxxSpelling = spelling;
                item.needDefinitions = false;
                item.explicitInstantiation = false;
                cflat_cinterop::ExtractResult raw;
                std::string requestError;
                if (requestGroup.headers.empty()
                    || !RunCxxTypeRequests(requestGroup, { item }, {}, false, raw, requestError))
                    return false;
                auto found = std::find_if(raw.records.begin(), raw.records.end(),
                    [&](const cflat_cinterop::RawRecord& candidate) {
                        return candidate.name == cflatName;
                    });
                if (found == raw.records.end()) return false;
                record = *found;
                return true;
            };
            cflat_cinterop::RawRecord ownerRecord;
            cflat_cinterop::RawRecord otherRecord;
            if (!requestLayout(ownerLayoutGroup, ownerRecord) || !requestLayout(i, otherRecord))
                continue;
            auto sameLayout = [](const cflat_cinterop::RawRecord& left,
                                 const cflat_cinterop::RawRecord& right) {
                if (left.sizeBytes != right.sizeBytes || left.alignBytes != right.alignBytes
                    || left.fields.size() != right.fields.size()) return false;
                for (size_t field = 0; field < left.fields.size(); ++field)
                    if (left.fields[field].offsetBytes != right.fields[field].offsetBytes
                        || left.fields[field].sizeBytes != right.fields[field].sizeBytes
                        || left.fields[field].alignBytes != right.fields[field].alignBytes
                        || left.fields[field].bitOffset != right.fields[field].bitOffset
                        || left.fields[field].ctype != right.fields[field].ctype)
                        return false;
                return true;
            };
            if (sameLayout(ownerRecord, otherRecord)) continue;
            error = std::format("C++ template instantiation '{}' has different layouts across "
                                "import groups {} and {}; using both violates the C++ ODR",
                                spelling, describeGroup(cxxImportGroups_[ownerLayoutGroup]),
                                describeGroup(other));
            return true;
        }
        return false;
}

/*
 * Owning import groups of every C++ class named ANYWHERE in a requested spelling. A nested
 * specialization (`std::vector<std::shared_ptr<user::C>>`) is owned by the group of its OUTER
 * template, so only this recursive union brings `user::C`'s own header into the outer request.
 */
void LLVMBackend::CollectCxxTypeOwnerGroups(const std::string& cflatTypeName,
                                            std::vector<size_t>& groups,
                                            std::unordered_set<std::string>& visited) const
{
        std::string type = cflatTypeName;
        while (!type.empty() && (type.back() == '*' || type.back() == ' ')) type.pop_back();
        if (type.empty() || !visited.insert(type).second) return;
        if (auto owner = cxxTypeOwnerGroup_.find(type);
            owner != cxxTypeOwnerGroup_.end()
            && owner->second < cxxImportGroups_.size()
            && !cxxImportGroups_[owner->second].headers.empty()
            && std::find(groups.begin(), groups.end(), owner->second) == groups.end())
            groups.push_back(owner->second);
        TypeSpelling parsed;
        if (!DemangleType(*this, type, parsed)) return;
        // A POINTER or VIEW argument mangles as a PREFIX ('.p$nest.Cell'), and no group owns that
        // key: the owning group is recorded for the POINTEE, so visit it with the prefix removed.
        if (parsed.pointerDepth > 0 || parsed.view)
        {
            TypeSpelling pointee = parsed;
            pointee.pointerDepth = 0;
            pointee.view = false;
            const std::string pointeeName = MangleType(*this, pointee);
            if (pointeeName != type)
            {
                CollectCxxTypeOwnerGroups(pointeeName, groups, visited);
                return;
            }
        }
        if (parsed.args.empty()) return;
        CollectCxxTypeOwnerGroups(parsed.base, groups, visited);
        for (const TypeSpelling& arg : parsed.args)
            CollectCxxTypeOwnerGroups(MangleType(*this, arg), groups, visited);
}

/*
 * The first nested class-typed component of a requested spelling that no import group owns - the
 * type a failed request is actually missing, as opposed to the outer template everyone can see.
 */
bool LLVMBackend::FirstUnownedCxxComponent(const std::string& cflatTypeName,
                                           std::string& outSpelling) const
{
        TypeSpelling parsed;
        if (!DemangleType(*this, cflatTypeName, parsed)) return false;
        for (const TypeSpelling& arg : parsed.args)
        {
            if (arg.value) continue;
            // A pointer or view argument mangles as a PREFIX ('.p$nest.Missing'), which is not
            // writable source: name the POINTEE class, the component a group has to declare.
            TypeSpelling pointee = arg;
            pointee.pointerDepth = 0;
            pointee.view = false;
            std::string name = MangleType(*this, pointee);
            while (!name.empty() && (name.back() == '*' || name.back() == ' ')) name.pop_back();
            if (name.empty()) continue;
            if (FirstUnownedCxxComponent(name, outSpelling)) return true;
            if (name.find('.') == std::string::npos) continue;
            if (cxxTypeOwnerGroup_.count(name) != 0 || IsCxxForeignTypeRegistered(name)
                || !ResolveEnumTypeName(name).empty())
                continue;
            if (!CxxSpellingForCflatType(name, outSpelling)) outSpelling = name;
            return true;
        }
        return false;
}

bool LLVMBackend::RequestCxxType(const std::string& baseName, const std::vector<std::string>& typeArgs,
                                 const std::string& cflatName, std::string& error,
                                 bool deferIncompleteCodegen)
{
        if (IsCxxForeignTypeRegistered(cflatName) && !cxxTentativeTypes_.count(cflatName))
            return true;
        std::string spelling = baseName;
        size_t pos = 0;
        while ((pos = spelling.find('.', pos)) != std::string::npos)
        { spelling.replace(pos, 1, "::"); pos += 2; }
        std::string aliasUseKey = baseName + "\x1f";
        for (const std::string& arg : typeArgs)
            aliasUseKey += std::to_string(arg.size()) + ":" + arg;
        auto aliasUse = cxxAliasUseSpellings_.find(aliasUseKey);
        if (aliasUse != cxxAliasUseSpellings_.end())
            spelling = aliasUse->second;
        else if (!typeArgs.empty())
        {
            spelling += "<";
            for (size_t i = 0; i < typeArgs.size(); ++i)
            {
                if (i) spelling += ", ";
                std::string arg;
                if (!CxxTemplateArgSpellingForCflatType(typeArgs[i], arg))
                {
                    error = std::format("'{}' has no C++ spelling, so it cannot be a template "
                                        "argument of the C++ template '{}' - use a primitive, a "
                                        "pointer, or another C++ type", typeArgs[i], baseName);
                    return false;
                }
                spelling += arg;
            }
            // `vector<vector<int>>` needs no space in C++11 and later, but keep the closing pair
            // readable for the diagnostic.
            spelling += ">";
        }
        // Inside an import group's extraction (a signature, a member, a nested type) the group is
        // already fixed and the request must not escape it.
        std::string generatedTypeSource;
        std::set<size_t> generatedDependencyGroups;
        std::unordered_set<std::string> incompleteTypes;
        GeneratedCxxDefinitionsFor(typeArgs, generatedTypeSource, generatedDependencyGroups,
                                    incompleteTypes);
        /*
         * A refusal over an incomplete argument is keyed on the full specialization. clang keeps
         * the invalid instantiation in the live TU, so a use after the struct is defined cannot
         * bind either; it is refused naming the earlier cause instead of blaming the import.
         */
        const std::string incompleteKey = cflatName + "\x1f" "INCOMPLETE";
        if (auto it = cxxForeignRequests_.find(incompleteKey); it != cxxForeignRequests_.end())
        {
            if (!incompleteTypes.empty())
            {
                error = it->second;
                return false;
            }
            std::string refusedSpelling;
            std::string refusedType;
            DecodeCxxIncompleteTemplateError(it->second, refusedSpelling, refusedType);
            error = std::string("\x1f" "CFLAT_REFUSED_INCOMPLETE_CPP_TEMPLATE" "\x1f")
                + SpellType(*this, TypeAndValue{ .TypeName = cflatName }) + "\n"
                + SpellType(*this, TypeAndValue{ .TypeName = refusedType });
            return false;
        }
        auto rememberIncomplete = [&](bool ok) {
            std::string spelled, incompleteName;
            if (!ok && DecodeCxxIncompleteTemplateError(error, spelled, incompleteName))
                cxxForeignRequests_[incompleteKey] = error;
            return ok;
        };
        // An incomplete [cpp] struct argument is refused here, by instantiating every member.
        struct IncompleteSpellingScope
        {
            std::string& slot; std::string saved;
            ~IncompleteSpellingScope() { slot = std::move(saved); }
        } incompleteSpellingScope{ cxxIncompleteArgumentSpelling_, cxxIncompleteArgumentSpelling_ };
        const bool deferIncomplete = deferIncompleteCodegen && !incompleteTypes.empty();
        cxxIncompleteArgumentSpelling_ = incompleteTypes.empty() ? std::string() : spelling;
        if (activeCxxRequestGroup_ != nullptr)
        {
            const bool ok = RequestCxxForeignType(cflatName, spelling, error,
                                                   /*needDefinitions*/ true,
                                                   /*explicitInstantiation*/
                                                       !deferIncomplete,
                                                   /*tentative*/ !incompleteTypes.empty(),
                                                   /*extraSource*/ {}, generatedTypeSource,
                                                   /*persistOnSuccess*/ incompleteTypes.empty(),
                                                   /*ownershipProbe*/ false,
                                                   /*deferIncompleteCodegen*/ deferIncomplete);
            if (!ok && !incompleteTypes.empty() && baseName != "std.shared_ptr")
                error = std::string("\x1f" "CFLAT_INCOMPLETE_CPP_TEMPLATE" "\x1f") + spelling + "\n"
                    + *incompleteTypes.begin();
            return rememberIncomplete(ok);
        }

        // A type spelled by CFlat code: find the import that owns the template, and bring in the
        // groups that own its template arguments so `std.vector<std.string>` still has <string>.
        std::string cxxBase = baseName;
        size_t bpos = 0;
        while ((bpos = cxxBase.find('.', bpos)) != std::string::npos)
        { cxxBase.replace(bpos, 1, "::"); bpos += 2; }
        std::vector<size_t> deps;
        std::unordered_set<std::string> visitedArgs;
        for (const std::string& arg : typeArgs)
            CollectCxxTypeOwnerGroups(arg, deps, visitedArgs);
        for (size_t group : generatedDependencyGroups)
            if (std::find(deps.begin(), deps.end(), group) == deps.end()) deps.push_back(group);
        const bool ok = RequestCxxTypeInOwningGroup(cxxBase, cflatName, spelling, deps, error,
                                                     generatedTypeSource,
                                                     /*retryable*/ !incompleteTypes.empty(),
                                                     /*deferIncompleteCodegen*/
                                                         deferIncomplete);
        if (!ok && !incompleteTypes.empty() && baseName != "std.shared_ptr")
            error = std::string("\x1f" "CFLAT_INCOMPLETE_CPP_TEMPLATE" "\x1f") + spelling + "\n"
                + *incompleteTypes.begin();
        return rememberIncomplete(ok);
}

/*
 * Group resolution for a type CFlat code spells. Candidates come from what each import published,
 * from a system header that IS the name, and from the namespaces each import seeded; the first
 * candidate whose headers actually declare the spelling wins and is remembered for every later use
 * of the same template. Nothing is memoized as a failure until the last candidate has been tried,
 * so a wrong first guess does not poison the name.
 */
bool LLVMBackend::RequestCxxTypeInOwningGroup(const std::string& cxxBase,
                                              const std::string& cflatName,
                                              const std::string& spelling,
                                              const std::vector<size_t>& deps, std::string& error,
                                              const std::string& prefixSource,
                                              bool retryable,
                                              bool deferIncompleteCodegen)
{
        const std::vector<size_t> order = CandidateCxxGroupsFor(cxxBase);
        if (order.empty())
            return false;
        std::vector<std::string> tried;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const bool last = k + 1 == order.size();
            std::vector<size_t> requestDeps = deps;
            std::vector<size_t> specializationGroups;
            const auto& owner = cxxImportGroups_[order[k]];
            auto specializes = [&](size_t index, const std::string& templateName) {
                return std::any_of(
                    cxxImportGroups_[index].specializedTemplates.begin(),
                    cxxImportGroups_[index].specializedTemplates.end(), [&](std::string name) {
                        size_t pos = 0;
                        while ((pos = name.find('.', pos)) != std::string::npos)
                        { name.replace(pos, 1, "::"); pos += 2; }
                        return name == templateName;
                    });
            };
            auto addSpecializationGroup = [&](size_t index) {
                if (std::find(requestDeps.begin(), requestDeps.end(), index) == requestDeps.end())
                    requestDeps.push_back(index);
                if (std::find(specializationGroups.begin(), specializationGroups.end(), index)
                    == specializationGroups.end())
                    specializationGroups.push_back(index);
            };
            for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
            {
                if (i == order[k] || cxxImportGroups_[i].defines != owner.defines) continue;
                if (specializes(i, cxxBase)) addSpecializationGroup(i);
            }
            std::string ownerDot = cxxBase;
            size_t ownerPos = 0;
            while ((ownerPos = ownerDot.find("::", ownerPos)) != std::string::npos)
            { ownerDot.replace(ownerPos, 2, "."); ++ownerPos; }
            std::unordered_set<std::string> visitedVariableTemplates;
            std::vector<std::string> variableTemplateWorklist;
            std::vector<std::string> variableTemplateClosure;
            if (auto dependencies = owner.variableTemplateDependencies.find(ownerDot);
                dependencies != owner.variableTemplateDependencies.end())
                for (const std::string& dependency : dependencies->second)
                    if (visitedVariableTemplates.insert(dependency).second)
                        variableTemplateWorklist.push_back(dependency);
            while (!variableTemplateWorklist.empty())
            {
                const std::string dependency = std::move(variableTemplateWorklist.back());
                variableTemplateWorklist.pop_back();
                variableTemplateClosure.push_back(dependency);
                for (const CxxImportGroup& group : cxxImportGroups_)
                {
                    if (group.defines != owner.defines) continue;
                    if (auto dependencies = group.variableTemplateDependencies.find(dependency);
                        dependencies != group.variableTemplateDependencies.end())
                        for (const std::string& transitive : dependencies->second)
                            if (visitedVariableTemplates.insert(transitive).second)
                                variableTemplateWorklist.push_back(transitive);
                }
            }
            for (const std::string& dependency : variableTemplateClosure)
            {
                std::string dependencyCxx = dependency;
                size_t dependencyPos = 0;
                while ((dependencyPos = dependencyCxx.find('.', dependencyPos))
                       != std::string::npos)
                { dependencyCxx.replace(dependencyPos, 1, "::"); dependencyPos += 2; }
                for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
                    if (i != order[k] && cxxImportGroups_[i].defines == owner.defines
                        && specializes(i, dependencyCxx))
                        addSpecializationGroup(i);
            }
            if (activeCxxRequestGroup_ == nullptr
                && CxxTemplateDefineMismatch(cxxBase, spelling, cflatName, order[k], deps, error))
                return false;
            CxxRequestGroup group = MakeCxxRequestGroup(order[k], requestDeps);
            group.specializationGroups = std::move(specializationGroups);
            if (group.headers.empty()) continue;
            llvm::TimeTraceScope groupScope("CxxRequestGroup", spelling + " -> " + group.label);
            CxxRequestGroupScope guard(*this, &group);
            std::string localError;
            if (RequestCxxForeignType(cflatName, spelling, localError, /*needDefinitions*/ true,
                                      /*explicitInstantiation*/ !deferIncompleteCodegen,
                                      /*tentative*/ !last || retryable,
                                      /*extraSource*/ {}, prefixSource,
                                      /*persistOnSuccess*/ !retryable,
                                      /*ownershipProbe*/ true,
                                      /*deferIncompleteCodegen*/ deferIncompleteCodegen))
            {
                cxxTemplateOwnerGroup_[cxxBase] = order[k];
                cxxTypeOwnerGroup_[cflatName] = order[k];
                return true;
            }
            tried.push_back(group.label);
            if (verbose)
                std::cout << std::format("[verbose] C++ owner candidate '{}' rejected for {}: {}\n",
                                         group.label, spelling, localError);
            // An ownership rejection is not a clang diagnostic worth relaying; it reads as
            // "no header declares" below, like any exhausted candidate list.
            if (last && order.size() == 1 && localError != kCxxOwnershipProbeRejected)
            {
                error = localError;
                return false;
            }
        }
        llvm::TimeTraceScope groupScope("CxxRequestGroup", spelling + " -> unresolved");
        std::string names;
        for (size_t i = 0; i < tried.size(); ++i) names += (i ? ", '" : "'") + tried[i] + "'";
        std::string missing;
        if (FirstUnownedCxxComponent(cflatName, missing))
            error = std::format("no imported C++ header declares '{}', a template argument of "
                                "'{}' - tried {}", missing, spelling, names);
        else
            error = std::format("no imported C++ header declares '{}' - tried {}", cxxBase, names);
        // A refusal over a still-incomplete CFlat argument is not memoized as a missing
        // header; RequestCxxType keys it on the specialization's incomplete refusal instead.
        if (!retryable) cxxForeignRequests_[cflatName] = error;
        return false;
    }

bool LLVMBackend::ExtractCFileClang(const std::string& cSourcePath, const std::vector<std::string>& driverArgs,
                           std::vector<CSigEntry>& outSigs, std::vector<CRecordEntry>& outRecords,
                           std::vector<CGlobalEntry>& outGlobals, std::vector<CTypeAliasEntry>& outTypedefs,
                           std::vector<std::string>& outIncludes, bool cxxMode,
                           uint64_t& outLongDoubleWidth, bool& outLongDoubleIsIEEEDouble,
                           std::string& outTargetTriple)
{
        llvm::TimeTraceScope extractScope("CFileExtract", cSourcePath);

        cflat_cinterop::ExtractRequest req;
        req.realPath        = cSourcePath;     // parsed from disk
        req.args            = driverArgs;
        req.cxxMode         = cxxMode;
        req.wantMacros      = false;
        req.wantIncludes    = true;
        req.requireInScope  = true;
        req.inScopeDirs.push_back(std::filesystem::path(cSourcePath).parent_path().string());
        req.definitionsOnly = true;
        req.verbose         = verbose;

        if (verbose)
            std::cout << std::format("[verbose] extracting {} signatures: {} (clang C++ API)\n",
                                     cxxMode ? "C++" : "C", cSourcePath);

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   C extraction failed: {}\n", err);
            return false;
        }
        SetCInteropTargetFacts(raw);
        outLongDoubleWidth = raw.longDoubleWidth;
        outLongDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
        outTargetTriple = raw.targetTriple;
        outIncludes = raw.includedFiles;

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", cSourcePath);
            AdoptRawTypedefs(raw);
            for (const auto& t : raw.typedefs)
            {
                CTypeAliasEntry alias;
                alias.name = t.name;
                alias.target = t.underlying;
                alias.qualifiedName = t.qualifiedName;
                outTypedefs.push_back(std::move(alias));
            }
        }
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", cSourcePath);
            // No prune here: the .c path has no scope filter, so every top-level record is wanted.
            // Pruning would drop the synthesized nested records (inScope=false) a pointer field names.
            MapRawRecords(raw, outRecords);
            RegisterCRecords(outRecords, cSourcePath);
        }
        {
            llvm::TimeTraceScope sigScope("MapSignatures", cSourcePath);
            for (const auto& rs : raw.sigs)
            {
                CSigEntry e;
                if (MapRawSig(rs, e)) outSigs.push_back(std::move(e));
            }
        }
        {
            llvm::TimeTraceScope globalScope("MapGlobals", cSourcePath);
            for (const auto& rg : raw.globals)
            {
                CGlobalEntry e;
                if (MapRawGlobal(rg, e, cxxMode)) outGlobals.push_back(std::move(e));
            }
        }
        return true;
    }

void LLVMBackend::RegisterCFileSigEntry(CFileSigCacheEntry& entry, const std::string& fileForLsp,
                                        const std::string& programAlias)
{
        // The output's up-to-date check must see the transitive includes on a hit too.
        for (const auto& dep : entry.deps) RecordDependency(dep.path);
        SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble, entry.targetTriple);
        // Same adoption AdoptRawTypedefs performs on the parse path.
        for (const auto& alias : entry.typeAliases)
            if (!alias.name.empty() && !alias.target.empty() && alias.target != alias.name)
            {
                cTypedefMap_.emplace(alias.name, alias.target);
                if (!alias.qualifiedName.empty() && alias.qualifiedName != alias.name)
                    cTypedefMap_.emplace(alias.qualifiedName, alias.target);
            }
        // Records must be registered before sigs so signatures referencing struct-by-
        // value resolve to the same dataStructures entries on cache hits.
        RegisterCRecords(entry.records, fileForLsp);
        RegisterCSignatures(entry.sigs, fileForLsp, programAlias);
        RegisterCGlobals(entry.globals, fileForLsp);
    }

bool LLVMBackend::ExtractCSignatures(const std::string& cSourcePath, const std::string& programAlias, bool cxxMode)
{
        // Canonical path: stable cache key + the real .c for LSP go-to-definition.
        llvm::SmallString<256> realPath;
        std::string fileForLsp = cSourcePath;
        if (!llvm::sys::fs::real_path(cSourcePath, realPath))
            fileForLsp = realPath.str().str();

        // Defines can gate which functions a .c defines, so fold them into the cache key
        // (the file path alone is the LSP identity; the key is path + defines).
        std::string cacheKey = fileForLsp
            + (cxxMode ? "|CXX|STD" + cppStandard_ : "|C");
        for (const auto& def : cDefines_) cacheKey += "|D" + def;

        // Hash the file at most once per call, and only when actually needed.
        uint64_t currentHash = 0;
        bool haveHash = false;
        auto hashNow = [&]() -> uint64_t
        {
            if (!haveHash) { HashFileContents(fileForLsp, currentHash); haveHash = true; }
            return currentHash;
        };

        std::error_code mtEc;
        auto currentMtime = std::filesystem::last_write_time(fileForLsp, mtEc);

        // --- Cache lookup under lock. Copy the entry out, then register after the lock is
        //     released, so the global cache never serializes per-backend work. ---
        CFileSigCacheEntry hitEntry;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto cacheIt = cFileSigCache_.find(cacheKey);
            if (!mtEc && cacheIt != cFileSigCache_.end())
            {
                CFileSigCacheEntry& entry = cacheIt->second;
                if (entry.mtime == currentMtime)
                {
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (mtime) for {}\n", fileForLsp);
                    hit = true;
                }
                // Timestamp moved but content may be identical - only now pay for a hash.
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (hash) for {}\n", fileForLsp);
                    entry.mtime = currentMtime; // refresh so the next check short-circuits on mtime
                    hit = true;
                }
                if (hit)
                {
                    TouchCFileSigEntry(cacheKey, entry);
                    hitEntry = entry;
                }
            }
        }
        if (hit)
        {
            RegisterCFileSigEntry(hitEntry, fileForLsp, programAlias);
            return true;
        }

        // The disk entry is keyed on the whole clang invocation (include dirs, defines, target),
        // so a compile under different options never replays another's surface.
        const std::vector<std::string> driverArgs =
            BuildClangDriverArgs(/*headerDir*/ "", /*extraDefines*/ {}, /*errorRecovery*/ true, cxxMode);
        uint64_t diskKey = 14695981039346656037ULL;
        auto fold = [&diskKey](const std::string& s) {
            for (unsigned char c : s) { diskKey ^= c; diskKey *= 1099511628211ULL; }
        };
        fold("|SRC"); fold(fileForLsp); fold(cxxMode ? "|CXX" : "|C");
        for (const auto& arg : driverArgs) { fold("|A"); fold(arg); }
        const std::string cacheDir = GetCHeaderCacheDir();
        const bool diskAllowed = !mtEc && !cacheDir.empty();
        if (diskAllowed)
        {
            CFileSigCacheEntry diskEntry;
            std::string missReason;
            bool diskHit;
            {
                llvm::TimeTraceScope loadScope("CFileJsonLoad", fileForLsp);
                diskHit = TryLoadCHeaderDiskCache(cacheDir, diskKey, currentMtime, hashNow, diskEntry,
                                                  /*expectedRequestKey*/ {}, /*requireBitcode*/ false,
                                                  &missReason);
            }
            if (diskHit)
            {
                if (verbose) std::cout << std::format("[verbose] C signatures disk cache hit for {}\n", fileForLsp);
                {
                    std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                    InsertCFileSigEntry(cacheKey, CFileSigCacheEntry(diskEntry), verbose);
                }
                RegisterCFileSigEntry(diskEntry, fileForLsp, programAlias);
                return true;
            }
            if (verbose)
                std::cout << std::format("[verbose] C signatures disk cache miss for {}: {}\n",
                                         fileForLsp, missReason);
        }

        // Cache miss - parse outside the lock; concurrent misses redo work harmlessly.
        // Extraction uses the clang C++ API in-process (no clang-cl needed), so LSP works too.
        if (!CountTuParse(cxxMode ? "C++ source" : "C source", cacheKey, fileForLsp, "signature extraction"))
            return false;
        CFileSigCacheEntry entry;
        std::vector<std::string> includes;
        if (!ExtractCFileClang(cSourcePath, driverArgs, entry.sigs, entry.records, entry.globals,
                               entry.typeAliases, includes, cxxMode, entry.longDoubleWidth,
                               entry.longDoubleIsIEEEDouble, entry.targetTriple))
            return false;

        // Keep only real on-disk paths in the transitive dependency list.
        // Non-existent deps would otherwise poison every later cache validation.
        std::unordered_set<std::string> seenIncludes;
        for (const auto& inc : includes)
        {
            RecordDependency(inc);
            std::error_code dec;
            auto dm = std::filesystem::last_write_time(inc, dec);
            if (dec || !seenIncludes.insert(inc).second) continue;
            CHeaderDep dep;
            dep.path  = inc;
            dep.mtime = (int64_t)dm.time_since_epoch().count();
            dep.hashTime = (int64_t)std::filesystem::file_time_type::clock::now()
                               .time_since_epoch().count();
            HashFileFnv1a(inc, dep.hash);
            entry.deps.push_back(std::move(dep));
        }

        // Records were already registered inside ExtractCFileClang (so it could map
        // struct-by-value parameter types); do not re-register here.
        RegisterCSignatures(entry.sigs, fileForLsp, programAlias);
        RegisterCGlobals(entry.globals, fileForLsp);

        if (!mtEc)
        {
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            // Same rule as the header cache: --run and LSP analysis never write to disk.
            if (diskAllowed && !runMode_ && symbolSink_ == nullptr)
                ScheduleCHeaderDiskCacheWrite(cacheDir, diskKey, currentMtime, entry.hash,
                                              CFileSigCacheEntry(entry));
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            InsertCFileSigEntry(cacheKey, std::move(entry), verbose);
        }
        return true;
    }

std::string LLVMBackend::ConstIntValueSuffix(const std::string& typeName, long long value)
{
        bool isUnsigned = !typeName.empty() && typeName[0] == 'u';  // u8/u16/u32/u64
        if (isUnsigned)
        {
            unsigned bits = BitfieldStorageBits(typeName);
            uint64_t mask = (bits == 0 || bits >= 64) ? ~0ull : ((1ull << bits) - 1);
            return std::format(" = 0x{:x}", (uint64_t)value & mask);
        }
        return std::format(" = {}", value);
    }

void LLVMBackend::RegisterCEnums(const std::vector<CEnumEntry>& enums, const std::string& fileForLsp,
                                 bool cxxBoundary)
{
        // An enum's enumerators arrive as one run sharing type, backing and promotion, so the
        // per-type work (backing mapping, owner group, resolved LLVM type) is done once per run.
        const CEnumEntry* runHead = nullptr;
        // Many enums share one underlying spelling (int, unsigned char): map each spelling once.
        std::unordered_map<std::string, std::optional<std::string>> mappedNames;
        auto mappedTypeName = [&](const std::string& spelling) -> const std::string* {
            auto [it, inserted] = mappedNames.try_emplace(spelling);
            if (inserted)
            {
                TypeAndValue mapped;
                if (MapCTypeToTypeAndValue(spelling, mapped, cxxBoundary))
                    it->second = std::move(mapped.TypeName);
            }
            return it->second ? &*it->second : nullptr;
        };
        std::string runBacking;
        bool runTypeResolved = false;
        llvm::IntegerType* runIntTy = nullptr;
        std::string lastNamespacePrefix;
        for (const CEnumEntry& e : enums)
        {
            if (e.name.empty()) continue;
            const bool sameRun = runHead != nullptr && !e.enumType.empty()
                && e.isScoped == runHead->isScoped && e.enumType == runHead->enumType
                && e.underlyingType == runHead->underlyingType
                && e.promotedType == runHead->promotedType;
            if (!sameRun)
            {
                runHead = &e;
                runTypeResolved = false;
                runIntTy = nullptr;
                if (e.isScoped && !e.enumType.empty()) RegisterScopedEnumType(e.enumType);
                if (!e.enumType.empty()) importedEnumTypes_.insert(e.enumType);
                if (cxxBoundary && activeCxxRequestGroup_ != nullptr && !e.enumType.empty())
                    cxxTypeOwnerGroup_.emplace(e.enumType, activeCxxRequestGroup_->primary);
                if (!e.enumType.empty() && !e.underlyingType.empty())
                {
                    const std::string* backingName = mappedTypeName(e.underlyingType);
                    if (backingName != nullptr) RegisterEnumBackingType(e.enumType, *backingName);
                    if (!e.promotedType.empty())
                        if (const std::string* promotedName = mappedTypeName(e.promotedType))
                            enumPromotedTypes_[e.enumType] = *promotedName;
                }
                runBacking = e.enumType.empty() ? std::string{} : GetEnumBackingType(e.enumType);
            }
            if (!e.enumType.empty() && runBacking.empty())
            {
                if (verbose)
                    std::cout << std::format("[verbose]   skipping enum constant '{}': enum type '{}' has no supported backing\n",
                                             e.name, e.enumType);
                continue;
            }
            // `E.v` spelling of a scoped enumerator: remember the bare name for the undefined-name hint.
            if (cxxBoundary && e.isScoped && e.name.size() > e.enumType.size()
                && e.name.compare(0, e.enumType.size(), e.enumType) == 0 && e.name[e.enumType.size()] == '.')
                scopedCppEnumeratorHints_.try_emplace(e.name.substr(e.enumType.size() + 1), e.enumType);
            // First writer wins: a hand-written declaration or an earlier header takes
            // precedence over a duplicate constant name.
            if (globalNamedVariable.count(e.name)) continue;
            // A qualified C++ enumerator ("ns.Cls.Kind.One") needs every dotted prefix registered
            // as a namespace before the name resolves at a use site. Consecutive enumerators of
            // one enum share the whole prefix, registered by the first.
            if (const size_t lastDot = e.name.rfind('.'); lastDot != std::string::npos)
            {
                if (e.name.size() > lastDot && lastNamespacePrefix.size() == lastDot
                    && e.name.compare(0, lastDot, lastNamespacePrefix) == 0)
                {
                    // already registered
                }
                else
                {
                    for (size_t pos = 0; (pos = e.name.find('.', pos)) != std::string::npos; ++pos)
                        RegisterNamespace(e.name.substr(0, pos));
                    lastNamespacePrefix.assign(e.name, 0, lastDot);
                }
            }

            bool wide = (e.value < INT32_MIN || e.value > INT32_MAX);
            TypeAndValue tv;
            tv.TypeName     = e.enumType.empty() ? (wide ? "i64" : "int") : e.enumType;
            tv.EnumBacking  = e.enumType.empty() ? std::string{} : runBacking;
            tv.IsScopedEnum = e.isScoped;
            tv.VariableName = e.name;
            tv.Pointer      = false;
            llvm::Constant* c = nullptr;
            if (!e.enumType.empty())
            {
                if (!runTypeResolved)
                {
                    runIntTy = llvm::dyn_cast_or_null<llvm::IntegerType>(GetType(tv));
                    runTypeResolved = true;
                }
                if (auto* et = runIntTy)
                    // Unsigned backings hold values past the signed range (u8 enumerator 200):
                    // truncate to the backing width instead of asserting on the sign.
                    c = llvm::ConstantInt::get(et, llvm::APInt(et->getBitWidth(), (uint64_t)e.value,
                                                              e.value < 0, /*implicitTrunc*/ true));
            }
            if (c == nullptr)
                c = wide ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)e.value))
                         : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)e.value));
            auto* sink = GetSymbolSink();
            const std::string sinkTypeName = sink != nullptr ? tv.TypeName : std::string{};
            if (auto* gv = CreateGlobalVariable(std::move(tv), c))
            {
                importedEnumeratorGlobals_.insert(std::string(gv->getName()));
                if (cxxBoundary && e.enumType.empty())
                    cxxUnnamedEnumExpressions_.insert_or_assign(gv, CxxNameFromCflat(e.name));
            }

            if (sink != nullptr)
                sink->Register(SymbolKind::Variable, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col,
                               sinkTypeName + " " + e.name + ConstIntValueSuffix(sinkTypeName, e.value));
        }
        if (verbose)
            std::cout << std::format("[verbose]   registered {} C enum constant(s) from {}\n", enums.size(), fileForLsp);
    }

/*
 * Headers imported on separate `import cpp` lines are extracted as separate translation units,
 * so a `using ry::E;` in one and an `enum class E` in the same namespace in another are never
 * seen together by clang. Including both in one TU is ill-formed ("target of using declaration
 * conflicts with declaration already in scope"); diagnose the same shape here instead of letting
 * the first registration silently win. Types (enums, classes, aliases, class using-declarations)
 * and values are separate categories (a class and a function may share a name); functions form
 * overload sets, so function vs function is never a conflict. A name bound again by the same
 * header, or to the same entity, stays silent.
 */
void LLVMBackend::CheckCxxNamespaceConflicts(const std::vector<CSigEntry>& sigs,
                                             const std::vector<CEnumEntry>& enums,
                                             const std::vector<CTypeAliasEntry>& aliases,
                                             const std::vector<CGlobalEntry>& globals,
                                             const std::vector<CRecordEntry>& records,
                                             const std::vector<std::string>& classTemplateNames,
                                             const std::vector<std::string>& variableTemplateNames,
                                             const std::vector<std::pair<std::string, std::string>>& classUsings,
                                             const std::vector<cflat_cinterop::RawFunctionTemplate>& functionTemplates,
                                             const std::string& fileForLsp)
{
        // The file name is interned once per call so an entity carries a pointer, not a copy.
        const std::string* const fileKey = &*cxxNamespaceFiles_.insert(fileForLsp).first;
        cxxNamespaceEntities_.reserve(cxxNamespaceEntities_.size() + sigs.size() + enums.size() * 2
                                      + aliases.size() + globals.size() + records.size()
                                      + classTemplateNames.size() + variableTemplateNames.size()
                                      + classUsings.size());
        auto note = [&](const std::string& name, CxxNamespaceEntity ent) {
            if (name.find('.') == std::string::npos) return;   // namespace members only
            const std::string key = (ent.isType ? "T:" : "V:") + name;
            auto [it, inserted] = cxxNamespaceEntities_.try_emplace(key);
            if (inserted)
            {
                it->second = std::move(ent);
                it->second.file = fileKey;
                return;
            }
            ent.file = fileKey;
            const CxxNamespaceEntity& prev = it->second;
            if (prev.file == ent.file) return;
            if (prev.kind == "function" && ent.kind == "function") return;
            if (prev.kind == ent.kind && (prev.identity.empty() || ent.identity.empty())) return;
            if (prev.identity == ent.identity) return;
            auto base = [](const std::string& f) {
                return std::filesystem::path(f).filename().string();
            };
            LogErrorMessage("C++ declaration conflicts with a declaration already in scope: '{}' is {} "
                            "in '{}' but {} in '{}'",
                            { name, ent.describe, base(*ent.file), prev.describe, base(*prev.file) });
        };
        for (const CSigEntry& s : sigs)
            if (s.isCxx)
                note(s.name, { false, "function", "function:" + s.name,
                               "a function", nullptr });
        for (const CEnumEntry& e : enums)
        {
            const std::string of = e.enumType.empty() ? std::string{} : " of '" + e.enumType + "'";
            note(e.name, { false, "enumerator",
                           "enumerator:" + e.enumType + "=" + std::to_string(e.value),
                           "an enumerator" + of, nullptr });
            if (!e.enumType.empty())
                note(e.enumType, { true, "enum", "enum:" + e.enumType, "an enum", nullptr });
        }
        // A class is identified by its canonical qualified name, so a forward declaration, a
        // definition and a `using` of the same class all read as one entity.
        std::unordered_set<std::string> batchClasses;
        for (const CRecordEntry& r : records)
        {
            batchClasses.insert(r.name);
            const bool isTemplateSpecialization = r.canonicalCtype.find('<') != std::string::npos;
            note(r.name, { true, "class",
                           (isTemplateSpecialization ? "class-template:" : "class:") + r.name,
                           "a class", nullptr });
        }
        // A class template also occupies the value namespace: clang refuses it against a function,
        // a variable or an enumerator of the same name (a plain class does not, so it stays silent).
        for (const std::string& t : classTemplateNames)
        {
            batchClasses.insert(t);
            note(t, { true, "class", "class-template:" + t, "a class template", nullptr });
            note(t, { false, "class template", "class-template:" + t, "a class template", nullptr });
        }
        // Variable templates share their name with class declarations and value entities in clang.
        for (const std::string& t : variableTemplateNames)
        {
            note(t, { true, "variable template", "variable-template:" + t,
                      "a variable template", nullptr });
            note(t, { false, "variable template", "variable-template:" + t,
                      "a variable template", nullptr });
        }
        for (const auto& ft : functionTemplates)
            if (ft.kind == cflat_cinterop::RawFunctionTemplate::Free && !ft.name.empty())
                note(ft.name, { false, "function", "function-template:" + ft.name,
                                "a function template", nullptr });
        for (const auto& [alias, target] : classUsings)
            note(alias, { true, "alias", "class:" + target, "an alias of '" + target + "'", nullptr });
        // A per-group static (__cflat_sv_<16 hex>_<mangled>) is the same declaration in every group.
        auto globalIdentity = [](const std::string& linkage) {
            constexpr std::string_view prefix = "__cflat_sv_";
            constexpr size_t aliasLength = prefix.size() + 16 + 1;
            if (linkage.size() > aliasLength && linkage.starts_with(prefix) && linkage[aliasLength - 1] == '_')
                return linkage.substr(aliasLength);
            return linkage;
        };
        for (const CGlobalEntry& g : globals)
            note(g.name, { false, "variable", globalIdentity(g.linkageName), "a variable", nullptr });
        for (const CTypeAliasEntry& a : aliases)
        {
            const std::string& name = a.qualifiedName.empty() ? a.name : a.qualifiedName;
            // `typedef W<int> Name;` names a class (type) and a value-namespace entity, as clang
            // refuses it against a same-named class, class template, function, variable or enumerator.
            if (!a.cxxSpecialization.empty() && !a.isCxxAliasTemplate && !a.target.empty())
            {
                const std::string spec = "specialization:" + a.cxxSpecialization;
                note(name, { true, "alias", spec, "an alias of '" + a.cxxSpecialization + "'", nullptr });
                note(name, { false, "alias", spec, "an alias of '" + a.cxxSpecialization + "'", nullptr });
                continue;
            }
            if (!a.cxxSpecialization.empty() || a.isCxxAliasTemplate || a.target.empty()) continue;
            TypeAndValue tv;
            if (!MapCTypeToTypeAndValue(a.target, tv, true) || tv.Pointer || tv.TypeName == name)
                continue;
            // Compare only aliases of a named enum or record; spelling noise on builtin
            // typedefs across TUs must not read as two entities.
            const bool isEnum = !GetEnumBackingType(tv.TypeName).empty();
            if (!isEnum && dataStructures.find(tv.TypeName) == dataStructures.end()
                && !batchClasses.count(tv.TypeName))
                continue;
            note(name, { true, "alias", (isEnum ? "enum:" : "class:") + tv.TypeName,
                         "an alias of '" + tv.TypeName + "'", nullptr });
        }
}

/*
 * Records a C++ global about to bind under `e.name`. Returns true when the name must NOT bind:
 * it is already refused as ambiguous (whatever links the new entry has), or `e` is an
 * internal-linkage variable that another group defines differently. Two import groups own two
 * objects of an internal variable; it stays bound only when they cannot differ for a CFlat
 * read: the same group's object reached again, or a const (folded or not) from the same
 * declaration with the same evaluated value. Mutable storage and differing values refuse.
 * On refusal the bound entry is withdrawn and every use reports the refusal.
 */
bool LLVMBackend::NoteCxxGroupStatic(const CGlobalEntry& e, bool folded, const std::string& importHeader)
{
        const std::string ambiguityPrefix = std::format("'{}' is ambiguous:", e.name);
        if (auto rit = cxxBindingRefusals_.find(e.name);
            rit != cxxBindingRefusals_.end() && rit->second.starts_with(ambiguityPrefix))
            return true;
        if (!e.isInternalLinkage) return false;
        auto it = cxxGroupStatics_.find(e.name);
        if (it == cxxGroupStatics_.end())
        {
            if (globalNamedVariable.count(e.name)) return false;  // not ours; first writer wins
            cxxGroupStatics_[e.name] = { e.file, e.line, e.col, e.linkageName, folded, e.isConst,
                                         e.constantValue, e.floatValue, e.constInitHash, importHeader };
            return false;
        }
        const CxxGroupStaticOrigin& prev = it->second;
        // One header reached through two groups may be spelled with different separators or
        // `.`/`..` segments (Windows: `\` vs `/`); compare file identity, never the lexical form
        // (`link/../a.h` through a symlink is another file). Unresolvable paths stay distinct.
        std::error_code sameEc;
        const bool sameDeclFile = prev.declFile == e.file
            || (!prev.declFile.empty() && !e.file.empty()
                && std::filesystem::equivalent(prev.declFile, e.file, sameEc) && !sameEc);
        bool same = prev.folded == folded;
        if (same && folded)
            same = sameDeclFile && prev.line == e.line && prev.col == e.col
                   && (e.isFloatConstant ? std::bit_cast<uint64_t>(prev.floatValue)
                                               == std::bit_cast<uint64_t>(e.floatValue)
                                         : prev.constantValue == e.constantValue);
        else if (same && prev.linkageName != e.linkageName)
            // Another group's object. A const one of the same declaration with the same evaluated
            // value reads alike; anything else (mutable, another value) is ambiguous.
            same = e.isConst && prev.isConst && e.constInitHash != 0
                   && prev.constInitHash == e.constInitHash && sameDeclFile
                   && prev.line == e.line && prev.col == e.col;
        if (same) return true;
        auto base = [](const std::string& f) {
            return std::filesystem::path(f).filename().string();
        };
        // Grouping imports helps only one declaration reached twice; distinct definitions are a
        // clang redefinition, so their hint is rename or remove.
        const bool sharedDecl = sameDeclFile && prev.line == e.line && prev.col == e.col;
        if (!sharedDecl)
            cxxBindingRefusals_[e.name] = std::format(
                "{} C++ internal-linkage variable '{}' is defined in both '{}' and '{}' as distinct "
                "definitions; CFlat cannot tell which one is meant. Rename one of the conflicting "
                "declarations or remove one so a single '{}' remains;",
                ambiguityPrefix, e.name, base(prev.declFile.empty() ? prev.importHeader : prev.declFile),
                base(e.file.empty() ? importHeader : e.file), e.name);
        else
            cxxBindingRefusals_[e.name] = std::format(
                "{} C++ internal-linkage variable '{}' from '{}' has one copy per import group, and "
                "the imports of '{}' and '{}' each reach it; CFlat cannot tell which one is meant. "
                "Import them in one group so the variable has one copy: import cpp {{\"{}\", \"{}\"}};",
                ambiguityPrefix, e.name, base(e.file.empty() ? importHeader : e.file),
                base(prev.importHeader), base(importHeader),
                base(prev.importHeader), base(importHeader));
        globalNamedVariable.erase(e.name);
        constGlobalInts_.erase(e.name);
        cxxGroupStatics_.erase(it);
        return true;
}

void LLVMBackend::RegisterCGlobals(const std::vector<CGlobalEntry>& globals, const std::string& fileForLsp)
{
        for (const CGlobalEntry& e : globals)
        {
            if (e.name.empty()) continue;
            if (!e.bindRefusal.empty())
            {
                if (!globalNamedVariable.count(e.name))
                    cxxBindingRefusals_[e.name] = e.bindRefusal;
                continue;
            }
            if (!globalNamedVariable.count(e.name) && e.type.ConstArraySize > 0 && !e.type.Pointer
                && cxxNontrivialRecords_.count(e.type.TypeName) != 0)
            {
                std::string arrayType = e.type.TypeName;
                arrayType += std::format("[{}]", e.type.ConstArraySize);
                for (uint64_t dim : e.type.ConstInnerDimensions)
                    arrayType += std::format("[{}]", dim);
                cxxBindingRefusals_[e.name] = std::format(
                    "array type '{}' has a non-trivial element type and global '{}' cannot be bound",
                    arrayType, e.name);
                continue;
            }
            if (auto it = cxxBindingRefusals_.find(e.name);
                it != cxxBindingRefusals_.end() && it->second.starts_with("global array '"))
                cxxBindingRefusals_.erase(it);
            // Each import group owns its C++ internal statics (V14), so a CFlat use of a name two
            // groups define as two objects has no single referent: it is refused as ambiguous.
            const bool folded = e.isCxxConstexpr && e.isCompileTimeConstant && e.linkageName.empty();
            if (NoteCxxGroupStatic(e, folded, fileForLsp)) continue;
            if (globalNamedVariable.count(e.name)) continue;  // first writer wins

            TypeAndValue tv = e.type;
            tv.VariableName = e.name;
            const bool isConstant = e.isCxxConstexpr && e.isCompileTimeConstant;
            llvm::Constant* init = nullptr;
            if (isConstant)
            {
                llvm::Type* valueType = GetType(tv);
                if (valueType->isIntegerTy())
                {
                    init = llvm::ConstantInt::get(
                        valueType,
                        llvm::APInt(valueType->getIntegerBitWidth(), (uint64_t)e.constantValue,
                                    /*isSigned=*/true, /*implicitTrunc=*/true));
                }
                else if (e.isFloatConstant && valueType->isFloatingPointTy())
                    init = llvm::ConstantFP::get(valueType, e.floatValue);
                else
                {
                    if (verbose)
                        std::cout << std::format(
                            "[verbose]   C++ namespace variable {} not bound: mapped type is not an integer or floating point type",
                            e.name) << "\n";
                    continue;
                }
            }

            for (size_t pos = 0; (pos = e.name.find('.', pos)) != std::string::npos; ++pos)
                RegisterNamespace(e.name.substr(0, pos));
            if (e.isCxxConstexpr || !e.linkageName.empty()) NoteCxxForeignNamespace(e.name);
            auto* global = CreateGlobalVariable(tv, init, /*threadLocal*/ false,
                /*userAlign*/ 0, /*externalDecl*/ !isConstant, /*srcIsUnsigned*/ false,
                e.linkageName);
            if (e.isDllImport && global != nullptr && global->isDeclaration())
                global->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
            if (global != nullptr && !e.fromCxxHeader) cGlobalSymbols_.insert(global->getName().str());
            if (global != nullptr && e.fromCxxHeader) cxxHeaderGlobalSymbols_.insert(global->getName().str());
            if (isConstant)
            {
                SetConstGlobalInt(e.name, e.constantValue);
                SetConstGlobalInt(global->getName().str(), e.constantValue);
            }

            if (auto* s = GetSymbolSink())
            {
                const std::string typeText = isConstant
                    ? SpellType(*this, tv)
                    : tv.TypeName + (tv.Pointer ? "*" : "");
                s->Register(SymbolKind::Variable, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col,
                            typeText + " " + e.name
                                + (isConstant ? ConstIntValueSuffix(tv.TypeName, e.constantValue)
                                              : ""));
            }
        }
        RegisterCMacroAliases({}, {}, fileForLsp);
        if (verbose)
            std::cout << std::format("[verbose]   registered {} C global(s) from {}\n", globals.size(), fileForLsp);
    }

/*
 * Add unnamed [N x u8] filler fields so the LLVM struct reproduces clang's field offsets and
 * any data clang places past the last stored field; trailing padding for an over-aligned record
 * is added by CreateStructType from the record's alignment. A padding field carries an empty VariableName,
 * which every consumer (LSP registration, member lookup) already treats as a synthetic slot.
 */
void LLVMBackend::InsertCxxLayoutPadding(const CRecordEntry& r, std::vector<DeclTypeAndValue>& fields)
{
        if (fields.size() != r.fields.size()) return;   // shape changed; the verifier reports it
        const llvm::DataLayout& dl = module->getDataLayout();
        std::vector<DeclTypeAndValue> out;
        out.reserve(fields.size());
        uint64_t at = 0;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            llvm::Type* ft = GetType(fields[i]);
            if (ft == nullptr || !ft->isSized()) return;
            uint64_t want = r.fields[i].offsetBytes;
            uint64_t natural = r.isPacked
                ? at
                : llvm::alignTo(at, dl.getABITypeAlign(ft).value());
            if (want < natural) return;                 // cannot be reached by padding; verifier reports
            if (want > natural)
            {
                DeclTypeAndValue pad;
                pad.TypeName = "u8";
                pad.ConstArraySize = want - natural;
                out.push_back(pad);
                at = want;
            }
            else
                at = natural;
            at += (uint64_t)dl.getTypeAllocSize(ft);
            out.push_back(fields[i]);
        }
        // Clang's data can end past the last stored field: an empty [[no_unique_address]]
        // member that may not share its address with a same-typed one (join_view's caches).
        if (!r.isPacked && r.alignBytes != 0 && r.sizeBytes > llvm::alignTo(at, r.alignBytes))
        {
            DeclTypeAndValue pad;
            pad.TypeName = "u8";
            pad.ConstArraySize = r.sizeBytes - at;
            out.push_back(pad);
        }
        fields.swap(out);
    }

std::string LLVMBackend::VerifyImportedRecordLayout(const CRecordEntry& r)
{
        std::string mismatch;
        auto it = dataStructures.find(r.name);
        if (it == dataStructures.end() || it->second.StructType == nullptr) return mismatch;
        llvm::StructType* st = it->second.StructType;
        if (st->isOpaque() || !st->isSized()) return mismatch;
        if (r.sizeBytes == 0) return mismatch;   // clang reported no layout (opaque forward declaration)

        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = (uint64_t)dl.getTypeAllocSize(st);
        uint64_t align = std::max<uint64_t>(dl.getABITypeAlign(st).value(),
                                            it->second.UserRequestedAlignment);
        auto refuse = [&](const std::string& detail) {
            mismatch = "layout is not representable: " + detail;
            return mismatch;
        };
        if (size != r.sizeBytes)
            return refuse(std::format("cflat lays it out as {} bytes, clang as {}", size, r.sizeBytes));
        if (align != r.alignBytes)
            return refuse(std::format("cflat aligns it to {}, clang to {}", align, r.alignBytes));
        for (const auto& f : it->second.StructFields)
            if (auto anon = cxxAnonLayoutRefusals_.find(f.TypeName); anon != cxxAnonLayoutRefusals_.end())
                return refuse(std::format("anonymous member '{}': {}", f.VariableName, anon->second));
        if (r.isUnion)
        {
            for (const auto& cf : r.fields)
            {
                if (!cf.isBitfield || cf.name.empty()) continue;
                auto bit = std::find_if(it->second.Bitfields.begin(), it->second.Bitfields.end(),
                    [&](const BitfieldInfo& b) { return b.Name == cf.name; });
                if (bit == it->second.Bitfields.end())
                    return refuse(std::format("bitfield '{}' was not recorded in cflat", cf.name));
                if (bit->BitOffset != cf.bitOffset)
                    return refuse(std::format("bitfield '{}' sits at bit {} in cflat and bit {} in clang",
                                              cf.name, bit->BitOffset, cf.bitOffset));
            }
            return mismatch;  // non-bitfield union members all start at offset 0
        }

        const llvm::StructLayout* sl = dl.getStructLayout(st);
        // Compare absolute BIT positions: clang's byte offset of a bitfield is its bit offset / 8,
        // which lands inside the storage unit for any bit past the first byte.
        auto checkBitfield = [&](const CRecordFieldEntry& cf, size_t* elem) -> bool {
            if (cf.name.empty()) return true;  // unnamed bitfields only reserve storage
            auto bit = std::find_if(it->second.Bitfields.begin(), it->second.Bitfields.end(),
                                    [&](const BitfieldInfo& b) { return b.Name == cf.name; });
            if (bit == it->second.Bitfields.end())
            {
                refuse(std::format("bitfield '{}' was not recorded in cflat", cf.name));
                return false;
            }
            if (bit->StorageFieldIndex >= st->getNumElements())
            {
                refuse(std::format("bitfield '{}' has an invalid storage slot", cf.name));
                return false;
            }
            const uint64_t bitAt = sl->getElementOffset(bit->StorageFieldIndex) * 8 + bit->BitOffset;
            if (bitAt != cf.bitOffset)
            {
                refuse(std::format("bitfield '{}' sits at bit {} in cflat and bit {} in clang",
                                   cf.name, bitAt, cf.bitOffset));
                return false;
            }
            if (elem != nullptr) *elem = std::max<size_t>(*elem, bit->StorageFieldIndex + 1);
            return true;
        };

        if (it->second.CxxOffsetLayout && !it->second.Bitfields.empty())
        {
            // Physical slots come first and promoted/zero-size semantic fields are offset-mapped.
            const auto& data = it->second;
            if (data.CxxFieldOffsets.size() != data.StructFields.size()
                || data.CxxFieldElements.size() != data.StructFields.size())
                return refuse("zero-size field offset map does not match the recorded field list");
            for (const auto& cf : r.fields)
            {
                if (cf.isBitfield)
                {
                    if (!checkBitfield(cf, nullptr)) return mismatch;
                    continue;
                }
                size_t j = 0;
                while (j < data.StructFields.size() && (data.StructFields[j].IsBitfield
                        || data.StructFields[j].VariableName != cf.name)) ++j;
                if (cf.name.empty() || j == data.StructFields.size())
                    return refuse(std::format("field '{}' was not recorded in cflat", cf.name));
                if (cf.offsetBytes != data.CxxFieldOffsets[j])
                    return refuse(std::format("field '{}' offset map is {}, clang reports {}",
                        cf.name, data.CxxFieldOffsets[j], cf.offsetBytes));
                const unsigned element = data.CxxFieldElements[j];
                if (cf.isZeroSize || cf.isPromoted)
                {
                    if (element != std::numeric_limits<unsigned>::max())
                        return refuse(std::format("field '{}' has unexpected physical storage", cf.name));
                    if (cf.offsetBytes > r.sizeBytes
                        || (cf.isPromoted && cf.sizeBytes > r.sizeBytes - cf.offsetBytes))
                        return refuse(std::format("field '{}' lies past the record", cf.name));
                    continue;
                }
                if (element >= st->getNumElements() || sl->getElementOffset(element) != cf.offsetBytes)
                    return refuse(std::format("field '{}' does not sit at clang's byte {}",
                        cf.name, cf.offsetBytes));
            }
            return mismatch;
        }

        if (it->second.CxxOffsetLayout)
        {
            const auto& data = it->second;
            if (data.CxxFieldOffsets.size() != data.StructFields.size()
                || data.CxxFieldElements.size() != data.StructFields.size())
                return refuse("zero-size field offset map does not match the recorded field list");
            const llvm::StructLayout* byteLayout = dl.getStructLayout(st);
            for (const auto& field : r.fields)
            {
                if (field.isBitfield)
                {
                    if (field.bitWidth == 0 || field.name.empty()) continue;
                    if (!checkBitfield(field, nullptr)) return mismatch;
                    continue;
                }
                size_t i = 0;
                while (i < data.StructFields.size()
                    && data.StructFields[i].VariableName != field.name) ++i;
                if (field.name.empty() || i == data.StructFields.size())
                    return refuse(std::format("field '{}' was not recorded in cflat", field.name));
                if (field.offsetBytes != data.CxxFieldOffsets[i])
                    return refuse(std::format("field '{}' offset map is {}, clang reports {}",
                        field.name, data.CxxFieldOffsets[i], field.offsetBytes));
                const unsigned element = data.CxxFieldElements[i];
                if (field.isZeroSize || field.isPromoted)
                {
                    if (element != std::numeric_limits<unsigned>::max())
                        return refuse(std::format("field '{}' has unexpected physical storage", field.name));
                    if (field.offsetBytes > r.sizeBytes
                        || (field.isPromoted && field.sizeBytes > r.sizeBytes - field.offsetBytes))
                        return refuse(std::format("field '{}' lies past the record", field.name));
                    continue;
                }
                if (element >= st->getNumElements())
                    return refuse(std::format("field '{}' has an invalid physical slot", field.name));
                const uint64_t actual = byteLayout->getElementOffset(element);
                if (actual != field.offsetBytes)
                    return refuse(std::format("field '{}' sits at byte {} in cflat and byte {} in clang",
                        field.name, actual, field.offsetBytes));
            }
            return mismatch;
        }

        const auto& decl = it->second.StructFields;
        size_t elem = 0;
        for (const auto& cf : r.fields)
        {
            if (cf.isBitfield)
            {
                if (!checkBitfield(cf, &elem)) return mismatch;
                continue;
            }
            // Skip the synthetic padding slots InsertCxxLayoutPadding added.
            while (elem < decl.size() && decl[elem].VariableName.empty()) ++elem;
            if (elem >= decl.size() || elem >= st->getNumElements()) return mismatch;
            uint64_t off = sl->getElementOffset((unsigned)elem);
            if (off != cf.offsetBytes)
                return refuse(std::format("field '{}' sits at byte {} in cflat and byte {} in clang",
                                          cf.name, off, cf.offsetBytes));
            ++elem;
        }
        return mismatch;
    }

bool LLVMBackend::InsertCBitfieldLayoutPadding(const CRecordEntry& r,
                                               std::vector<DeclTypeAndValue>& fields,
                                               std::vector<BitfieldInfo>& bitfields)
{
        auto candidateFields = fields;
        auto candidateBitfields = bitfields;
        if (!TryInsertCBitfieldLayoutPadding(r, candidateFields, candidateBitfields)) return false;
        fields.swap(candidateFields);
        bitfields.swap(candidateBitfields);
        return true;
    }

bool LLVMBackend::TryInsertCBitfieldLayoutPadding(const CRecordEntry& r,
                                                  std::vector<DeclTypeAndValue>& fields,
                                                  std::vector<BitfieldInfo>& bitfields)
{
        if (r.isUnion || fields.empty()) return false;
        const llvm::DataLayout& dl = module->getDataLayout();
        std::vector<uint64_t> desired(fields.size());
        std::vector<uint64_t> storageStart(fields.size(), std::numeric_limits<uint64_t>::max());
        std::vector<bool> matchedBitfields(r.fields.size(), false);
        for (const auto& bf : bitfields)
        {
            if (bf.StorageFieldIndex >= fields.size()) return false;
            auto cf = std::find_if(r.fields.begin(), r.fields.end(), [&](const CRecordFieldEntry& f) {
                return f.isBitfield && f.name == bf.Name;
            });
            if (cf == r.fields.end()) return false;
            matchedBitfields[static_cast<size_t>(cf - r.fields.begin())] = true;
            auto& start = storageStart[bf.StorageFieldIndex];
            start = std::min(start, cf->bitOffset / 8);
        }
        for (size_t i = 0; i < fields.size(); ++i)
        {
            if (!fields[i].IsBitfieldStorage
                || storageStart[i] != std::numeric_limits<uint64_t>::max()) continue;
            auto cf = r.fields.begin();
            while (cf != r.fields.end())
            {
                const size_t index = static_cast<size_t>(cf - r.fields.begin());
                if (cf->isBitfield && cf->name.empty() && cf->bitWidth != 0
                    && !matchedBitfields[index]) break;
                ++cf;
            }
            if (cf == r.fields.end()) return false;
            matchedBitfields[static_cast<size_t>(cf - r.fields.begin())] = true;
            storageStart[i] = cf->bitOffset / 8;
        }
        for (size_t i = 0; i < fields.size(); ++i)
        {
            if (!fields[i].IsBitfieldStorage) continue;
            unsigned usedBits = 0;
            for (const auto& bf : bitfields)
            {
                if (bf.StorageFieldIndex != i) continue;
                auto cf = std::find_if(r.fields.begin(), r.fields.end(), [&](const CRecordFieldEntry& f) {
                    return f.isBitfield && f.name == bf.Name;
                });
                if (cf == r.fields.end() || cf->bitOffset < storageStart[i] * 8) return false;
                usedBits = std::max<unsigned>(usedBits,
                    static_cast<unsigned>(cf->bitOffset - storageStart[i] * 8) + cf->bitWidth);
            }
            if (usedBits != 0)
            {
                if (r.isCxx && !r.isPacked)
                {
                    fields[i].TypeName = "u8";
                    fields[i].ConstArraySize = (usedBits + 7) / 8;
                }
                else
                {
                    fields[i].TypeName = usedBits <= 8 ? "u8" : usedBits <= 16 ? "u16"
                        : usedBits <= 32 ? "u32" : usedBits <= 64 ? "u64" : fields[i].TypeName;
                }
            }
        }

        for (size_t i = 0; i < fields.size(); ++i)
        {
            if (fields[i].IsBitfieldStorage)
            {
                if (storageStart[i] == std::numeric_limits<uint64_t>::max()) return false;
                desired[i] = storageStart[i];
                continue;
            }
            auto cf = std::find_if(r.fields.begin(), r.fields.end(), [&](const CRecordFieldEntry& f) {
                return !f.isBitfield && !f.name.empty() && f.name == fields[i].VariableName;
            });
            if (cf == r.fields.end()) return false;
            desired[i] = cf->offsetBytes;
        }

        std::vector<DeclTypeAndValue> out;
        out.reserve(fields.size() * 2);
        std::vector<unsigned> newIndices(fields.size());
        uint64_t at = 0;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            llvm::Type* ft = GetType(fields[i]);
            if (ft == nullptr || !ft->isSized()) return false;
            const uint64_t natural = r.isPacked
                ? at
                : llvm::alignTo(at, GetFieldSlotAlignment(fields[i], ft));
            if (desired[i] < natural) return false;
            if (desired[i] > natural)
            {
                DeclTypeAndValue pad;
                pad.TypeName = "u8";
                pad.ConstArraySize = desired[i] - natural;
                out.push_back(std::move(pad));
            }
            at = desired[i];
            newIndices[i] = static_cast<unsigned>(out.size());
            out.push_back(fields[i]);
            at += static_cast<uint64_t>(dl.getTypeAllocSize(ft));
        }
        // MS ABI keeps a packed record's last bitfield unit at its declared size; pad to clang's size.
        if (r.isPacked && r.sizeBytes > at)
        {
            DeclTypeAndValue pad;
            pad.TypeName = "u8";
            pad.ConstArraySize = r.sizeBytes - at;
            out.push_back(std::move(pad));
        }
        for (auto& bf : bitfields)
        {
            const unsigned oldIndex = bf.StorageFieldIndex;
            const auto cf = std::find_if(r.fields.begin(), r.fields.end(), [&](const CRecordFieldEntry& f) {
                return f.isBitfield && f.name == bf.Name;
            });
            if (cf == r.fields.end() || cf->bitOffset < storageStart[oldIndex] * 8) return false;
            bf.StorageFieldIndex = newIndices[oldIndex];
            bf.BitOffset = static_cast<unsigned>(cf->bitOffset - storageStart[oldIndex] * 8);
        }
        fields.swap(out);
        return true;
    }

// An opaque, correctly sized and aligned stand-in for a C++ field whose type cflat cannot map.
// Use an integer array for the bytes and carry over-alignment separately.
bool LLVMBackend::MakeOpaqueFieldBlob(const CRecordFieldEntry& f, DeclTypeAndValue& out) const
{
        if (f.sizeBytes == 0 || f.alignBytes == 0) return false;
        uint64_t unit = std::min<uint64_t>(f.alignBytes, 8);
        while (unit > 1 && f.sizeBytes % unit != 0) unit /= 2;
        out = DeclTypeAndValue{};
        out.TypeName = unit == 8 ? "u64" : unit == 4 ? "u32" : unit == 2 ? "u16" : "u8";
        out.ConstArraySize = f.sizeBytes / unit;
        if (f.alignBytes > 8) out.UserAlignValue = f.alignBytes;
        out.VariableName = f.name;
        out.IsAnonymousMember = f.isAnonymousMember;
        MarkCxxDeclaredConstField(out, f.isConst);
        out.IsCxxMutableField = f.isMutable;
        return true;
}

void LLVMBackend::RegisterGeneratedCxxOverrideNames(const std::string& typeName,
                                                     const std::set<std::string>& names)
{
        auto it = generatedCxxRecords_.find(typeName);
        if (it != generatedCxxRecords_.end()) it->second.overrideNames = names;
}

/*
 * The record a BY-VALUE field names, normalized to the spelling records are keyed on. Empty for a
 * pointer, a reference or a function type: those need no layout, so they create no ordering edge.
 */
std::string LLVMBackend::ValueFieldRecordKey(const std::string& ctype)
{
        std::string s = ctype;
        if (s.find('(') != std::string::npos) return {};
        if (auto br = s.find('['); br != std::string::npos) s.erase(br);
        if (s.find('*') != std::string::npos || s.find('&') != std::string::npos) return {};
        auto trim = [](std::string& t) {
            while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
            const size_t a = t.find_first_not_of(" \t");
            if (a == std::string::npos) t.clear();
            else if (a > 0) t.erase(0, a);
        };
        trim(s);
        for (bool peeled = true; peeled; )
        {
            peeled = false;
            for (const char* w : { "const ", "volatile ", "struct ", "class ", "union ", "enum " })
                if (s.rfind(w, 0) == 0) { s.erase(0, std::strlen(w)); peeled = true; break; }
            trim(s);
        }
        if (s.empty()) return {};
        return SqueezeCxxSpelling(s);
}

void LLVMBackend::RegisterCxxRecordShell(const CRecordEntry& r, const std::string& fileForLsp)
{
        if (r.name.empty()) return;
        NoteCxxForeignNamespace(r.name);
        if (activeCxxRequestGroup_ != nullptr)
            cxxTypeOwnerGroup_.emplace(r.name, activeCxxRequestGroup_->primary);
        if (r.specialMembersPending && activeCxxRequestGroup_ != nullptr)
        {
            cxxSpecialMemberOwner_[r.name] = activeCxxRequestGroup_->primary;
            if (const std::string key = ValueFieldRecordKey(r.canonicalCtype); !key.empty())
                cxxPendingSpecialBySpelling_.emplace(key, r.name);
        }
        for (size_t pos = 0; (pos = r.name.find('.', pos)) != std::string::npos; ++pos)
            RegisterNamespace(r.name.substr(0, pos));
        if (dataStructures.find(r.name) == dataStructures.end())
            CreateStructType(r.name, {});
        cxxRecords_.insert(r.name);
        if (r.isTriviallyRelocatable) cxxTriviallyRelocatableRecords_.insert(r.name);
        const bool stdValueRecord = r.name.starts_with("std.pair$")
                                  || r.name.starts_with("std.optional$")
                                  || r.name.starts_with("std.tuple$");
        if (r.isTriviallyCopyable || (stdValueRecord && r.hasTrivialDtor))
            cxxTriviallyCopyableRecords_.insert(r.name);
        else if (!r.isUnion)
            cxxNontrivialRecords_.insert(r.name);
        StoreCxxRecordEntry(r.name, r);
        pendingCxxRecordProjections_[r.name] = fileForLsp;
        // Same spelling maps RegisterCxxClassMembers builds (identity keys: RegisterCRecords).
        RememberCxxRecordSpellings(r);
        if (auto* s = GetSymbolSink())
            s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line, r.col < 0 ? 0 : r.col,
                        (r.isUnion ? "union " : "class ")
                            + SpellType(*this, TypeAndValue{ .TypeName = r.name }));
}

void LLVMBackend::EnsureCxxRecordLayoutsForMemberSignatures(const CRecordEntry& record,
                                                             const std::string& memberFilter)
{
        auto ensure = [&](const std::string& spelling, bool allowPointerRequest) {
            TypeAndValue type;
            bool mapped = MapCTypeToTypeAndValue(spelling, type, true);
            // std::function specializations are requested by the lazy std::function bind under
            // their closure identity; a spelling-derived identity here would be a second type.
            const size_t templateClose = spelling.rfind('>');
            auto pointerCvRefSuffixOnly = [&]() {
                if (templateClose == std::string::npos) return false;
                for (size_t i = templateClose + 1; i < spelling.size(); ++i)
                    if (spelling[i] != '*' && spelling[i] != '&'
                        && !std::isspace((unsigned char)spelling[i]))
                    {
                        // cv qualifiers are accepted as complete words only.
                        const std::string_view tail(spelling.data() + i, spelling.size() - i);
                        if (!tail.starts_with("const") && !tail.starts_with("volatile")) return false;
                        if (tail.starts_with("const") && tail.size() > 5
                            && !std::isspace((unsigned char)tail[5]) && tail[5] != '*' && tail[5] != '&')
                            return false;
                        if (tail.starts_with("volatile") && tail.size() > 8
                            && !std::isspace((unsigned char)tail[8]) && tail[8] != '*' && tail[8] != '&')
                            return false;
                        i += tail.starts_with("const") ? 4 : 7;
                    }
                return true;
            };
            const bool pointerToTemplateSpecialization = templateClose != std::string::npos
                && spelling.find('<') != std::string::npos
                && spelling.find('*', templateClose) != std::string::npos
                && pointerCvRefSuffixOnly();
            const bool mappedAsOpaquePointer = mapped && type.Pointer
                && (type.TypeName.empty() || type.TypeName == "void");
            std::string topLevel;
            int templateDepth = 0;
            for (char ch : spelling)
            {
                if (ch == '<') ++templateDepth;
                else if (ch == '>') --templateDepth;
                else if (templateDepth == 0) topLevel += ch;
            }
            const bool pointerSpelling = topLevel.find('*') != std::string::npos;
            const bool referenceToTemplateSpecialization = templateClose != std::string::npos
                && spelling.find('<') != std::string::npos
                && topLevel.find('&') != std::string::npos
                && topLevel.find("&&") == std::string::npos
                && pointerCvRefSuffixOnly();
            bool hasPointerRefTemplateArgument = false;
            std::vector<bool> nestedTemplateInArgument;
            for (char ch : spelling)
            {
                if (ch == '<')
                {
                    if (!nestedTemplateInArgument.empty())
                        nestedTemplateInArgument.back() = true;
                    nestedTemplateInArgument.push_back(false);
                }
                else if (ch == '>')
                {
                    if (!nestedTemplateInArgument.empty()) nestedTemplateInArgument.pop_back();
                }
                else if (ch == ',' && !nestedTemplateInArgument.empty())
                    nestedTemplateInArgument.back() = false;
                // A '&' after a nested template (valarray's `const valarray<T>&` operands) stays
                // opaque; a '*' always counts (deque<vector<int>>::iterator).
                else if ((ch == '*' || ch == '&') && !nestedTemplateInArgument.empty()
                         && (ch == '*' || !nestedTemplateInArgument.back()))
                    hasPointerRefTemplateArgument = true;
            }
            const bool byValueTemplate = templateClose != std::string::npos && !pointerSpelling
                && topLevel.find('&') == std::string::npos
                && hasPointerRefTemplateArgument;
            const std::string valueSpelling = CxxMemberValueSpelling(spelling);
            std::string stdFunctionIdentitySpelling = spelling;
            for (bool changed = true; changed; )
            {
                changed = false;
                while (!stdFunctionIdentitySpelling.empty()
                       && std::isspace((unsigned char)stdFunctionIdentitySpelling.back()))
                    stdFunctionIdentitySpelling.pop_back();
                if (!stdFunctionIdentitySpelling.empty()
                    && (stdFunctionIdentitySpelling.back() == '*'
                        || stdFunctionIdentitySpelling.back() == '&'))
                {
                    stdFunctionIdentitySpelling.pop_back();
                    changed = true;
                    continue;
                }
                for (const char* cv : { "const", "volatile" })
                {
                    const size_t n = std::strlen(cv);
                    if (stdFunctionIdentitySpelling.size() > n
                        && stdFunctionIdentitySpelling.compare(
                            stdFunctionIdentitySpelling.size() - n, n, cv) == 0
                        && std::isspace((unsigned char)stdFunctionIdentitySpelling[
                            stdFunctionIdentitySpelling.size() - n - 1]))
                    {
                        stdFunctionIdentitySpelling.erase(stdFunctionIdentitySpelling.size() - n);
                        changed = true;
                        break;
                    }
                }
            }
            const bool stdFunctionIdentity = IsTopLevelStdFunctionSpelling(stdFunctionIdentitySpelling);
            const bool canRequest = allowPointerRequest || !pointerSpelling;
            if (canRequest && (!mapped
                               || ((pointerToTemplateSpecialization || byValueTemplate)
                                   && mappedAsOpaquePointer))
                && !memberFilter.empty() && !stdFunctionIdentity
                && (byValueTemplate
                    || (allowPointerRequest && referenceToTemplateSpecialization)
                    || (spelling.find('*') == std::string::npos
                        && spelling.find('&') == std::string::npos)
                    || pointerToTemplateSpecialization))
            {
                const std::string identity = CanonicalCxxForeignIdentity(valueSpelling);
                auto owner = cxxTypeOwnerGroup_.find(record.name);
                if (valueSpelling.find('<') != std::string::npos && !identity.empty()
                    && owner != cxxTypeOwnerGroup_.end())
                {
                    CxxRequestGroup group = MakeCxxRequestGroup(owner->second, {});
                    if (!group.headers.empty())
                    {
                        CxxRequestGroupScope groupScope(*this, &group);
                        std::string error;
                        RequestCxxForeignType(identity, valueSpelling, error,
                                              /*needDefinitions*/ true,
                                              /*explicitInstantiation*/ true);
                        mapped = MapCTypeToTypeAndValue(spelling, type, true);
                    }
                }
            }
            // A `const T&` / `T&` crosses as an alias of T, whose by-value shape needs T laid out;
            // a specialization known but still pending would refuse the overload, order-dependently.
            if (mapped && type.Pointer && !type.ElemPointer && !type.IsFunctionPointer
                && !type.TypeName.empty() && type.TypeName != "void"
                && pendingCxxRecordProjections_.count(type.TypeName) != 0
                && !pointerSpelling && topLevel.find('&') != std::string::npos
                && topLevel.find("&&") == std::string::npos)
                EnsureCxxRecordProjected(type.TypeName, false);
            if (!mapped
                || type.Pointer || type.ElemPointer)
                return;
            if (pendingCxxRecordProjections_.count(type.TypeName) != 0
                || pendingCxxMemberProjections_.count(type.TypeName) != 0)
                EnsureCxxRecordProjected(type.TypeName, false);
            if (memberFilter != "__ctor" && memberFilter != "__dtor"
                && memberFilter != "__copy_move_ctor"
                && pendingCxxMemberProjections_.count(type.TypeName) != 0)
                EnsureCxxMemberProjected(type.TypeName, "__ctor");
        };
        for (const auto& member : record.members)
        {
            if (!memberFilter.empty()
                && !((memberFilter == "__ctor"
                      && member.kind == cflat_cinterop::RawCxxMember::Constructor)
                     || (memberFilter == "__copy_move_ctor"
                         && member.kind == cflat_cinterop::RawCxxMember::Constructor
                         && (member.isCopyCtor || member.isMoveCtor))
                     || (memberFilter == "__dtor"
                         && member.kind == cflat_cinterop::RawCxxMember::Destructor)
                     || member.name == memberFilter))
                continue;
            ensure(member.retType, !memberFilter.empty());
            for (const auto& param : member.paramTypes) ensure(param, false);
        }
        if (memberFilter.empty() || memberFilter == "__ctor")
            for (const auto& ctor : record.ctorTemplates)
                for (const auto& param : ctor.paramTypes) ensure(param, false);
}

bool LLVMBackend::RegisterCxxMembersForProjectedRecord(const std::string& typeName)
{
        const std::string stableTypeName = typeName;
        auto entry = cxxRecordEntries_.find(stableTypeName);
        if (entry == cxxRecordEntries_.end()) return false;
        auto pending = pendingCxxMemberProjections_.find(stableTypeName);
        if (pending == pendingCxxMemberProjections_.end()) return true;
        const CRecordEntry record = entry->second;
        const std::string file = pending->second;
        for (const auto& base : record.bases) EnsureCxxRecordProjected(base.name, true);
        EnsureCxxRecordLayoutsForMemberSignatures(record);
        const auto layout = dataStructures.find(stableTypeName);
        const bool laidOut = layout != dataStructures.end() && layout->second.StructType != nullptr
            && !layout->second.StructType->isOpaque() && layout->second.StructType->isSized();
        const bool oldRegistering = registeringCxxProjection_;
        const bool oldMembers = registeringCxxProjectionMembers_;
        registeringCxxProjection_ = true;
        registeringCxxProjectionMembers_ = true;
        RegisterCxxClassMembers(record, file);
        if (laidOut)
        {
            RegisterCxxInheritedMembers(record);
            for (int round = 0; round < 8; ++round)
                if (!RegisterCxxInheritedMembers(record)) break;
            if (cxxNontrivialRecords_.count(stableTypeName) != 0)
                GetOrCreateCxxClassDestructor(stableTypeName);
        }
        registeringCxxProjection_ = oldRegistering;
        registeringCxxProjectionMembers_ = oldMembers;
        pendingCxxMemberProjections_.erase(stableTypeName);
        return true;
}

bool LLVMBackend::CxxClassHasMemberNamed(const std::string& typeName, const std::string& name)
{
        std::set<std::string> visited;
        std::function<bool(const std::string&)> lookup = [&](const std::string& type) {
            if (!visited.insert(type).second) return false;
            EnsureCxxRecordProjected(type, false);
            auto it = cxxRecordEntries_.find(type);
            if (it == cxxRecordEntries_.end()) return false;
            const CRecordEntry& record = it->second;
            for (const auto& member : record.members)
                if (member.name == name) return true;
            for (const auto& field : record.fields)
                if (field.name == name) return true;
            for (const auto& var : record.staticVars)
                if (var.name == name) return true;
            if (HasCxxFunctionTemplateMember(type, name)) return true;
            const std::vector<cflat_cinterop::RawCxxBase> bases = record.bases;
            for (const auto& base : bases)
                if (lookup(base.name)) return true;
            return false;
        };
        return lookup(typeName);
}

bool LLVMBackend::CxxClassHasScopeName(const std::string& typeName, const std::string& name)
{
        // scopeNames already folds in the bases (extraction walks them), so one record suffices.
        EnsureCxxRecordProjected(typeName, false);
        auto it = cxxRecordEntries_.find(typeName);
        if (it == cxxRecordEntries_.end()) return false;
        const auto& names = it->second.scopeNames;
        return std::find(names.begin(), names.end(), name) != names.end();
}

bool LLVMBackend::EnsureCxxMemberProjected(const std::string& typeName,
                                           const std::string& memberName)
{
        const std::string stableTypeName = typeName;
        const std::string stableMemberName = memberName;
        const bool projected = EnsureCxxMemberProjectedImpl(stableTypeName, stableMemberName);
        if (projected) RetryLazyNestedCxxMemberRefusal(stableTypeName, stableMemberName);
        return projected;
}

void LLVMBackend::RetryLazyNestedCxxMemberRefusal(const std::string& typeName,
                                                  const std::string& memberName)
{
        auto recordIt = cxxRecordEntries_.find(typeName);
        if (recordIt == cxxRecordEntries_.end()) return;
        using Member = cflat_cinterop::RawCxxMember;
        const auto& members = recordIt->second.members;
        /*
         * Top-level parameter cv is not part of the C++ function type: `const int` == `int`,
         * `int *const` == `int *`; pointee cv and references stay distinct.
         */
        auto stripTopLevelCv = [](std::string s) {
            auto trim = [](std::string& t) {
                while (!t.empty() && t.back() == ' ') t.pop_back();
                while (!t.empty() && t.front() == ' ') t.erase(0, 1);
            };
            auto endsWithWord = [](const std::string& t, const char* w) {
                const size_t n = std::strlen(w);
                if (t.size() < n || t.compare(t.size() - n, n, w) != 0) return false;
                const char before = t.size() == n ? ' ' : t[t.size() - n - 1];
                return before == ' ' || before == '*';
            };
            trim(s);
            bool strippedTrailing = false;
            for (const char* w : {"const", "volatile", "const", "volatile"})
                if (endsWithWord(s, w)) { s.resize(s.size() - std::strlen(w)); trim(s); strippedTrailing = true; }
            if (strippedTrailing) return s;
            int depth = 0;
            for (char c : s)
            {
                if (c == '<' || c == '(' || c == '[') ++depth;
                else if (c == '>' || c == ')' || c == ']') --depth;
                if ((c == '*' || c == '&' || c == '(' || c == '[') && (depth == 0 || c == '(' || c == '['))
                    return s;
            }
            for (bool again = true; again;)
            {
                again = false;
                for (const char* w : {"const ", "volatile "})
                    if (s.rfind(w, 0) == 0) { s.erase(0, std::strlen(w)); trim(s); again = true; }
            }
            return s;
        };
        auto explicitParamsEqual = [&](const Member& a, const Member& b) {
            if (a.paramTypes.size() != b.paramTypes.size()) return false;
            for (size_t i = 1; i < a.paramTypes.size(); ++i)
                if (stripTopLevelCv(a.paramTypes[i]) != stripTopLevelCv(b.paramTypes[i])) return false;
            return true;
        };
        /*
         * Every refused signature of the name counts, each by the extractor's structural fact.
         * Without the call's arguments, a refused non-const overload is selectable only where it
         * ties a bound const sibling on every argument and wins on the object (bitset::operator[]).
         */
        std::set<std::string> wanted;
        for (const Member& refused : members)
        {
            if (refused.name != memberName || refused.kind != Member::Instance
                || refused.bindRefusal.empty() || refused.lazyNestedSpelling.empty()
                || refused.isConst || refused.paramTypes.empty())
                continue;
            const bool beatsBoundSibling = std::any_of(members.begin(), members.end(),
                [&](const Member& bound) {
                    return &bound != &refused && bound.name == memberName
                        && bound.kind == Member::Instance && bound.isConst
                        && bound.bindRefusal.empty() && !bound.isDeleted
                        && bound.access == cflat_cinterop::AccessPublic
                        && explicitParamsEqual(bound, refused);
                });
            if (!beatsBoundSibling) continue;
            if (cxxLazyNestedRetried_.insert(typeName + '\x1f' + memberName + '\x1f'
                                             + refused.lazyNestedSpelling).second)
                wanted.insert(refused.lazyNestedSpelling);
        }
        if (!wanted.empty())
            TryBindRefusedCxxMember(typeName, memberName, static_cast<size_t>(-1), &wanted);
}

bool LLVMBackend::EnsureCxxMemberProjectedImpl(const std::string& typeName,
                                               const std::string& memberName)
{
        const std::string stableTypeName = typeName;
        const std::string stableMemberName = memberName;
        if (stableMemberName.empty()) return false;
        // Dot sees the unique_ptr's own members first; project to the pointee only for names
        // it does not own (`->` follows operator-> in the postfix listener).
        if (stableTypeName.starts_with("std.unique_ptr$"))
            if (const std::string pointee = CxxUniquePtrPointee(stableTypeName);
                !pointee.empty() && pointee != stableTypeName
                && !CxxClassHasMemberNamed(stableTypeName, stableMemberName))
                EnsureCxxMemberProjected(pointee, stableMemberName);
        EnsureCxxRecordProjected(stableTypeName, false);
        auto entry = cxxRecordEntries_.find(stableTypeName);
        if (entry == cxxRecordEntries_.end()) return false;
        auto pending = pendingCxxMemberProjections_.find(stableTypeName);
        if (pending == pendingCxxMemberProjections_.end())
        {
            /*
             * Registered eagerly, possibly before its base arrived (MSVC: basic_stringbuf<char>
             * laid out as a basic_stringstream field, basic_streambuf<char> from a later
             * request). Project the name on each known base and clone it now, once per name.
             */
            if (entry->second.bases.empty() || !entry->second.isCxx
                || projectedCxxMemberNames_[stableTypeName].count(stableMemberName) != 0)
                return true;
            const CRecordEntry record = entry->second;
            bool anyBase = false;
            for (const auto& base : record.bases)
                if (const std::string baseRecord = ResolveCxxBaseRecordName(base);
                    !baseRecord.empty() && baseRecord != stableTypeName)
                {
                    EnsureCxxMemberProjected(baseRecord, stableMemberName);
                    anyBase = true;
                }
            if (!anyBase) return true;   // retried once a base record arrives
            projectedCxxMemberNames_[stableTypeName].insert(stableMemberName);
            const auto layout = dataStructures.find(stableTypeName);
            if (cxxClasses_.count(stableTypeName) != 0 && layout != dataStructures.end()
                && layout->second.StructType != nullptr && !layout->second.StructType->isOpaque()
                && layout->second.StructType->isSized())
                RegisterCxxInheritedMembers(record);
            return true;
        }
        if (projectedCxxMemberNames_[stableTypeName].count(stableMemberName) != 0) return true;
        const std::string projectionKey = stableTypeName + '\x1f' + stableMemberName;
        if (!projectingCxxMemberNames_.insert(projectionKey).second) return true;
        struct ProjectionGuard
        {
            std::set<std::string>& active;
            std::string key;
            ~ProjectionGuard() { active.erase(key); }
        } projectionGuard{ projectingCxxMemberNames_, projectionKey };

        const CRecordEntry record = entry->second;
        const std::string file = pending->second;
        auto info = cxxClasses_.find(stableTypeName);
        if (info == cxxClasses_.end())
        {
            const bool oldRegistering = registeringCxxProjection_;
            const bool oldMembers = registeringCxxProjectionMembers_;
            registeringCxxProjection_ = true;
            registeringCxxProjectionMembers_ = false;
            RegisterCxxClassMembers(record, file, "__metadata");
            registeringCxxProjection_ = oldRegistering;
            registeringCxxProjectionMembers_ = oldMembers;
        }

        EnsureCxxRecordLayoutsForMemberSignatures(record, stableMemberName);
        for (const auto& base : record.bases)
        {
            const std::string baseRecord = ResolveCxxBaseRecordName(base);
            EnsureCxxMemberProjected(baseRecord.empty() || baseRecord == stableTypeName
                ? base.name : baseRecord, stableMemberName);
        }
        const bool oldRegistering = registeringCxxProjection_;
        const bool oldMembers = registeringCxxProjectionMembers_;
        registeringCxxProjection_ = true;
        registeringCxxProjectionMembers_ = true;
        RegisterCxxClassMembers(record, file, stableMemberName);
        const auto layout = dataStructures.find(stableTypeName);
        if (layout != dataStructures.end() && layout->second.StructType != nullptr
            && !layout->second.StructType->isOpaque() && layout->second.StructType->isSized())
            RegisterCxxInheritedMembers(record);
        registeringCxxProjection_ = oldRegistering;
        registeringCxxProjectionMembers_ = oldMembers;
        projectedCxxMemberNames_[stableTypeName].insert(stableMemberName);
        return true;
}

void LLVMBackend::EnsureCxxConversionOperatorsProjected(const std::string& typeName)
{
        EnsureCxxRecordProjected(typeName, false);
        std::set<std::string> names;
        std::set<std::string> visited;
        auto collect = [&](auto&& self, const std::string& name) -> void {
            auto record = cxxRecordEntries_.find(name);
            if (record == cxxRecordEntries_.end() || !visited.insert(name).second) return;
            for (const auto& member : record->second.members)
                if (member.isConversion)
                {
                    names.insert(member.name);
                    TypeAndValue returnType;
                    if (MapCTypeToTypeAndValue(member.retType, returnType, /*cxxBoundary*/ true))
                    {
                        std::string spelling = returnType.IsPrimitive() && !returnType.Pointer
                            ? CanonicalPrimitiveTypeName(ResolveTypeAlias(returnType.TypeName))
                            : SpellType(*this, returnType);
                        names.insert(spelling.empty() ? member.name : "operator " + spelling);
                    }
                    else
                        names.insert(member.name);
                }
            for (const auto& base : record->second.bases)
            {
                const std::string baseName = ResolveCxxBaseRecordName(base);
                if (!baseName.empty()) self(self, baseName);
            }
        };
        collect(collect, typeName);
        for (const auto& name : names) EnsureCxxMemberProjected(typeName, name);
}

// Lay out the C++ record a field holds BY VALUE (directly or as a fixed array element) - the
// dependency edge pass 1b of RegisterCRecords orders an eager batch by.
void LLVMBackend::ProjectCxxByValueFieldRecord(const std::string& ctype)
{
        if (ValueFieldRecordKey(ctype).empty()) return;
        std::string element = ctype;
        if (const size_t bracket = element.find('['); bracket != std::string::npos)
            element.erase(bracket);
        TypeAndValue fieldType;
        if (MapCTypeToTypeAndValue(element, fieldType, true)
            && !fieldType.Pointer && !fieldType.ElemPointer
            && pendingCxxRecordProjections_.count(fieldType.TypeName) != 0)
            EnsureCxxRecordProjected(fieldType.TypeName, false, true);
}

void LLVMBackend::RegisterPendingSyntheticCRecord(const std::string& ctype)
{
        SyntheticCRecordBatch* const batch = syntheticCRecordBatch_;
        if (batch == nullptr) return;
        const std::string key = ValueFieldRecordKey(ctype);
        auto hit = key.empty() ? batch->index.end() : batch->index.find(key);
        if (hit == batch->index.end() || batch->registered[hit->second]) return;
        batch->registered[hit->second] = true;   // before recursing, so a cycle stops here
        std::vector<CRecordEntry> one{ (*batch->records)[hit->second] };
        for (const auto& field : one.front().fields)
        {
            RegisterPendingSyntheticCRecord(field.ctype);
            ProjectCxxByValueFieldRecord(field.ctype);
        }
        const bool oldRegistering = registeringCxxProjection_;
        const bool oldMembers = registeringCxxProjectionMembers_;
        registeringCxxProjection_ = true;
        registeringCxxProjectionMembers_ = true;
        RegisterCRecords(one, batch->file);
        registeringCxxProjection_ = oldRegistering;
        registeringCxxProjectionMembers_ = oldMembers;
}

bool LLVMBackend::EnsureCxxRecordProjected(const std::string& typeName, bool includeMembers,
                                           bool gateSpecialMembersOnly)
{
        auto pending = pendingCxxRecordProjections_.find(typeName);
        if (pending == pendingCxxRecordProjections_.end())
            return !includeMembers || RegisterCxxMembersForProjectedRecord(typeName);
        // The caller's name may live in state this projection grows; keep a private copy.
        const std::string stableTypeName = typeName;
        auto entry = cxxRecordEntries_.find(stableTypeName);
        if (entry == cxxRecordEntries_.end()) return false;
        if (!projectingCxxRecords_.insert(stableTypeName).second) return true;
        if (entry->second.specialMembersPending)
        {
            CompleteCxxRecordSpecialMembers(stableTypeName);
            entry = cxxRecordEntries_.find(stableTypeName);
        }
        CRecordEntry record = entry->second;
        const std::string file = pending->second;
        for (const auto& base : record.bases)
            EnsureCxxRecordProjected(base.name, includeMembers, gateSpecialMembersOnly);
        for (const auto& field : record.fields)
        {
            RegisterPendingSyntheticCRecord(field.ctype);
            ProjectCxxByValueFieldRecord(field.ctype);
        }
        if (includeMembers) EnsureCxxRecordLayoutsForMemberSignatures(record);
        const bool oldRegistering = registeringCxxProjection_;
        const bool oldMembers = registeringCxxProjectionMembers_;
        registeringCxxProjection_ = true;
        registeringCxxProjectionMembers_ = includeMembers;
        std::vector<CRecordEntry> one{ std::move(record) };
        RegisterCRecords(one, file);
        if (!includeMembers)
            RegisterCxxClassMembers(one.front(), file, "__metadata");
        cxxRecordEntries_[stableTypeName] = one.front();
        registeringCxxProjection_ = oldRegistering;
        registeringCxxProjectionMembers_ = oldMembers;
        projectingCxxRecords_.erase(stableTypeName);
        pendingCxxRecordProjections_.erase(stableTypeName);
        if (includeMembers) return true;
        pendingCxxMemberProjections_[stableTypeName] = file;

        // Install lifetime members during the initial projection, before codegen can retain them.
        // The gate path requests only dtor/copy/move; other callers keep the full ctor projection.
        if (gateSpecialMembersOnly)
            EnsureCxxGateSpecialMembersProjected(stableTypeName);
        else
        {
            EnsureCxxMemberProjected(stableTypeName, "__dtor");
            EnsureCxxMemberProjected(stableTypeName, "__ctor");
        }
        const auto layout = dataStructures.find(stableTypeName);
        if (cxxNontrivialRecords_.count(stableTypeName) != 0 && layout != dataStructures.end()
            && layout->second.StructType != nullptr && !layout->second.StructType->isOpaque()
            && layout->second.StructType->isSized())
            GetOrCreateCxxClassDestructor(stableTypeName);
        return true;
}

/*
 * R1 demand-only special members. A live group's header harvest leaves the implicit / defaulted
 * special members and inline-vtable virtuals of its records undefined (the transitive
 * instantiation behind them is most of chunk 0's cost, for records the program never
 * constructs), so their member facts - linkage name, local definition, refusal - are not final.
 * The first projection of such a record asks its group for a completion chunk: a marker per
 * record (plus each still-pending base, one chunk for the lot) that defines exactly what the
 * header harvest would have defined for it and re-harvests the members. Only those facts are
 * folded into the retained record; its layout, member list and spellings stay the header's.
 */
void LLVMBackend::CompleteCxxRecordSpecialMembers(const std::string& typeName)
{
        auto root = cxxRecordEntries_.find(typeName);
        if (root == cxxRecordEntries_.end() || !root->second.specialMembersPending) return;
        auto settle = [&](const std::string& name) {
            if (auto it = cxxRecordEntries_.find(name); it != cxxRecordEntries_.end())
                it->second.specialMembersPending = false;
        };
        auto owner = cxxSpecialMemberOwner_.find(typeName);
        if (owner == cxxSpecialMemberOwner_.end() || owner->second >= cxxImportGroups_.size())
        {
            settle(typeName);
            return;
        }
        const size_t ownerGroup = owner->second;
        /*
         * One chunk for the record and every still-pending record of the same group that its
         * projection reaches next - its bases and by-value fields - so a hierarchy completes in
         * one parse rather than one per class.
         */
        std::vector<std::string> batch;
        std::set<std::string> queued;
        auto collect = [&](auto&& self, const std::string& name) -> void {
            auto it = cxxRecordEntries_.find(name);
            if (it == cxxRecordEntries_.end() || !it->second.specialMembersPending
                || !queued.insert(name).second)
                return;
            auto recordOwner = cxxSpecialMemberOwner_.find(name);
            if (recordOwner == cxxSpecialMemberOwner_.end() || recordOwner->second != ownerGroup)
                return;
            batch.push_back(name);
            const CRecordEntry& record = it->second;
            for (const auto& base : record.bases) self(self, base.name);
            for (const auto& field : record.fields)
            {
                std::string element = field.ctype;
                if (const size_t bracket = element.find('['); bracket != std::string::npos)
                    element.erase(bracket);
                const std::string key = ValueFieldRecordKey(element);
                if (key.empty()) continue;
                if (auto found = cxxPendingSpecialBySpelling_.find(key);
                    found != cxxPendingSpecialBySpelling_.end())
                    self(self, found->second);
            }
        };
        collect(collect, typeName);
        llvm::TimeTraceScope scope("CxxCompleteSpecialMembersRequest", typeName);
        CxxRequestGroup group = MakeCxxClassRequestGroup(ownerGroup, typeName);
        CxxRequestGroupScope groupScope(*this, &group);
        // The extractor finds each record by its CFlat identity; the marker is a placeholder.
        std::vector<CxxRequestItem> items;
        std::string batchLabel = "SPECIAL_MEMBERS";
        for (const std::string& name : batch)
        {
            CxxRequestItem item;
            item.cflatName = name;
            item.cxxSpelling = "int";
            item.needDefinitions = true;
            item.explicitInstantiation = false;
            items.push_back(std::move(item));
            batchLabel += "|" + name;
        }
        const std::string requestSource = BuildCxxRequestIncludes(group)
            + BuildCxxRequestMarkers(items, /*instantiateAll*/ false) + "// " + batchLabel + "\n";
        const std::string requestKey = CxxTypeRequestCacheKey(group, batchLabel, requestSource,
            BuildCxxRequestClangArgs(group), /*emitDefinitions*/ true);
        CFileSigCacheEntry completed;
        std::string error;
        if (!TryLoadCxxTypeRequestCache(group, requestKey, /*emitDefinitions*/ true, completed,
                                        error, /*allowDisk*/ true))
        {
            cflat_cinterop::ExtractResult raw;
            error.clear();
            const bool oldCompleting = completingCxxSpecialMembers_;
            completingCxxSpecialMembers_ = true;
            const bool ran = RunCxxTypeRequests(group, items, "// " + batchLabel + "\n",
                                                /*emitDefinitions*/ true, raw, error);
            completingCxxSpecialMembers_ = oldCompleting;
            if (!ran || raw.records.empty())
            {
                // Incomplete facts would bind silently wrong members; stop at the first use.
                LogErrorMessage("could not complete the special members of C++ record '{}' for its "
                                "first use: {}",
                                { typeName, !error.empty() ? error : raw.firstError });
                for (const std::string& name : batch) settle(name);
                return;
            }
            completed.longDoubleWidth = raw.longDoubleWidth;
            completed.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
            completed.targetTriple = raw.targetTriple;
            std::vector<CRecordEntry> mapped;
            MapRawRecords(raw, mapped);
            for (auto& record : mapped)
                if (queued.count(record.name) != 0
                    && std::find(batch.begin(), batch.end(), record.name) != batch.end())
                    completed.records.push_back(std::move(record));
            completed.cxxBitcode = raw.bitcode;
            if (raw.demandRecorded)
            {
                completed.cxxDemandGroupKey = CxxIncrementalGroupKey(group);
                completed.cxxDemandReplayChunk = raw.demandReplayChunk;
            }
            StoreCxxTypeRequestCache(group, requestKey, /*emitDefinitions*/ true,
                                     CFileSigCacheEntry(completed), /*allowDisk*/ true, nullptr);
        }
        if (!completed.cxxBitcode.empty() && symbolSink_ == nullptr)
            AdoptCxxCompanionBitcode(completed.cxxBitcode);
        for (const std::string& name : batch)
        {
            auto retained = cxxRecordEntries_.find(name);
            if (retained == cxxRecordEntries_.end()) continue;
            const CRecordEntry* fresh = nullptr;
            for (const auto& candidate : completed.records)
                if (candidate.name == name) { fresh = &candidate; break; }
            CRecordEntry merged = retained->second;
            merged.specialMembersPending = false;
            if (fresh == nullptr)
                LogErrorMessage("could not complete the special members of C++ record '{}' for its "
                                "first use: the completion request returned no record", { name });
            if (fresh != nullptr)
                for (auto& member : merged.members)
                {
                    auto match = std::find_if(fresh->members.begin(), fresh->members.end(),
                        [&](const auto& candidate) {
                            return candidate.kind == member.kind && candidate.name == member.name
                                && candidate.paramTypes == member.paramTypes
                                && candidate.isConst == member.isConst
                                && candidate.isVolatile == member.isVolatile
                                && candidate.refQualifier == member.refQualifier
                                && candidate.isTemplateSpecialization
                                    == member.isTemplateSpecialization
                                && candidate.isImplicit == member.isImplicit;
                        });
                    if (match == fresh->members.end()) continue;
                    member.linkageName = match->linkageName;
                    member.needsLocalDefinition = match->needsLocalDefinition;
                    member.definitionAssumed = match->definitionAssumed;
                    member.bindRefusal = match->bindRefusal;
                    member.refusalCause = match->refusalCause;
                    member.abi = match->abi;
                }
            StoreCxxRecordEntry(name, merged);
        }
}

void LLVMBackend::EnsureCxxGateSpecialMembersProjected(const std::string& typeName)
{
        EnsureCxxMemberProjected(typeName, "__dtor");
        EnsureCxxMemberProjected(typeName, "__copy_move_ctor");
}

void LLVMBackend::RegisterCRecords(std::vector<CRecordEntry>& records, const std::string& fileForLsp)
{
        if (records.empty()) return;
        for (CRecordEntry& r : records)
            if (r.isCxx)
            {
                r.name = CanonicalGeneratedCxxIdentity(r.name);
                r.qualifiedName = CanonicalGeneratedCxxIdentity(r.qualifiedName);
                // Base names must match the canonical record names (MSVC STL members live in bases).
                for (auto& b : r.bases) b.name = CanonicalGeneratedCxxIdentity(b.name);
                for (auto& b : r.virtualBases) b.name = CanonicalGeneratedCxxIdentity(b.name);
            }

        if (!registeringCxxProjection_
            && std::any_of(records.begin(), records.end(), [](const CRecordEntry& r) { return r.isCxx; }))
        {
            // Pass 1 of the eager path: specialization identity spellings for the whole batch
            // first, so a later record's reverse spelling can never shadow one.
            for (const CRecordEntry& r : records)
                if (r.isCxx && !r.canonicalCtype.empty() && r.name.find('$') != std::string::npos)
                {
                    cxxForeignTypeSpellings_.emplace(SqueezeCxxSpelling(r.canonicalCtype), r.name);
                    if (const std::string bare = ValueFieldRecordKey(r.canonicalCtype); !bare.empty())
                        cxxForeignTypeSpellings_.emplace(bare, r.name);
                }
            std::vector<CRecordEntry> cRecords;
            cRecords.reserve(records.size());
            for (const CRecordEntry& r : records)
            {
                if (r.isCxx)
                {
                    // A still-pending shell stands for a record the eager path had already laid
                    // out; a later batch's copy (a request TU) only adds its spellings.
                    if (pendingCxxRecordProjections_.count(r.name) != 0)
                    {
                        RememberCxxRecordSpellings(r);
                        continue;
                    }
                    auto existing = dataStructures.find(r.name);
                    if (existing == dataStructures.end() || existing->second.StructType == nullptr
                        || existing->second.StructType->isOpaque())
                        RegisterCxxRecordShell(r, fileForLsp);
                }
                else cRecords.push_back(r);
            }
            if (cRecords.empty()) return;
            // A C record (a C++ class's synthetic anonymous member) can hold a C++ record by
            // value; the eager path laid that out first in the same batch, so project it now.
            // That C++ record's own synthetic members sit in this batch too: they register on
            // demand (RegisterPendingSyntheticCRecord), the rest after the projections.
            SyntheticCRecordBatch batch;
            batch.records = &cRecords;
            batch.registered.assign(cRecords.size(), false);
            batch.file = fileForLsp;
            for (size_t i = 0; i < cRecords.size(); ++i)
            {
                std::string spelling = cRecords[i].name;
                for (size_t pos = 0; (pos = spelling.find('.', pos)) != std::string::npos; pos += 2)
                    spelling.replace(pos, 1, "::");
                batch.index.emplace(SqueezeCxxSpelling(spelling), i);
                batch.index.emplace(cRecords[i].name, i);
            }
            SyntheticCRecordBatch* const oldBatch = syntheticCRecordBatch_;
            syntheticCRecordBatch_ = &batch;
            for (const CRecordEntry& r : cRecords)
                for (const auto& field : r.fields) ProjectCxxByValueFieldRecord(field.ctype);
            syntheticCRecordBatch_ = oldBatch;
            std::vector<CRecordEntry> remaining;
            for (size_t i = 0; i < cRecords.size(); ++i)
                if (!batch.registered[i]) remaining.push_back(std::move(cRecords[i]));
            if (remaining.empty()) return;
            const bool oldRegistering = registeringCxxProjection_;
            const bool oldMembers = registeringCxxProjectionMembers_;
            registeringCxxProjection_ = true;
            registeringCxxProjectionMembers_ = true;
            RegisterCRecords(remaining, fileForLsp);
            registeringCxxProjection_ = oldRegistering;
            registeringCxxProjectionMembers_ = oldMembers;
            return;
        }

        // Pass 1: opaque shells. CFlat-defined types win; anonymous records are skipped
        // (clang inlines their fields at the JSON layer).
        std::vector<CRecordEntry*> ours;
        ours.reserve(records.size());
        for (auto& r : records)
        {
            if (r.name.empty()) continue;
            // The namespace a C++ import declared a class in, so a dotted name under it may be
            // requested as a specialization later (see cxxForeignNamespaces_).
            if (r.isCxx)
            {
                NoteCxxForeignNamespace(r.name);
                // A record registered under a foreign IDENTITY (a class-template specialization)
                // is reachable from a field's C++ spelling only through this map.
                if (!r.canonicalCtype.empty() && r.name.find('$') != std::string::npos)
                {
                    cxxForeignTypeSpellings_.emplace(SqueezeCxxSpelling(r.canonicalCtype), r.name);
                    // The lookup strips an elaborated `class `/`struct ` keyword first, so key the
                    // stripped spelling too rather than assume which form clang produced.
                    if (const std::string bare = ValueFieldRecordKey(r.canonicalCtype); !bare.empty())
                        cxxForeignTypeSpellings_.emplace(bare, r.name);
                }
                if (activeCxxRequestGroup_ != nullptr)
                    cxxTypeOwnerGroup_.emplace(r.name, activeCxxRequestGroup_->primary);
                for (size_t pos = 0; (pos = r.name.find('.', pos)) != std::string::npos; ++pos)
                    RegisterNamespace(r.name.substr(0, pos));
            }
            auto existing = dataStructures.find(r.name);
            if (existing != dataStructures.end())
            {
                // The forward pass may create an opaque shell for a requested C++ template
                // specialization. Keep it in this pass so the extracted layout and members fill it.
                if (!r.isCxx || existing->second.StructType == nullptr
                    || !existing->second.StructType->isOpaque())
                    continue;
                ours.push_back(&r);
                continue;
            }
            // Create the opaque shell so later fields/records in the same batch can refer to it.
            CreateStructType(r.name, /*typeAndValues*/{});
            ours.push_back(&r);
        }

        /*
         * Pass 1b: lay a record out AFTER every record it holds BY VALUE. Pass 1 leaves an opaque
         * shell, and a field whose type is still a shell has no sized LLVM type - the layout below
         * would embed it as opaque bytes and its members would never resolve (a nested member class
         * is emitted after its owner by clang's traversal, so this is the common case). Pointer
         * fields create no edge, so a nested class pointing back at its owner cannot cycle; a cycle
         * that does exist keeps the original relative order.
         */
        {
            std::unordered_map<std::string, size_t> byKey;
            for (size_t i = 0; i < ours.size(); ++i)
            {
                const CRecordEntry& r = *ours[i];
                if (!r.name.empty()) byKey.emplace(SqueezeCxxSpelling(r.name), i);
                if (!r.canonicalCtype.empty())
                    if (const std::string key = ValueFieldRecordKey(r.canonicalCtype); !key.empty())
                        byKey.emplace(key, i);
                std::string cxxSpelling = r.name;
                for (size_t p = 0; (p = cxxSpelling.find('.', p)) != std::string::npos; p += 2)
                    cxxSpelling.replace(p, 1, "::");
                if (cxxSpelling != r.name) byKey.emplace(SqueezeCxxSpelling(cxxSpelling), i);
            }
            std::vector<CRecordEntry*> ordered;
            ordered.reserve(ours.size());
            std::vector<char> state(ours.size(), 0);      // 0 unvisited, 1 on stack, 2 emitted
            auto visit = [&](auto&& self, size_t i) -> void {
                if (state[i] != 0) return;
                state[i] = 1;
                for (const auto& f : ours[i]->fields)
                {
                    const std::string key = ValueFieldRecordKey(f.ctype);
                    if (key.empty()) continue;
                    auto it = byKey.find(key);
                    if (it == byKey.end() || it->second == i || state[it->second] != 0) continue;
                    self(self, it->second);
                }
                state[i] = 2;
                ordered.push_back(ours[i]);
            };
            for (size_t i = 0; i < ours.size(); ++i) visit(visit, i);
            ours.swap(ordered);
        }

        // Pass 2: bodies. On unmappable fields leave the opaque shell in place so a later
        // reference surfaces a clear error rather than crashing on a partial struct.
        // Member registration is DEFERRED to pass 3: a member's ABI recipe needs the layout of
        // every class it takes or returns by value, and that class may be laid out later in this
        // same batch (`padded_string::operator padded_string_view()` precedes the view's body).
        std::vector<std::pair<CRecordEntry*, bool>> deferredMembers;   // (record, laid out)
        for (CRecordEntry* rp : ours)
        {
            CRecordEntry& r = *rp;
            // M6 - a class whose layout cflat could NOT flatten (virtual inheritance, or a
            // bitfield / anonymous member inside a hierarchy) keeps the opaque shell from pass 1:
            // a pointer to it stays a legal handle, while every by-value or member use is refused
            // at the use site by RejectUnsupportedCxxLayout. A polymorphic class WITHOUT those
            // problems is laid out for real below - the vptr slot and each base subobject are
            // already in r.fields at clang's own offsets.
            if (r.isCxx && !r.layoutRefusal.empty())
            {
                cxxRecords_.insert(r.name);
                deferredMembers.emplace_back(&r, false);
                if (auto* s = GetSymbolSink())
                    s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line,
                                r.col < 0 ? 0 : r.col, "class " + SpellType(*this,
                                    TypeAndValue{ .TypeName = r.name }));
                continue;
            }
            std::vector<DeclTypeAndValue> fields;
            bool anyOpaqueBlob = false;   // candidate for BindLazyCxxStdField
            bool ok = true;
            std::string badFieldName;
            std::string badFieldType;
            auto generatedIt = generatedCxxRecords_.find(r.name);
            const bool generated = r.isCxx && generatedIt != generatedCxxRecords_.end();
            if (generated)
            {
                fields = generatedIt->second.fields;
                auto* literal = generatedIt->second.literalType;
                const llvm::DataLayout& dl = module->getDataLayout();
                if (literal == nullptr || literal->isOpaque() || !literal->isSized())
                {
                    r.layoutRefusal = "generated CFlat field layout is not sized";
                    ok = false;
                }
                else
                {
                    const auto clangFields = r.fields;
                    auto marker = std::find_if(clangFields.begin(), clangFields.end(),
                        [](const CRecordFieldEntry& f) { return f.name == "__cflat_fields"; });
                    if (marker == clangFields.end())
                    {
                        r.layoutRefusal = "generated C++ struct has no __cflat_fields descriptor";
                        ok = false;
                    }
                    const uint64_t blockOffset = marker == clangFields.end()
                        ? 0 : marker->offsetBytes;
                    auto mapClangField = [&](const CRecordFieldEntry& f,
                                             std::vector<DeclTypeAndValue>& out) -> bool {
                        DeclTypeAndValue d;
                        TypeAndValue tv;
                        std::vector<uint64_t> arrDims;
                        std::string fieldSpelling = f.ctype;
                        if (f.isZeroSize)
                            if (size_t incomplete = fieldSpelling.find("[]"); incomplete != std::string::npos)
                                fieldSpelling.replace(incomplete, 2, "[0]");
                        std::string elemSpelling = StripFixedArrayDims(fieldSpelling, arrDims);
                        // Keep a typed element view for a flexible or zero-length member; its
                        // field address comes from the clang offset map, with no record storage.
                        if (f.isZeroSize && !arrDims.empty() && arrDims.front() == 0)
                            arrDims.front() = 1;
                        if (!MapCTypeToTypeAndValue(elemSpelling, tv, r.isCxx))
                        {
                            if (r.isCxx && !f.isBitfield && arrDims.empty()
                                && MakeOpaqueFieldBlob(f, d))
                            {
                                d.VariableName = f.name;
                                d.IsAnonymousMember = f.isAnonymousMember;
                                out.push_back(std::move(d));
                                return true;
                            }
                            r.layoutRefusal = std::format(
                                "generated C++ field '{}' has unsupported type '{}'", f.name, f.ctype);
                            return false;
                        }
                        const bool incompleteArray = f.ctype.find("[]") != std::string::npos;
                        const bool zeroLengthArray = f.ctype.find("[0]") != std::string::npos;
                        if (!arrDims.empty())
                        {
                            tv.ConstArraySize = (incompleteArray || zeroLengthArray) ? 1 : arrDims[0];
                            tv.ConstInnerDimensions.assign(arrDims.begin() + 1, arrDims.end());
                        }
                        tv.IsFlexibleArrayMember = f.isZeroSize && !arrDims.empty();
                        tv.IsZeroLengthArrayMember = tv.IsFlexibleArrayMember && zeroLengthArray;
                        tv.IsCxxMutableField = f.isMutable;
                        MarkCxxReferenceField(f.ctype, tv, r.isCxx);
                        // C++ field pointers have a known one-level shape even though the shared
                        // type mapper leaves their ordinary value depth unrecorded.
                        if (r.isCxx && tv.Pointer && !tv.ElemPointer
                            && !tv.IsFunctionPointer && !tv.IsCxxRef())
                            tv.PointerDepth = 1;
                        MarkCxxDeclaredConstField(tv, r.isCxx && f.isConst && !f.isMutable);
                        static_cast<TypeAndValue&>(d) = tv;
                        d.VariableName = f.name;
                        d.IsAnonymousMember = f.isAnonymousMember;
                        if (f.isBitfield)
                        {
                            d.IsBitfield = true;
                            d.BitWidth = f.bitWidth;
                        }
                        out.push_back(std::move(d));
                        return true;
                    };

                    std::vector<CRecordFieldEntry> prefixRecords;
                    prefixRecords.reserve(clangFields.size());
                    // The C++ view of the CFlat bytes (anonymous union at the block offset) is not
                    // base storage: its members sit at the block and CFlat fields are laid out below.
                    for (const auto& f : clangFields)
                        if (f.name != "__cflat_fields"
                            && !((f.isAnonymousMember || f.isPromoted) && f.offsetBytes >= blockOffset))
                            prefixRecords.push_back(f);
                    std::vector<DeclTypeAndValue> prefixFields;
                    for (const auto& f : prefixRecords)
                        if (!mapClangField(f, prefixFields)) ok = false;
                    uint64_t prefixEnd = 0;
                    for (size_t i = 0; i < prefixRecords.size() && i < prefixFields.size(); ++i)
                    {
                        auto* type = GetType(prefixFields[i]);
                        if (type != nullptr && type->isSized())
                            prefixEnd = std::max<uint64_t>(prefixEnd,
                                prefixRecords[i].offsetBytes + dl.getTypeAllocSize(type));
                    }
                    CRecordEntry prefix = r;
                    prefix.fields = prefixRecords;
                    prefix.sizeBytes = 0;   // the CFlat fields follow: no tail padding here
                    InsertCxxLayoutPadding(prefix, prefixFields);

                    std::vector<DeclTypeAndValue> combined;
                    combined.reserve(prefixFields.size() + fields.size() + 1);
                    combined.insert(combined.end(), prefixFields.begin(), prefixFields.end());
                    std::vector<llvm::Type*> prefixTypes;
                    for (const auto& f : prefixFields)
                    {
                        auto* type = GetType(f);
                        if (type == nullptr || !type->isSized())
                        {
                            prefixTypes.clear();
                            break;
                        }
                        prefixTypes.push_back(type);
                    }
                    if (!prefixRecords.empty() && prefixTypes.empty()) ok = false;
                    if (prefixEnd > blockOffset)
                    {
                        r.layoutRefusal = std::format(
                            "generated C++ base storage ends at byte {}, past __cflat_fields at byte {}",
                            prefixEnd, blockOffset);
                        ok = false;
                    }
                    if (prefixEnd < blockOffset)
                    {
                        DeclTypeAndValue pad;
                        pad.TypeName = "u8";
                        pad.ConstArraySize = blockOffset - prefixEnd;
                        combined.push_back(std::move(pad));
                    }
                    combined.insert(combined.end(), fields.begin(), fields.end());
                    fields.swap(combined);

                    r.fields = prefixRecords;
                    const auto* layout = dl.getStructLayout(literal);
                    for (size_t fi = 0; fi < generatedIt->second.fields.size(); ++fi)
                    {
                        const auto& field = generatedIt->second.fields[fi];
                        if (field.IsPadding || field.VariableName.empty()) continue;
                        CRecordFieldEntry descriptor;
                        descriptor.name = field.VariableName;
                        descriptor.ctype = field.TypeName;
                        descriptor.access = cflat_cinterop::AccessPublic;
                        descriptor.isConst = field.IsConstObject();
                        descriptor.isMutable = field.IsCxxMutableField;
                        descriptor.offsetBytes = blockOffset + layout->getElementOffset((unsigned)fi);
                        auto* fieldType = GetType(field);
                        if (fieldType == nullptr || !fieldType->isSized())
                        {
                            r.layoutRefusal = std::format(
                                "generated field '{}' has no sized LLVM type", field.VariableName);
                            ok = false;
                            break;
                        }
                        descriptor.sizeBytes = dl.getTypeAllocSize(fieldType);
                        descriptor.alignBytes = dl.getABITypeAlign(fieldType).value();
                        r.fields.push_back(std::move(descriptor));
                    }
                }
            }
            else
            {
                fields.reserve(r.fields.size());
                for (const auto& f : r.fields)
                {
                    TypeAndValue tv;
                // Strip fixed-array dims before mapping: the shared mapper decays `[N]` to a
                // pointer (right for params, wrong for fields), so peel them here first.
                std::vector<uint64_t> arrDims;
                std::string fieldSpelling = f.ctype;
                // Outer brackets only: `default_delete<int[]>` is a class, not a flexible array.
                if (f.isZeroSize)
                    if (size_t incomplete = fieldSpelling.rfind("[]");
                        incomplete != std::string::npos && fieldSpelling.ends_with("]"))
                        fieldSpelling.replace(incomplete, 2, "[0]");
                std::string elemSpelling = StripFixedArrayDims(fieldSpelling, arrDims);
                // Keep a typed element view for a flexible or zero-length member; its field
                // address comes from the clang offset map, with no record storage.
                if (f.isZeroSize && !arrDims.empty() && arrDims.front() == 0)
                    arrDims.front() = 1;
                if (r.isCxx && f.ctype.find("::*") != std::string::npos)
                {
                    r.layoutRefusal = std::format(
                        "field '{}' of '{}' is a pointer to member; member-pointer fields are not supported (an ordinary pointer is not equivalent)",
                        f.name, r.name);
                    badFieldName = f.name;
                    badFieldType = f.ctype;
                    ok = false;
                    break;
                }
                if (!MapCTypeToTypeAndValue(elemSpelling, tv, r.isCxx))
                {
                    // A C++ field whose TYPE has no CFlat mapping yet (a class-template
                    // specialization such as `ImVector<T>`) still has a size and alignment
                    // from Clang: embed it as an opaque blob so the record and its other
                    // fields stay usable. Typed access to the blob goes through the request
                    // layer.
                    DeclTypeAndValue blob;
                    if (r.isCxx && !f.isBitfield && arrDims.empty() && MakeOpaqueFieldBlob(f, blob))
                    {
                        if (verbose) std::cout << std::format("[verbose]   C++ struct '{}': field '{}' of type '{}' embedded as {} opaque bytes\n",
                            r.name, f.name, f.ctype, f.sizeBytes);
                        fields.push_back(std::move(blob));
                        anyOpaqueBlob = true;
                        continue;
                    }
                    badFieldName = f.name;
                    badFieldType = f.ctype;
                    if (verbose) std::cout << std::format("[verbose]   skipping C {} '{}': unsupported field '{}' of type '{}'\n",
                        r.isUnion ? "union" : "struct", r.name, f.name, f.ctype);
                    ok = false;
                    break;
                }
                tv.IsCxxMutableField = f.isMutable;
                MarkCxxReferenceField(f.ctype, tv, r.isCxx);
                // Keep a pointer field's known one-level shape when taking its address.
                if (r.isCxx && tv.Pointer && !tv.ElemPointer
                    && !tv.IsFunctionPointer && !tv.IsCxxRef())
                    tv.PointerDepth = 1;
                // `const char* p`: the pointee stays const for C++ overload choice; a class-typed
                // const field makes its object read-only to the C++ receiver checks.
                if (r.isCxx && tv.Pointer && !tv.IsFunctionPointer)
                    tv.ConstMask |= CxxSpellingPointeeConstMask(f.ctype, tv.ValuePointerDepth());
                if (r.isCxx)
                    MarkCxxDeclaredConstField(tv, f.isConst && !f.isMutable);
                // A C field keeps its spelled pointee const (`const char* p`, a `PCSTR`; ctype is
                // canonical), and its own top-level const (an array's is its element's). Only a
                // downward-closed mask is a CFlat spelling (R11).
                if (!r.isCxx && !tv.IsFunctionPointer && !tv.IsArrayView)
                {
                    const int depth = tv.ValuePointerDepth();
                    const uint32_t mask = CSpellingPointeeConstMask(elemSpelling, depth)
                        | (f.isConst && !f.isMutable
                            ? LLVMBackend::TypeAndValue::ConstBitAt(depth) : 0u);
                    if (mask != 0 && (mask & (mask + 1u)) == 0) tv.ConstMask = mask;
                }
                if (r.isCxx && CxxSpellingHasVolatileTarget(f.ctype))
                    tv.IsCxxPointeeVolatile = true;
                const bool incompleteArray = f.ctype.find("[]") != std::string::npos;
                const bool zeroLengthArray = f.ctype.find("[0]") != std::string::npos;
                if (!arrDims.empty())
                {
                    tv.ConstArraySize = (incompleteArray || zeroLengthArray) ? 1 : arrDims[0];
                    tv.ConstInnerDimensions.assign(arrDims.begin() + 1, arrDims.end());
                }
                tv.IsFlexibleArrayMember = f.isZeroSize && !arrDims.empty();
                tv.IsZeroLengthArrayMember = tv.IsFlexibleArrayMember && zeroLengthArray;
                // A C fn-ptr field maps to a THIN function<T> ("__c_fn_ptr") - a bare,
                // pointer-sized C function pointer, same size as the void* it replaces, so the
                // struct layout is unchanged. Keeping the real signature makes MIDL COM vtable
                // slots (e.g. ID3D12DeviceVtbl) callable as `obj->lpVtbl->Method(obj, ...)`
                // through the existing thin-call path, instead of an opaque void* the user
                // must reinterpret by hand.

                // A pointer field to a KNOWN aggregate keeps its pointee type instead of decaying
                // to opaque void* (the shared mapper's default for struct pointers). This is what
                // makes a COM object's `lpVtbl` typed as `<Interface>Vtbl*` so the member-access
                // chain resolves; unknown/opaque pointees still fall back to void*.
                if (!tv.IsFunctionPointer && tv.Pointer && tv.TypeName == "void")
                {
                    int ptrLevels = 0;
                    std::string tag = AggregatePointeeTag(elemSpelling, ptrLevels);
                    // Clang spells a C++ class with no struct/union keyword, so
                    // AggregatePointeeTag cannot answer for one - CxxRecordPointeeTag can, and it
                    // covers the reference spelling (`Foo&`, which binds as a pointer) too.
                    if (tag.empty() && r.isCxx)
                        tag = CxxRecordPointeeTag(elemSpelling, ptrLevels);
                    if (!tag.empty() && ptrLevels <= 2 && dataStructures.find(tag) != dataStructures.end())
                        tv.TypeName = tag;   // keep Pointer / ElemPointer as the mapper set them
                }
                DeclTypeAndValue d;
                static_cast<TypeAndValue&>(d) = tv;
                d.VariableName = f.name;
                d.IsAnonymousMember = f.isAnonymousMember;
                if (f.isBitfield)
                {
                    d.IsBitfield = true;
                    d.BitWidth = f.bitWidth;
                }
                    fields.push_back(std::move(d));
                }
            }
            if (fields.empty() && r.isCxx && r.fields.empty())
            {
                // C++ empty classes still occupy sizeof(T) bytes (one byte for the usual
                // non-packed case). Keep a sized opaque shell so an empty foreign value can be
                // passed through a generated wrapper, while exposing no synthetic field to CFlat.
                CRecordFieldEntry storage;
                storage.sizeBytes = r.sizeBytes;
                storage.alignBytes = r.alignBytes;
                DeclTypeAndValue blob;
                if (!MakeOpaqueFieldBlob(storage, blob))
                {
                    cxxRecords_.insert(r.name);
                    deferredMembers.emplace_back(&r, false);
                    continue;
                }
                fields.push_back(std::move(blob));
            }
            if (!ok || fields.empty())
            {
                if (r.isCxx && r.layoutRefusal.empty())
                    r.layoutRefusal = IsLongDoubleSpelling(badFieldType)
                        && !IsCInteropLongDoubleSupported()
                    ? std::format("field '{}' of '{}': {}", badFieldName, r.name,
                                  CInteropLongDoubleRefusal())
                    : std::format(
                        "field '{}' of '{}' has unsupported C++ type '{}'",
                        badFieldName, r.name, badFieldType);
                if (r.isCxx && !r.layoutRefusal.empty())
                {
                    cxxRecords_.insert(r.name);
                    deferredMembers.emplace_back(&r, false);
                }
                continue;
            }
            // An opaque-shell by-value aggregate field has no size; CreateStructType would
            // assert "Cannot getTypeInfo() on unsized". Abandon and leave the shell.
            for (size_t fi = 0; fi < fields.size(); ++fi)
            {
                auto& d = fields[fi];
                if (d.Pointer) continue;            // pointers are always sized
                auto* ft = GetType(d);
                if (ft && !ft->isSized())
                {
                    // Same blob fallback: the field's record was itself refused or dropped,
                    // but Clang still told us how big the field is.
                    DeclTypeAndValue blob;
                    if (r.isCxx && fi < r.fields.size() && !r.fields[fi].isBitfield
                        && MakeOpaqueFieldBlob(r.fields[fi], blob))
                    {
                        if (verbose) std::cout << std::format("[verbose]   C++ struct '{}': field '{}' has no layout for type '{}' (its record was refused or never registered); embedded as {} opaque bytes\n",
                            r.name, d.VariableName, d.TypeName, r.fields[fi].sizeBytes);
                        d = std::move(blob);
                        anyOpaqueBlob = true;
                        continue;
                    }
                    if (verbose) std::cout << std::format("[verbose]   skipping C {} '{}': field '{}' has incomplete (unsized) type '{}'\n",
                        r.isUnion ? "union" : "struct", r.name, d.VariableName, d.TypeName);
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            // Imported records use the resolved target ABI; native CFlat structs retain the
            // existing MSVC-style packing. The pass produces synthetic slots and BitfieldInfo.
            std::vector<BitfieldInfo> packedBitfields;
            bool anyBitfields = false;
            for (const auto& tv : fields) { if (tv.IsBitfield) { anyBitfields = true; break; } }
            // A synthetic anonymous member of a C++ record is not itself isCxx, but carries
            // clang's size so a [[no_unique_address]] member inside it takes this layout too.
            const bool clangZeroSizeLayout = (r.isCxx || r.sizeBytes != 0) && !generated
                && !r.isUnion && std::any_of(r.fields.begin(), r.fields.end(),
                    [](const CRecordFieldEntry& f) { return f.isZeroSize; });
            const bool hasZeroSizeFields = clangZeroSizeLayout && !anyBitfields;
            const bool hasPromotedFields = r.isCxx && !generated
                && std::any_of(r.fields.begin(), r.fields.end(),
                    [](const CRecordFieldEntry& f) { return f.isPromoted; });
            const bool hasSemanticOnlyFields = hasZeroSizeFields || (hasPromotedFields && !r.isUnion);
            // With bitfields the zero-size members are lifted out before packing and appended
            // after the physical slots, so BitfieldInfo storage indices stay element indices.
            const bool zeroSizeBitfields = clangZeroSizeLayout && anyBitfields;
            std::vector<DeclTypeAndValue> liftedZeroSizeFields;
            std::vector<uint64_t> liftedZeroSizeOffsets;
            if (zeroSizeBitfields)
            {
                std::vector<DeclTypeAndValue> kept;
                for (size_t i = 0; i < fields.size(); ++i)
                {
                    if (i < r.fields.size() && r.fields[i].isZeroSize)
                    {
                        liftedZeroSizeFields.push_back(fields[i]);
                        liftedZeroSizeOffsets.push_back(r.fields[i].offsetBytes);
                    }
                    else
                        kept.push_back(fields[i]);
                }
                fields.swap(kept);
            }
            std::vector<DeclTypeAndValue> cxxSemanticFields;
            std::vector<uint64_t> cxxFieldOffsets;
            std::vector<unsigned> cxxFieldElements;
            if (hasSemanticOnlyFields)
            {
                cxxSemanticFields = fields;
                CRecordEntry physical = r;
                physical.fields.clear();
                fields.clear();
                for (size_t i = 0; i < r.fields.size() && i < cxxSemanticFields.size(); ++i)
                {
                    cxxFieldOffsets.push_back(r.fields[i].offsetBytes);
                    if (r.fields[i].isZeroSize || r.fields[i].isPromoted)
                    {
                        cxxFieldElements.push_back(std::numeric_limits<unsigned>::max());
                        continue;
                    }
                    physical.fields.push_back(r.fields[i]);
                    fields.push_back(cxxSemanticFields[i]);
                }
                if (fields.empty() && r.sizeBytes != 0)
                {
                    DeclTypeAndValue storage;
                    storage.TypeName = "u8";
                    storage.ConstArraySize = r.sizeBytes;
                    fields.push_back(std::move(storage));
                }
                else
                    InsertCxxLayoutPadding(physical, fields);
                unsigned physicalIndex = 0;
                cxxFieldElements.clear();
                for (size_t semanticIndex = 0; semanticIndex < cxxSemanticFields.size(); ++semanticIndex)
                {
                    if (semanticIndex < r.fields.size()
                        && (r.fields[semanticIndex].isZeroSize || r.fields[semanticIndex].isPromoted))
                    {
                        cxxFieldElements.push_back(std::numeric_limits<unsigned>::max());
                        continue;
                    }
                    while (physicalIndex < fields.size() && fields[physicalIndex].VariableName.empty())
                        ++physicalIndex;
                    cxxFieldElements.push_back(physicalIndex++);
                }
            }
            // Save semantic fields before PackBitfields replaces them with __bfN slots;
            // used only for LSP symbol registration below.
            std::vector<DeclTypeAndValue> prePackFields;
            if (anyBitfields)
            {
                prePackFields = fields;
                fields = PackBitfields(fields, packedBitfields, !targetWindows_, r.isUnion);
            }
            bool cBitfieldOffsetLayout = false;
            // C records and Itanium C++ records use Clang's byte-granular bitfield
            // offsets. Keep the Windows C++ path on its established MS-ABI packing.
            if (!r.isUnion && (!r.isCxx || (!targetWindows_ && !r.isPacked))
                && (anyBitfields || zeroSizeBitfields))
                cBitfieldOffsetLayout = InsertCBitfieldLayoutPadding(r, fields, packedBitfields);
            // Field offsets are clang's: pad wherever clang placed a field past natural packing
            // (over-aligned members, empty-member slots); a naturally laid-out C record gets none.
            if (!generated && !r.isUnion && !anyBitfields && !hasSemanticOnlyFields)
                InsertCxxLayoutPadding(r, fields);
            // A C record over-aligned past its members takes clang's alignment like an alignas C++
            // record: user alignment sizes it, physical alignment sets its ABI alignment.
            bool cOverAligned = false;
            if (!r.isCxx && !r.isPacked && !anyBitfields && r.alignBytes > 1)
            {
                uint64_t cMaxFieldAlign = 1;
                for (const auto& tv : fields)
                    if (llvm::Type* ft = GetType(tv); ft != nullptr && ft->isSized())
                        cMaxFieldAlign = std::max<uint64_t>(cMaxFieldAlign,
                            module->getDataLayout().getABITypeAlign(ft).value());
                cOverAligned = r.alignBytes > cMaxFieldAlign;
            }
            if (r.isUnion)
            {
                const bool cUnionClangAlign = !r.isCxx && (r.isPacked || anyBitfields);
                CreateUnionType(r.name, fields, r.isCxx || cUnionClangAlign || cOverAligned ? r.alignBytes : 0,
                                anyBitfields ? &packedBitfields : nullptr, cUnionClangAlign);
                if (hasPromotedFields)
                {
                    auto it = dataStructures.find(r.name);
                    if (it != dataStructures.end())
                    {
                        it->second.CxxOffsetLayout = true;
                        it->second.CxxFieldOffsets.reserve(r.fields.size());
                        it->second.CxxFieldElements.assign(r.fields.size(),
                            std::numeric_limits<unsigned>::max());
                        for (const auto& field : r.fields)
                            it->second.CxxFieldOffsets.push_back(field.offsetBytes);
                    }
                }
            }
            else
            {
                CreateStructType(r.name, fields,
                    r.isCxx || r.isPacked || clangZeroSizeLayout || cBitfieldOffsetLayout || cOverAligned
                        ? r.alignBytes : 0,
                    anyBitfields ? &packedBitfields : nullptr,
                    r.isPacked,
                    (cBitfieldOffsetLayout && !r.isPacked) || cOverAligned ? r.alignBytes : 0);
                if (zeroSizeBitfields)
                {
                    auto it = dataStructures.find(r.name);
                    if (it != dataStructures.end() && it->second.StructType != nullptr)
                    {
                        auto& data = it->second;
                        const llvm::StructLayout* sl =
                            module->getDataLayout().getStructLayout(data.StructType);
                        data.CxxFieldOffsets.clear();
                        data.CxxFieldElements.clear();
                        for (unsigned i = 0; i < data.StructFields.size(); ++i)
                        {
                            data.CxxFieldOffsets.push_back(sl->getElementOffset(i));
                            data.CxxFieldElements.push_back(i);
                        }
                        for (size_t i = 0; i < liftedZeroSizeFields.size(); ++i)
                        {
                            data.StructFields.push_back(liftedZeroSizeFields[i]);
                            data.CxxFieldOffsets.push_back(liftedZeroSizeOffsets[i]);
                            data.CxxFieldElements.push_back(std::numeric_limits<unsigned>::max());
                        }
                        data.CxxOffsetLayout = true;
                    }
                }
                if (hasSemanticOnlyFields)
                {
                    auto it = dataStructures.find(r.name);
                    if (it != dataStructures.end())
                    {
                        it->second.StructFields = cxxSemanticFields;
                        it->second.CxxOffsetLayout = true;
                        it->second.CxxFieldOffsets = std::move(cxxFieldOffsets);
                        it->second.CxxFieldElements = std::move(cxxFieldElements);
                    }
                }
                if (r.isCxx && anyOpaqueBlob)
                    if (auto it = dataStructures.find(r.name); it != dataStructures.end())
                        cxxOpaqueFieldOwners_[it->second.StructType] = r.name;
            }
            if (r.isCxx)
            {
                cxxRecords_.insert(r.name);
                if (r.isTriviallyRelocatable) cxxTriviallyRelocatableRecords_.insert(r.name);
                const bool stdValueRecord = r.name.starts_with("std.pair$")
                                          || r.name.starts_with("std.optional$")
                                          || r.name.starts_with("std.tuple$");
                if (r.isTriviallyCopyable || (stdValueRecord && r.hasTrivialDtor))
                    cxxTriviallyCopyableRecords_.insert(r.name);
                // A class that is NOT trivially copyable owns its lifetime: every construction,
                // copy, move and destruction of it must route through the C++ special members
                // (M4b). A POLYMORPHIC class always lands here - its constructor is what writes
                // the vptr, so a CFlat bitwise store could never produce a valid object.
                else if (!r.isUnion)
                    cxxNontrivialRecords_.insert(r.name);
                // A layout cflat could not reproduce is a per-record refusal, not an import
                // failure: pointers to the record stay usable, every by-value or field use is
                // rejected at its own site with this reason (RejectUnsupportedCxxLayout).
                if (std::string mismatch = VerifyImportedRecordLayout(r); !mismatch.empty())
                {
                    r.layoutRefusal = std::move(mismatch);
                    if (generated)
                        LogErrorMessage("generated C++ struct '{}' has a layout disagreement: {}",
                                        { DisplayCxxClassName(r.name), r.layoutRefusal });
                    if (verbose) std::cout << std::format("[verbose]   C++ struct '{}': {}\n", r.name, r.layoutRefusal);
                }
                deferredMembers.emplace_back(&r, true);
            }
            else if (clangZeroSizeLayout)
            {
                // The enclosing C++ record's verifier refuses on this detail.
                if (std::string mismatch = VerifyImportedRecordLayout(r); !mismatch.empty())
                {
                    r.layoutRefusal = mismatch;
                    // Keep the detail only; the enclosing refusal supplies the prefix.
                    constexpr std::string_view prefix = "layout is not representable: ";
                    if (mismatch.starts_with(prefix)) mismatch.erase(0, prefix.size());
                    cxxAnonLayoutRefusals_[r.name] = std::move(mismatch);
                }
            }
            if (!r.isCxx && (anyBitfields || cOverAligned))
            {
                if (std::string mismatch = VerifyImportedRecordLayout(r); !mismatch.empty())
                    LogErrorMessage("C struct '{}' has a layout disagreement: {}",
                                    { r.name, mismatch });
            }
            if (auto* s = GetSymbolSink())
            {
                s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line, r.col < 0 ? 0 : r.col,
                            (r.isUnion ? "union " : "struct ") + SpellType(*this,
                                TypeAndValue{ .TypeName = r.name }));
                // For bitfield records use prePackFields (semantic names before packing).
                if (zeroSizeBitfields)
                    prePackFields.insert(prePackFields.end(), liftedZeroSizeFields.begin(),
                                         liftedZeroSizeFields.end());
                const auto& symFields = hasSemanticOnlyFields ? cxxSemanticFields
                    : anyBitfields ? prePackFields : fields;
                for (const auto& f : symFields)
                {
                    if (f.VariableName.empty()) continue;  // skip unnamed padding markers
                    std::string annSig;
                    for (uint64_t d : f.ConstInnerDimensions)
                        annSig += "[" + std::to_string(d) + "] ";
                    if (f.ConstArraySize > 0)
                        annSig = "[" + std::to_string(f.ConstArraySize) + "] " + annSig;
                    std::string typeSig = SpellType(*this, f);
                    std::string fieldSig = annSig + typeSig + " " + f.VariableName;
                    if (f.IsBitfield && f.BitWidth > 0)
                        fieldSig += ":" + std::to_string(f.BitWidth);
                    s->Register(SymbolKind::Field, r.name + "." + f.VariableName,
                                fileForLsp, r.line, 0, fieldSig);
                }
            }
        }

        // Pass 3: members, now that every body in the batch exists.
        std::vector<const CRecordEntry*> inheritDeferred;
        if (registeringCxxProjectionMembers_)
        for (const auto& [rp, laidOut] : deferredMembers)
        {
            const CRecordEntry& r = *rp;
            RegisterCxxClassMembers(r, fileForLsp);
            if (!laidOut) continue;
            RegisterCxxInheritedMembers(r);
            inheritDeferred.push_back(rp);
            // Hook the C++ complete-object destructor into the SAME destructor slot CFlat uses for
            // its own owning struct locals, so every existing scope-exit, early-return, break and
            // continue cleanup path destroys it exactly once.
            if (cxxNontrivialRecords_.count(r.name) != 0)
                GetOrCreateCxxClassDestructor(r.name);
        }

        // Pass 3b: base-before-derived does NOT hold for a class template specialization used as
        // a base - clang emits cppt::TplBase<TplHolderA> AFTER cppt::TplHolderA, so the first
        // sweep saw no base to clone from. Repeat until nothing new is cloned; the clone step is
        // idempotent, so the only cost of an extra round is the scan. The cap bounds a pathological
        // hierarchy rather than a real one (each round resolves at least one more level).
        for (int round = 0; round < 8; ++round)
        {
            bool added = false;
            for (const CRecordEntry* rp : inheritDeferred)
                added |= RegisterCxxInheritedMembers(*rp);
            if (!added) break;
        }

        // Register each header-COM interface's IID as its "uuid" type annotation (over ALL records,
        // not just freshly-created `ours`, so a cache hit re-annotates already-registered types).
        // EmitIidGlobalFor then resolves iidof(<HeaderComType>) through the existing uuid path.
        for (const auto& r : records)
        {
            if (r.uuid.empty()) continue;
            std::vector<AnnotationValue> anns;
            if (auto it = typeAnnotations_.find(r.name); it != typeAnnotations_.end()) anns = it->second;
            bool had = false;
            for (auto& a : anns) if (a.Name == "uuid") { a.Value = r.uuid; had = true; }
            if (!had) anns.push_back(AnnotationValue{ "uuid", r.uuid });
            SetTypeAnnotations(r.name, std::move(anns));
        }

        RegisterCMacroAliases({}, {}, fileForLsp);

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C record(s) from {}\n", ours.size(), fileForLsp);
    }

bool LLVMBackend::RejectUnsupportedCxxLayout(const std::string& typeName)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || info->layoutRefusal.empty()) return false;
        LogError(std::format("C++ class '{}' {}", DisplayCxxClassName(typeName),
                             info->layoutRefusal));
        return true;
    }

bool LLVMBackend::RejectAbstractCxxClass(const std::string& typeName, const char* what)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || !info->isAbstract) return false;
        LogError(std::format(
            "cannot {} C++ class '{}': it is abstract (it has an unoverridden pure virtual "
            "member), so no complete object of it can exist - use a pointer to a derived class",
            what, DisplayCxxClassName(typeName)));
        return true;
    }

bool LLVMBackend::FindCxxBaseOffset(const std::string& derived, const std::string& base,
                                    uint64_t& offsetOut, bool& foundButInaccessible) const
{
        foundButInaccessible = false;
        if (derived == base) { offsetOut = 0; return true; }
        const CxxClassInfo* info = GetCxxClassInfo(derived);
        if (info == nullptr) return false;
        for (const auto& b : info->bases)
        {
            if (b.isVirtual) { foundButInaccessible = true; continue; }
            const std::string resolvedIdentity = ResolveCxxBaseIdentity(b);
            // Record-only bases have no CxxClassInfo entry, so retain the direct spelling used
            // by the pre-identity lookup path instead of dropping the conversion.
            const std::string baseIdentity = resolvedIdentity.empty() ? b.name : resolvedIdentity;
            if (baseIdentity.empty()) continue;
            uint64_t inner = 0;
            bool innerInaccessible = false;
            const bool hit = baseIdentity == base
                || FindCxxBaseOffset(baseIdentity, base, inner, innerInaccessible);
            if (!hit) { foundButInaccessible = foundButInaccessible || innerInaccessible; continue; }
            if (b.access != cflat_cinterop::AccessPublic || innerInaccessible)
            {
                foundButInaccessible = true;
                continue;
            }
            offsetOut = b.offsetBytes + (baseIdentity == base ? 0 : inner);
            return true;
        }
        return false;
    }

std::string LLVMBackend::CxxStructorReferentSpelling(const std::string& owner,
                                                     const std::string& linkage,
                                                     size_t paramIndex, size_t paramCount) const
{
        auto record = cxxRecordEntries_.find(owner);
        if (record == cxxRecordEntries_.end()) return {};
        std::string referent;
        for (const auto& member : record->second.members)
            if (member.linkageName == linkage && paramIndex < member.paramTypes.size()
                && member.paramTypes.size() == paramCount)
            {
                referent = member.paramTypes[paramIndex];
                break;
            }
        if (referent.starts_with("const ")) referent.erase(0, 6);
        while (!referent.empty() && (referent.back() == '&' || referent.back() == ' '))
            referent.pop_back();
        return referent;
}

bool LLVMBackend::FindCxxReferentSpellingOffset(const std::string& derived,
                                                const std::string& referent,
                                                uint64_t& offsetOut,
                                                const NamedVariable* source) const
{
        // Non-virtual edges only: their offsets hold in any object of the static type.
        auto nonVirtual = [&](auto& self, const std::string& name, uint64_t& out, int depth) -> bool {
            if (depth > 32) return false;
            if (std::string spelling; CxxSpellingForCflatType(name, spelling) && spelling == referent)
            { out = 0; return true; }
            const CxxClassInfo* info = GetCxxClassInfo(name);
            if (info == nullptr) return false;
            for (const auto& base : info->bases)
            {
                if (base.isVirtual) continue;
                if (base.canonicalType == referent) { out = base.offsetBytes; return true; }
                const std::string next = ResolveCxxBaseIdentity(base);
                uint64_t inner = 0;
                if (!next.empty() && self(self, next, inner, depth + 1))
                { out = base.offsetBytes + inner; return true; }
            }
            return false;
        };
        if (nonVirtual(nonVirtual, derived, offsetOut, 0)) return true;
        const CxxClassInfo* info = GetCxxClassInfo(derived);
        if (info == nullptr) return false;
        for (const auto& vb : info->virtualBases)
        {
            uint64_t inner = 0;
            const std::string identity = ResolveCxxBaseIdentity(vb);
            if (vb.canonicalType != referent
                && (identity.empty() || !nonVirtual(nonVirtual, identity, inner, 1)))
                continue;
            if (source != nullptr && !IsCxxCompleteObjectValue(*source))
            {
                std::string dottedReferent;
                for (size_t k = 0; k < referent.size(); ++k)
                    if (referent.compare(k, 2, "::") == 0) { dottedReferent += '.'; ++k; }
                    else dottedReferent += referent[k];
                cxxVirtualBaseRefusal_ = std::format(
                    "converting '{}' to its virtual base '{}' needs the object's dynamic type: only a "
                    "local, global or temporary '{}' converts, not a reference result, a dereferenced "
                    "pointer or an alias parameter (take the base from C++ instead)",
                    SpellType(*this, TypeAndValue{ .TypeName = derived }),
                    dottedReferent,
                    SpellType(*this, TypeAndValue{ .TypeName = derived }));
                return false;
            }
            offsetOut = vb.offsetBytes + inner;
            return true;
        }
        return false;
}

bool LLVMBackend::IsCxxCompleteObjectValue(const NamedVariable& value) const
{
        // A `final` class object is always the complete object: no more-derived type can hold it.
        if (const CxxClassInfo* info = GetCxxClassInfo(value.TypeAndValue.TypeName);
            info != nullptr && info->isFinal) return true;
        if (value.TypeAndValue.Pointer || value.TypeAndValue.IsAlias) return false;
        const llvm::Value* storage = value.Storage;
        if (storage == nullptr)
            return value.Primary != nullptr && value.Primary->getType()->isStructTy();
        if (IsAliasValue(storage)) return false;
        // Struct field steps keep a complete object (a member's type is exact); a byte step is a
        // base-subobject adjustment and proves nothing.
        for (;;)
        {
            storage = storage->stripPointerCasts();
            const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(storage);
            if (gep == nullptr) break;
            if (gep->getSourceElementType()->isIntegerTy(8)) return false;
            storage = gep->getPointerOperand();
        }
        return llvm::isa<llvm::AllocaInst>(storage) || llvm::isa<llvm::GlobalVariable>(storage);
}

bool LLVMBackend::LowerCxxValueBaseOffset(const std::string& derived, const std::string& base,
                                          uint64_t& offsetOut, const NamedVariable& source)
{
        bool inaccessible = false;
        cxxVirtualBaseRefusal_.clear();
        const bool found = FindCxxValueBaseOffset(derived, base, offsetOut, inaccessible, &source);
        if (!found && !cxxVirtualBaseRefusal_.empty()) LogError(cxxVirtualBaseRefusal_);
        cxxVirtualBaseRefusal_.clear();
        return found;
}

bool LLVMBackend::FindCxxValueBaseOffset(const std::string& derived, const std::string& base,
                                         uint64_t& offsetOut, bool& foundButInaccessible,
                                         const NamedVariable* source) const
{
        if (FindCxxBaseOffset(derived, base, offsetOut, foundButInaccessible)) return true;
        const CxxClassInfo* info = GetCxxClassInfo(derived);
        if (info == nullptr) return false;
        for (const auto& vb : info->virtualBases)
        {
            const std::string resolved = ResolveCxxBaseIdentity(vb);
            const std::string identity = resolved.empty() ? vb.name : resolved;
            if (identity.empty()) continue;
            uint64_t inner = 0;
            bool innerInaccessible = false;
            if (identity != base && !FindCxxBaseOffset(identity, base, inner, innerInaccessible))
                continue;
            if (vb.access != cflat_cinterop::AccessPublic) { foundButInaccessible = true; continue; }
            if (source != nullptr && !IsCxxCompleteObjectValue(*source))
            {
                cxxVirtualBaseRefusal_ = std::format(
                    "converting '{}' to its virtual base '{}' needs the object's dynamic type: only a "
                    "local, global or temporary '{}' converts, not a reference result, a dereferenced "
                    "pointer or an alias parameter (take the base from C++ instead)",
                    SpellType(*this, TypeAndValue{ .TypeName = derived }),
                    SpellType(*this, TypeAndValue{ .TypeName = base }),
                    SpellType(*this, TypeAndValue{ .TypeName = derived }));
                return false;
            }
            foundButInaccessible = false;
            offsetOut = vb.offsetBytes + (identity == base ? 0 : inner);
            return true;
        }
        return false;
}

llvm::Value* LLVMBackend::EmitCxxBaseAdjust(llvm::Value* ptr, uint64_t offsetBytes)
{
        if (ptr == nullptr || offsetBytes == 0) return ptr;
        auto* i8 = builder->getInt8Ty();
        llvm::Value* shifted = builder->CreateGEP(i8, ptr, builder->getInt64(offsetBytes),
                                                 "cxx.base");
        // The C++ derived-to-base conversion is null-preserving; clang emits the same select.
        llvm::Value* isNull = builder->CreateICmpEQ(
            ptr, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr->getType())),
            "cxx.base.isnull");
        return builder->CreateSelect(isNull, ptr, shifted, "cxx.base.adj");
    }

LLVMBackend::CxxBaseRoutes LLVMBackend::CollectCxxBaseRoutes(const std::string& derived,
                                                             const std::string& base,
                                                             bool bySpelling) const
{
        CxxBaseRoutes routes;
        auto walk = [&](auto& self, const std::string& name, bool underVirtual, bool isPublic,
                        uint64_t offset, int depth) -> void {
            const CxxClassInfo* info = depth > 32 ? nullptr : GetCxxClassInfo(name);
            if (info == nullptr) { routes.unknown = true; return; }
            for (const auto& b : info->bases)
            {
                const std::string resolved = ResolveCxxBaseIdentity(b);
                const std::string identity = resolved.empty() ? b.name : resolved;
                const bool isVirtual = underVirtual || b.isVirtual;
                const bool publicEdge = isPublic && b.access == cflat_cinterop::AccessPublic;
                const uint64_t at = offset + b.offsetBytes;
                const bool hit = bySpelling ? (!b.canonicalType.empty()
                                               && SqueezeCxxSpelling(b.canonicalType) == base)
                                            : (!identity.empty() && identity == base);
                if (hit)
                {
                    if (isVirtual) routes.viaVirtual = true;
                    else
                    {
                        ++routes.nonVirtual;
                        routes.offset = at;
                        routes.offsetPublic = publicEdge;
                    }
                    continue;
                }
                if (identity.empty()) { routes.unknown = true; continue; }
                self(self, identity, isVirtual, publicEdge, at, depth + 1);
            }
        };
        walk(walk, derived, false, true, 0, 0);
        return routes;
}

bool LLVMBackend::IsCxxDerivedToClangBasePointer(const TypeAndValue& from,
                                                 const TypeAndValue& to) const
{
        if (!from.Pointer || !to.Pointer || from.ElemPointer || to.ElemPointer
            || from.PointerDepth > 1 || to.PointerDepth > 1)
            return false;
        if (from.TypeName == to.TypeName || from.TypeName.empty() || to.TypeName.empty())
            return false;
        if (!IsCxxRecord(from.TypeName) || !IsCxxRecord(to.TypeName)) return false;
        const CxxClassInfo* info = GetCxxClassInfo(from.TypeName);
        if (info == nullptr || info->virtualBases.empty()) return false;
        return CollectCxxBaseRoutes(from.TypeName, to.TypeName, false).viaVirtual;
}

LLVMBackend::CxxSpelledParamRelation LLVMBackend::CxxVoidParamClassRelation(
    const FunctionSymbol& candidate, size_t index, const TypeAndValue& param,
    const TypeAndValue& arg, std::string& base, bool& bySpelling) const
{
        bySpelling = false;
        if (!candidate.IsCxx || (candidate.IsMethod && index == 0))
            return CxxSpelledParamRelation::None;
        if (!param.Pointer || param.TypeName != "void" || !arg.Pointer)
            return CxxSpelledParamRelation::None;
        return CxxSpelledClassPointerRelation(CxxReferenceParameterSpelling(candidate, index), param,
                                              arg, base, bySpelling);
}

LLVMBackend::CxxSpelledParamRelation LLVMBackend::CxxSpelledClassPointerRelation(
    const std::string& spelling, const TypeAndValue& param, const TypeAndValue& arg,
    std::string& base, bool& bySpelling) const
{
        using Rel = CxxSpelledParamRelation;
        bySpelling = false;
        if (!param.Pointer || param.TypeName != "void" || param.ElemPointer || param.IsAlias
            || param.IsRvalueRef || param.IsFunctionPointer || param.IsArrayView
            || param.IsCxxRefToPointer || param.IsCxxNullptrT || param.PointerDepth > 1)
            return Rel::None;
        if (!arg.Pointer || arg.ElemPointer || arg.IsFunctionPointer || arg.IsArrayView
            || arg.IsInterface || arg.PointerDepth > 1 || !IsCxxRecord(arg.TypeName))
            return Rel::None;
        if (spelling.empty() || spelling.find_first_of("&()[") != std::string::npos
            || std::count(spelling.begin(), spelling.end(), '*') != 1)
            return Rel::None;
        std::string referent = spelling.substr(0, spelling.find('*'));
        for (bool peeled = true; peeled; )
        {
            peeled = false;
            while (!referent.empty() && referent.back() == ' ') referent.pop_back();
            for (const char* w : { "const", "volatile" })
                if (referent.ends_with(w) && referent.size() > std::strlen(w)
                    && referent[referent.size() - std::strlen(w) - 1] == ' ')
                { referent.erase(referent.size() - std::strlen(w)); peeled = true; }
            for (const char* w : { "const ", "volatile ", "class ", "struct " })
                if (referent.starts_with(w)) { referent.erase(0, std::strlen(w)); peeled = true; }
        }
        if (referent.empty() || referent == "void") return Rel::None;
        TypeAndValue mappedType;
        bool mapped = false;
        if (TryMapCxxForeignSpelling(spelling, mappedType, mapped) && mapped && mappedType.Pointer
            && !mappedType.ElemPointer && IsCxxRecord(mappedType.TypeName))
        {
            if (mappedType.TypeName == arg.TypeName) return Rel::Same;
            const CxxBaseRoutes routes = CollectCxxBaseRoutes(arg.TypeName, mappedType.TypeName, false);
            if (routes.Found()) { base = mappedType.TypeName; return Rel::Base; }
            return routes.unknown ? Rel::None : Rel::Unrelated;
        }
        // The named class is not registered yet: compare clang's canonical spellings.
        const std::string squeezed = SqueezeCxxSpelling(referent);
        std::string argSpelling;
        if (auto record = cxxRecordEntries_.find(arg.TypeName);
            record != cxxRecordEntries_.end() && !record->second.canonicalCtype.empty())
            argSpelling = SqueezeCxxSpelling(StripCxxRecordTag(record->second.canonicalCtype));
        if (!argSpelling.empty() && argSpelling == squeezed) return Rel::Same;
        const CxxBaseRoutes routes = CollectCxxBaseRoutes(arg.TypeName, squeezed, true);
        if (routes.Found()) { base = referent; bySpelling = true; return Rel::Base; }
        return routes.unknown || argSpelling.empty() ? Rel::None : Rel::Unrelated;
}

bool LLVMBackend::CxxArgumentPointeeConst(const NamedVariable& argument) const
{
        const TypeAndValue& t = argument.TypeAndValue;
        if (!t.Pointer || t.IsFunctionPointer || t.IsInterface) return false;
        if (t.PointeeConst()) return true;
        if (argument.Primary == nullptr || !argument.Primary->getType()->isPointerTy()
            || llvm::isa<llvm::ConstantPointerNull>(argument.Primary))
            return false;
        const int depth = t.ValuePointerDepth();
        std::string slotName;
        if (depth >= 1 && ((StoredConstLevels(argument.Primary, slotName) >> (depth - 1)) & 1u) != 0)
            return true;
        // The provenance walk skips C++ record slots: read a native slot's own spelling (an
        // explicit cast stamps its own qualification on the load).
        auto* load = llvm::dyn_cast<llvm::LoadInst>(argument.Primary);
        if (load != nullptr && IsCxxRecord(t.TypeName) && load->getMetadata("cflat.const.cast") == nullptr)
            if (const TypeAndValue* slot = DeclaredConstSlot(load->getPointerOperand(), slotName))
                return slot->Pointer && !slot->IsArrayView && slot->ValuePointerDepth() == depth
                    && (slot->ConstMask & 1u) != 0;
        return false;
}

LLVMBackend::CxxPointeeConstRelation LLVMBackend::CxxCandidatePointeeConstRelation(
    const FunctionSymbol& candidate, size_t index, const NamedVariable& fromArgument,
    const TypeAndValue& to) const
{
        // C++ fixed arrays decay at this boundary; retain their element const for the shared
        // record/primitive pointer relation, including pointers to inner array dimensions.
        TypeAndValue decayedFrom = fromArgument.TypeAndValue;
        if (decayedFrom.TypeName.empty() && !fromArgument.InferSourceTypeName.empty())
            decayedFrom.TypeName = fromArgument.InferSourceTypeName;
        // A native pointer's pointee const lives on its value, not the loaded TypeAndValue.
        if (decayedFrom.Pointer && !decayedFrom.PointeeConst() && CxxArgumentPointeeConst(fromArgument))
            decayedFrom.ConstMask |= 1u;
        if (decayedFrom.ConstArraySize > 0 && !decayedFrom.Pointer
            && !decayedFrom.ElemPointer && !decayedFrom.IsArrayView
            && !decayedFrom.IsFunctionPointer && !decayedFrom.IsInterface)
        {
            decayedFrom.Pointer = true;
            if (decayedFrom.PointerDepth == 0) decayedFrom.PointerDepth = 1;
            decayedFrom.ConstArraySize = 0;
        }
        if (candidate.IsCxx && !(candidate.IsMethod && index == 0)
            && decayedFrom.Pointer && !decayedFrom.ElemPointer
            && !decayedFrom.ConstInnerDimensions.empty() && to.Pointer && to.ElemPointer)
        {
            const std::string spelling = CxxReferenceParameterSpelling(candidate, index);
            std::string compactSpelling;
            for (char c : spelling)
                if (!std::isspace(static_cast<unsigned char>(c))) compactSpelling += c;
            // `int (*)[3]` and a qualified row pointer `int (*const)[3]`: qualifiers after the `*` are
            // the pointer's own, not the pointee's.
            bool rowPointer = false;
            for (size_t at = compactSpelling.find("(*"); at != std::string::npos && !rowPointer;
                 at = compactSpelling.find("(*", at + 1))
            {
                size_t pos = at + 2;
                for (bool skipped = true; skipped;)
                {
                    skipped = false;
                    for (const char* q : {"const", "volatile", "__restrict__", "__restrict", "restrict"})
                        if (compactSpelling.compare(pos, std::strlen(q), q) == 0)
                        {
                            pos += std::strlen(q);
                            skipped = true;
                            break;
                        }
                }
                rowPointer = compactSpelling.compare(pos, 2, ")[") == 0;
            }
            if (rowPointer)
            {
                const size_t open = spelling.find('(');
                const std::string pointeeSpelling = spelling.substr(0, open) + " *";
                return CxxPointeeConstRelationOf(decayedFrom, CxxSpellingHasConstPointee(pointeeSpelling));
            }
        }
        auto relation = CxxRecordPointeeConstRelation(decayedFrom, to);
        // Multi-level pointers need every pointee qualifier compared. Looking only at bit 0
        // makes `T* const*` indistinguishable from `T**` during overload selection.
        if (candidate.IsCxx && relation != CxxPointeeConstRelation::Unrelated
            && decayedFrom.Pointer && to.Pointer
            && !decayedFrom.IsFunctionPointer && !to.IsFunctionPointer
            && !IsCxxRecord(decayedFrom.TypeName) && !IsCxxRecord(to.TypeName)
            && decayedFrom.ValuePointerDepth() > 1
            && decayedFrom.ValuePointerDepth() == to.ValuePointerDepth()
            && !decayedFrom.TypeName.empty() && !to.TypeName.empty()
            && ResolveTypeAlias(decayedFrom.TypeName) == ResolveTypeAlias(to.TypeName))
        {
            const int depth = decayedFrom.ValuePointerDepth();
            const uint32_t sourceMask = decayedFrom.PointeeConstMask();
            uint32_t targetMask = to.PointeeConstMask();
            const std::string spelling = CxxReferenceParameterSpelling(candidate, index);
            if (!spelling.empty() && spelling.find_first_of("()[]&") == std::string::npos)
                targetMask = CxxSpellingPointeeConstMask(spelling, depth);
            if ((sourceMask & ~targetMask) != 0)
                return CxxPointeeConstRelation::Drops;
            const uint32_t addedMask = targetMask & ~sourceMask;
            for (int level = 0; level < depth; ++level)
            {
                if ((addedMask & TypeAndValue::ConstBitAt(level)) == 0) continue;
                const uint32_t higherLevels = TypeAndValue::ConstBitsBelow(depth)
                    & ~TypeAndValue::ConstBitsThrough(level);
                if ((targetMask & higherLevels) != higherLevels)
                    return CxxPointeeConstRelation::Drops;
            }
            relation = addedMask == 0 ? CxxPointeeConstRelation::Same
                                      : CxxPointeeConstRelation::Adds;
        }
        if (!decayedFrom.Pointer || !to.Pointer) return relation;
        const std::string referenceSpelling = candidate.IsCxx
            ? CxxReferenceParameterSpelling(candidate, index) : std::string();
        CxxReferenceKind referenceKind = CxxReferenceKind::None;
        CxxSpellingWithoutRef(referenceSpelling, &referenceKind);
        if (candidate.IsCxx && referenceKind == CxxReferenceKind::RefToPointer
            && decayedFrom.Pointer && to.Pointer
            && decayedFrom.ElemPointer == to.ElemPointer
            && decayedFrom.ValuePointerDepth() == to.ValuePointerDepth()
            && !decayedFrom.TypeName.empty() && !to.TypeName.empty()
            && ResolveTypeAlias(decayedFrom.TypeName) == ResolveTypeAlias(to.TypeName))
        {
            return CxxPointeeConstRelationOf(
                decayedFrom, CxxSpellingHasConstPointee(referenceSpelling));
        }
        if (relation != CxxPointeeConstRelation::Unrelated || !candidate.IsCxx
            || (candidate.IsMethod && index == 0))
            return relation;
        if (!to.Pointer || to.TypeName != "void" || to.ElemPointer || to.IsAlias || to.IsRvalueRef
            || to.IsFunctionPointer || to.PointerDepth > 1 || !decayedFrom.Pointer
            || decayedFrom.ElemPointer || decayedFrom.IsFunctionPointer
            || decayedFrom.PointerDepth > 1 || !IsCxxRecord(decayedFrom.TypeName))
            return relation;
        std::string base;
        bool bySpelling = false;
        if (CxxVoidParamClassRelation(candidate, index, to, decayedFrom, base, bySpelling)
            == CxxSpelledParamRelation::Unrelated)
            return relation;
        // The pointee const is whatever the spelling puts before its single `*`.
        bool toConst = to.PointeeConst();
        const std::string spelling = CxxReferenceParameterSpelling(candidate, index);
        if (const size_t star = spelling.find('*');
            star != std::string::npos && spelling.find('*', star + 1) == std::string::npos
            && spelling.find_first_of("&()[") == std::string::npos)
        {
            const std::string referent = " " + spelling.substr(0, star) + " ";
            toConst = referent.find(" const ") != std::string::npos;
        }
        return CxxPointeeConstRelationOf(decayedFrom, toConst);
}

LLVM_ATTRIBUTE_NOINLINE bool LLVMBackend::CxxPointerReferenceNeedsConversion(
        const NamedVariable& argument, const TypeAndValue& param) const
{
        TypeAndValue from = argument.TypeAndValue;
        if (!from.Pointer || !param.Pointer) return false;
        if (!from.PointeeConst() && CxxArgumentPointeeConst(argument))
            from.SetPointeeConstBit(true);
        if (from.ElemPointer != param.ElemPointer
            || from.ValuePointerDepth() != param.ValuePointerDepth())
            return false;
        if (from.PointeeConstMask() != param.PointeeConstMask())
            return true;
        const auto relation = CxxRecordPointeeConstRelation(from, param);
        return relation == CxxPointeeConstRelation::Adds
            || relation == CxxPointeeConstRelation::Drops
            || IsCxxDerivedToBasePointer(from, param);
}

LLVM_ATTRIBUTE_NOINLINE bool LLVMBackend::CxxPointerReferenceBindingInvalid(
        const FunctionSymbol& candidate, size_t index, const NamedVariable& argument,
        const TypeAndValue& param, CxxPointeeConstRelation relation) const
{
        if (!argument.TypeAndValue.Pointer || candidate.UniqueName.starts_with("__cflat_tpl_")
            || (candidate.IsMethod && index == 0))
            return false;
        const CxxPointerReferenceInfo properties = CxxPointerReferenceProperties(
            candidate, index, param);
        if (!properties.IsReference || properties.IsConstReference || param.IsRvalueRef)
            return false;
        return relation == CxxPointeeConstRelation::Adds
            || CxxPointerReferenceNeedsConversion(argument, param)
            || IsCxxRvalueReferenceArgument(argument);
}

/*
 * A derived-to-virtual-base (or ambiguous) pointer conversion has no offset cflat may compute: the
 * virtual base sits where the most derived object put it, read at run time from the vtable (vbptr
 * on MS). Clang emits the conversion in `Base* w(Derived* p) { return p; }`, requested once per
 * class pair and only for such a conversion; an ill-formed one (ambiguous, inaccessible) is
 * refused with clang's diagnostic.
 */
llvm::Value* LLVMBackend::EmitCxxClangBaseConversion(const std::string& derived,
                                                     const std::string& baseSpelling,
                                                     llvm::Value* ptr, std::string& error)
{
        if (ptr == nullptr || !ptr->getType()->isPointerTy())
        {
            error = "the converted value is not a pointer";
            return nullptr;
        }
        std::string derivedSpelling;
        if (!CxxSpellingForCflatType(derived, derivedSpelling))
        {
            error = "the C++ class type is not registered";
            return nullptr;
        }
        const std::string key = derivedSpelling + "|" + baseSpelling;
        auto known = cxxBaseConversionWrappers_.find(key);
        if (known == cxxBaseConversionWrappers_.end())
        {
            std::string wrapperName = std::format("__cflat_tobase_{:016x}",
                                                  HashWrapperKey(key + "|base-conversion"));
            std::string refusal;
            auto groupIt = cxxTypeOwnerGroup_.find(derived);
            CxxRequestGroup group = groupIt == cxxTypeOwnerGroup_.end()
                ? CxxRequestGroup{} : MakeCxxClassRequestGroup(groupIt->second, derived);
            if (group.headers.empty())
                refusal = "the C++ class's import group is unavailable";
            else
            {
                const std::string source = "extern \"C\" __attribute__((weak)) " + baseSpelling
                    + " * " + wrapperName + "(" + derivedSpelling + " * p0) noexcept { return p0; }\n";
                CxxRequestGroupScope groupScope(*this, &group);
                CSigEntry bound;
                std::string wrapperError;
                if (RequestGeneratedCxxWrapper(group, source, wrapperName, "BASE_CONVERSION", bound,
                                               wrapperError))
                {
                    bound.name = wrapperName;
                    RegisterCSignatures({ bound }, group.headers.front());
                }
                else
                {
                    std::string cause = CxxFirstDiagnosticLine(lastCxxWrapperCause_);
                    if (cause.empty()) cause = FirstCxxErrorLine(wrapperError);
                    if (cause.empty()) cause = "the generated C++ conversion could not be compiled";
                    refusal = cause.starts_with("clang: ") ? cause : "clang: " + cause;
                }
            }
            if (!refusal.empty()) wrapperName.clear();
            known = cxxBaseConversionWrappers_.emplace(key, std::make_pair(wrapperName, refusal)).first;
        }
        if (known->second.first.empty())
        {
            error = known->second.second;
            return nullptr;
        }
        llvm::Function* fn = nullptr;
        if (auto it = functionTable.find(known->second.first); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.UniqueName == known->second.first && symbol.Function != nullptr)
                    fn = llvm::dyn_cast<llvm::Function>(symbol.Function);
        if (fn == nullptr) fn = module->getFunction(known->second.first);
        if (fn == nullptr || fn->arg_size() != 1 || !fn->getReturnType()->isPointerTy())
        {
            error = "the generated C++ base conversion could not be registered";
            return nullptr;
        }
        ValidateCxxDemandAtUseSite(known->second.first);
        return builder->CreateCall(fn->getFunctionType(), fn, { ptr }, "cxx.base.clang");
}

llvm::Value* LLVMBackend::LowerCxxPointerToBase(const std::string& derived, const std::string& base,
                                                bool bySpelling, llvm::Value* ptr,
                                                const std::string& srcDesc,
                                                const std::string& destDesc,
                                                const std::string& what, bool* refused)
{
        if (refused != nullptr) *refused = false;
        if (ptr == nullptr || !ptr->getType()->isPointerTy()) return ptr;
        const CxxBaseRoutes routes = CollectCxxBaseRoutes(
            derived, bySpelling ? SqueezeCxxSpelling(base) : base, bySpelling);
        if (!routes.Found()) return ptr;
        if (!routes.NeedsClang())
        {
            if (bySpelling && routes.offsetPublic) return EmitCxxBaseAdjust(ptr, routes.offset);
            uint64_t off = 0;
            bool inaccessible = false;
            if (!bySpelling && FindCxxBaseOffset(derived, base, off, inaccessible))
                return EmitCxxBaseAdjust(ptr, off);
        }
        std::string baseSpelling = base;
        std::string error;
        llvm::Value* converted = nullptr;
        if (bySpelling || CxxSpellingForCflatType(base, baseSpelling))
            converted = EmitCxxClangBaseConversion(derived, baseSpelling, ptr, error);
        else
            error = "the C++ base class type is not registered";
        if (converted != nullptr) return converted;
        if (refused != nullptr) *refused = true;
        LogError(std::format("cannot convert '{}' to '{}' for {}: {}", srcDesc, destDesc, what,
                             error));
        return ptr;
}

bool LLVMBackend::LowerCxxReferenceToBase(const std::string& derived, const std::string& base,
                                          const NamedVariable& source, llvm::Value*& address)
{
        const CxxBaseRoutes routes = CollectCxxBaseRoutes(derived, base, false);
        // A complete object knows where its single virtual base sits; any other virtual or
        // ambiguous route asks clang.
        if (address != nullptr && address->getType()->isPointerTy() && routes.NeedsClang()
            && (routes.nonVirtual > 0 || !IsCxxCompleteObjectValue(source)))
        {
            TypeAndValue from{ .TypeName = derived };
            TypeAndValue to{ .TypeName = base };
            bool refused = false;
            address = LowerCxxPointerToBase(derived, base, false, address, SpellType(*this, from),
                                            SpellType(*this, to), "the reference", &refused);
            return !refused;
        }
        uint64_t offset = 0;
        if (!LowerCxxValueBaseOffset(derived, base, offset, source)) return false;
        address = EmitCxxBaseAdjust(address, offset);
        return true;
}

llvm::Value* LLVMBackend::AdjustCxxPointerForStore(const TypeAndValue& dest,
                                                   const TypeAndValue& src, llvm::Value* value,
                                                   const std::string& destDesc)
{
        if (value == nullptr || !value->getType()->isPointerTy()) return value;
        if (!dest.Pointer || !src.Pointer || dest.TypeName == src.TypeName) return value;
        if (!IsCxxRecord(dest.TypeName) || !IsCxxRecord(src.TypeName)) return value;
        // A virtual-base or ambiguous route is clang's conversion, never a static offset.
        if (!src.ElemPointer && !dest.ElemPointer
            && CollectCxxBaseRoutes(src.TypeName, dest.TypeName, false).NeedsClang())
            return LowerCxxPointerToBase(src.TypeName, dest.TypeName, false, value,
                                         SpellType(*this, src), SpellType(*this, dest), destDesc);
        uint64_t off = 0;
        bool inaccessible = false;
        if (FindCxxBaseOffset(src.TypeName, dest.TypeName, off, inaccessible))
            return EmitCxxBaseAdjust(value, off);
        if (inaccessible)
        {
            // Every user-facing surface demangles: `Box$int` is the internal spelling.
            const std::string srcBase = SpellType(*this, TypeAndValue{ .TypeName = src.TypeName });
            const std::string destBase = SpellType(*this, TypeAndValue{ .TypeName = dest.TypeName });
            LogError(std::format(
                "cannot convert '{}' to '{}' for {}: '{}' is not a PUBLIC base of '{}', and a "
                "conversion to a private or protected base is not allowed",
                SpellType(*this, src), SpellType(*this, dest), destDesc, destBase, srcBase));
        }
        return value;
    }

llvm::Value* LLVMBackend::AdjustCxxPointerForUniqueAdoption(
    const NamedVariable& arg, const TypeAndValue& param, llvm::Value* value,
    const std::string& destDesc)
{
        if (value == nullptr || !value->getType()->isPointerTy()
            || !IsCoreUniqueType(param.TypeName))
            return value;
        const std::string targetName = MangledGenericArgument(*this, param.TypeName);
        const std::string sourceName = arg.TypeAndValue.TypeName;
        if (!IsCxxRecord(targetName) || !IsCxxRecord(sourceName)
            || targetName == sourceName)
            return value;

        uint64_t offset = 0;
        bool inaccessible = false;
        if (!FindCxxBaseOffset(sourceName, targetName, offset, inaccessible))
        {
            if (!inaccessible) return value;
            TypeAndValue destPointer;
            destPointer.TypeName = targetName;
            destPointer.Pointer = true;
            TypeAndValue sourcePointer = arg.TypeAndValue;
            sourcePointer.Pointer = true;
            return AdjustCxxPointerForStore(
                destPointer, sourcePointer, value, destDesc);
        }
        if (!CxxHasVirtualDestructor(targetName))
        {
            TypeAndValue targetType;
            targetType.TypeName = targetName;
            LogError(std::format(
                "cannot adopt a '{}' into 'unique {}*': '{}' has no virtual destructor, so "
                "deleting through the base would be undefined",
                SpellType(*this, arg.TypeAndValue), SpellType(*this, targetType),
                SpellType(*this, targetType)));
        }

        return EmitCxxBaseAdjust(value, offset);
    }

bool LLVMBackend::IsCxxUniquePtrPointee(const std::string& pointee) const
{
        if (pointee.empty() || !HasCxxImportGroup()) return false;
        const std::string resolved = ResolveTypeAlias(pointee);
        return IsCxxRecord(resolved) || IsCppStructName(resolved);
}

std::string LLVMBackend::CxxUniquePtrForUniqueKeyword(const std::string& pointee,
                                                      std::string& error)
{
        error.clear();
        if (!IsCxxUniquePtrPointee(pointee)) return {};
        const std::string resolved = ResolveTypeAlias(pointee);
        std::string name = MangleGenericInstance(*this, "std.unique_ptr", { resolved });
        if (dataStructures.count(name) != 0 && CxxUniquePtrPointee(name) == resolved) return name;
        if (TryRequestCxxType("std.unique_ptr", { resolved }, name, error))
            return ResolveTypeAlias(name);
        if (error.empty() || error.find("does not name a C++ class type") != std::string::npos)
            error = std::format("'unique' on C++ class '{}' names std::unique_ptr<{}>, which no "
                                "imported C++ header declares - add 'import cpp \"memory\";'",
                                SpellType(*this, TypeAndValue{ .TypeName = resolved }),
                                SpellType(*this, TypeAndValue{ .TypeName = resolved }));
        return {};
}

std::string LLVMBackend::CxxUniquePtrPointee(const std::string& typeName) const
{
        if (!typeName.starts_with("std.unique_ptr$")) return {};
        TypeSpelling spelling;
        if (!DemangleType(*this, typeName, spelling) || spelling.base != "std.unique_ptr"
            || spelling.args.size() != 1)
            return {};
        return MangleType(*this, spelling.args[0]);
}

bool LLVMBackend::IsRawPointerForCxxUniquePtr(const std::string& typeName,
                                              const NamedVariable& source) const
{
        const std::string pointee = CxxUniquePtrPointee(typeName);
        if (pointee.empty()) return false;
        if (source.Primary != nullptr && llvm::isa<llvm::ConstantPointerNull>(source.Primary))
            return true;
        const TypeAndValue& tv = source.TypeAndValue;
        if (!tv.Pointer || tv.ElemPointer || tv.IsInterface || tv.IsFunctionPointer
            || tv.IsArrayView || tv.PointerDepth > 1)
            return false;
        const std::string sourceType = ResolveTypeAlias(tv.TypeName);
        if (sourceType == pointee) return true;
        return IsCxxRecord(sourceType) && IsCxxRecord(pointee) && IsCxxBaseOf(pointee, sourceType);
}

bool LLVMBackend::IsOwnedCxxAdoptSource(const NamedVariable& arg) const
{
        bool rebound = false;
        if (arg.BorrowsOwningLocal && arg.OwningLocalStorage != nullptr)
        {
            const auto* owner = FindVariableByStorage(arg.OwningLocalStorage);
            rebound = arg.PointerRebound || (owner != nullptr && owner->PointerRebound);
        }
        bool isBorrowed = arg.IsBorrowed || arg.IsAliasBorrow || arg.TypeAndValue.IsAlias
            || arg.BorrowsOwnedElement || (arg.BorrowsOwningLocal && !rebound);
        if (isBorrowed) return false;
        return arg.IsOwning || arg.IsOwningString || arg.IsOwningStruct || arg.TypeAndValue.IsMove
            || arg.IsExplicitMove || lastCallReturnsOwned || lastOwningResult
            || IsOwnedNewTemp(arg.Primary) || IsOwningPtrTempValue(arg.Primary)
            || IsMovedOutPtrValue(arg.Primary) || rebound
            || (arg.FieldName.empty() && !arg.CallerName.empty() && IsVariableOwning(arg.CallerName));
}

bool LLVMBackend::TryAdoptRawPointerIntoCxxUniquePtr(const std::string& typeName,
                                                    llvm::Value* slot,
                                                    const NamedVariable& source,
                                                    const std::string& destDesc,
                                                    const TypeAndValue* targetTypeInfo)
{
        if (slot == nullptr || !IsRawPointerForCxxUniquePtr(typeName, source)) return false;
        llvm::Value* value = source.Primary != nullptr ? source.Primary : LoadArgStorage(source);
        if (value == nullptr || !value->getType()->isPointerTy()) return false;
        if (llvm::isa<llvm::ConstantPointerNull>(value))
        {
            const auto* ctor = FindCxxDefaultCtor(typeName);
            if (ctor == nullptr) return false;
            EmitCxxStructorCall(typeName, *ctor, slot, {});
            return true;
        }
        const std::string pointee = CxxUniquePtrPointee(typeName);
        if (!IsOwnedCxxAdoptSource(source))
        {
            // Same owned/borrowed split as core unique<T>'s `move T*` ctor: adopting a borrow
            // would free the pointee twice.
            const std::string borrowed = BorrowedSourceDiagnosticName(source);
            TypeAndValue fallbackType{ .TypeName = typeName };
            const std::string uniqueType = SpellDiagnosticType(*this,
                targetTypeInfo != nullptr ? *targetTypeInfo : fallbackType);
            if (destDesc == "the return value")
                LogErrorMessage(
                    "cannot return borrowed value '{}' as '{}'; the return type owns the pointee - "
                    "use 'new', a move source, or a move-returning call",
                    { borrowed.empty() ? std::string("expression") : borrowed, uniqueType });
            else if (borrowed.empty())
                LogErrorMessage(
                    "cannot adopt a borrowed value into '{}' for {} - the source still owns it; "
                    "use 'new', a 'move' expression, or a move-returning call",
                    { uniqueType, destDesc });
            else
                LogErrorMessage(
                    "cannot adopt borrowed value '{}' into '{}' for {} - the source still owns it; "
                    "use 'new', a 'move' expression, or a move-returning call",
                    { borrowed, uniqueType, destDesc });
            return true;
        }
        const std::string sourceType = ResolveTypeAlias(source.TypeAndValue.TypeName);
        if (sourceType != pointee && !CxxHasVirtualDestructor(pointee))
        {
            TypeAndValue targetType{ .TypeName = pointee };
            LogError(std::format(
                "cannot adopt a '{}' into 'unique {}*': '{}' has no virtual destructor, so "
                "deleting through the base would be undefined",
                SpellType(*this, source.TypeAndValue), SpellType(*this, targetType),
                SpellType(*this, targetType)));
            return true;
        }
        // A derived pointer is adjusted to its base subobject here, as C++ would at the
        // unique_ptr(pointer) parameter.
        if (sourceType != pointee)
        {
            uint64_t offset = 0;
            bool inaccessible = false;
            if (!FindCxxBaseOffset(sourceType, pointee, offset, inaccessible)) return false;
            value = EmitCxxBaseAdjust(value, offset);
        }
        // A named source is consumed like a core unique<T>'s `move T*` argument: its slot reads
        // null afterwards, so its own ownership flag can never free the adopted pointee too.
        llvm::Value* sourceSlot = source.Storage;
        if (sourceSlot == nullptr && !source.CallerName.empty())
            sourceSlot = FindVariableStorage(source.CallerName).Storage;
        const std::string sourceName = !source.CallerName.empty() ? source.CallerName
            : source.Storage != nullptr ? source.TypeAndValue.VariableName : std::string();
        if (sourceSlot != nullptr && sourceSlot->getType()->isPointerTy())
            builder->CreateStore(llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(value->getType())), sourceSlot);
        if (!sourceName.empty() && sourceSlot != nullptr)
        {
            RecordNullSet(sourceName);
            // An explicit `move p` of a raw pointer leaves p readable as null, not moved-from.
            if (source.IsExplicitMove)
            {
                MarkVariableUnmoved(sourceName);
                MarkVariableExplicitlyMovedNull(sourceName);
            }
        }
        TypeAndValue pointerType{ .TypeName = pointee };
        pointerType.Pointer = true;
        NamedVariable raw;
        raw.Primary = value;
        raw.BaseType = value->getType();
        raw.TypeAndValue = pointerType;
        raw.IsRvalue = true;
        const std::vector<NamedVariable> rawArgs{ raw };
        std::string why;
        const auto ctorTypes = std::vector<TypeAndValue>{ pointerType };
        TryBindRefusedCxxMember(typeName, "__ctor", ctorTypes.size());
        const auto* ctor = SelectCxxConstructor(typeName, ctorTypes, why, false, &rawArgs);
        if (ctor == nullptr)
        {
            TryBindRefusedCxxMember(typeName, "__ctor", ctorTypes.size());
            ctor = SelectCxxConstructor(typeName, ctorTypes, why, false, &rawArgs);
        }
        // The listed pick is the constructor called here (no clang-resolved wrapper).
        if (std::string refusal = CxxConstructorBodyRefusalError(ctor); !refusal.empty())
            LogError(refusal);
        if (ctor == nullptr)
        {
            LogError(std::format("cannot adopt a raw pointer into '{}' for {}: {}",
                                 SpellType(*this, TypeAndValue{ .TypeName = typeName }),
                                 destDesc, why));
            return true;
        }
        EmitCxxStructorCall(typeName, *ctor, slot, { value }, &rawArgs);
        // The unique_ptr now owns the pointee: no end-of-expression release of the source temp.
        ConsumeOwnedNewTemp(source.Primary);
        ConsumeOwnedNewTemp(value);
        lastOwningResult = false;
        lastCallReturnsOwned = false;
        return true;
}

llvm::Value* LLVMBackend::AdoptRawPointerAsCxxUniquePtrValue(const std::string& typeName,
                                                             const NamedVariable& source,
                                                             const std::string& destDesc)
{
        if (!IsRawPointerForCxxUniquePtr(typeName, source)) return nullptr;
        llvm::Type* objectType = GetType(TypeAndValue{ .TypeName = typeName });
        if (objectType == nullptr || !objectType->isSized()) return nullptr;
        auto* slot = AllocaAtEntry(objectType, nullptr, "cxx.adopt.value");
        if (!TryAdoptRawPointerIntoCxxUniquePtr(typeName, slot, source, destDesc)) return nullptr;
        // Relocated, not copied: a default-deleter unique_ptr is one pointer, and the slot is
        // never destroyed, so the caller's store is the object's only owner.
        return CreateLoad(objectType, slot);
}

llvm::Value* LLVMBackend::CxxReferenceResultAsPointer(const TypeAndValue& dest,
                                                      const NamedVariable& src,
                                                      const std::string& destDesc)
{
        if (!dest.Pointer || dest.IsFunctionPointer || dest.IsInterface) return nullptr;
        const TypeAndValue& srcTv = src.TypeAndValue;
        if (!srcTv.IsAlias || srcTv.Pointer || !IsCxxRecord(srcTv.TypeName)) return nullptr;
        // IsAliasValue is the discriminator: the address must be the borrow the call handed back,
        // never a temp alloca holding a COPY of the referent.
        if (src.Storage == nullptr || !src.Storage->getType()->isPointerTy()) return nullptr;
        if (!IsAliasValue(src.Storage)) return nullptr;
        if (dest.TypeName == srcTv.TypeName) return src.Storage;
        // A different destination class only binds when it is a PUBLIC base of the referent;
        // anything else keeps the caller's refusal rather than aliasing an unrelated layout.
        if (!IsCxxRecord(dest.TypeName)) return nullptr;
        uint64_t offset = 0;
        bool inaccessible = false;
        if (FindCxxBaseOffset(srcTv.TypeName, dest.TypeName, offset, inaccessible))
            return EmitCxxBaseAdjust(src.Storage, offset);
        if (!inaccessible) return nullptr;
        // Found behind a non-public base: AdjustCxxPointerForStore owns that diagnostic.
        TypeAndValue srcAsPointer = srcTv;
        srcAsPointer.Pointer = true;
        return AdjustCxxPointerForStore(dest, srcAsPointer, src.Storage, destDesc);
    }

llvm::Value* LLVMBackend::EmitCxxVirtualCallee(const FunctionSymbol& candidate,
                                               llvm::Value* thisPtr,
                                               llvm::Value** adjustedThis)
{
        if (adjustedThis != nullptr) *adjustedThis = thisPtr;
        if (thisPtr == nullptr || candidate.Function == nullptr) return nullptr;
        auto it = cxxVirtualSlotByLinkage_.find(candidate.UniqueName);
        if (it == cxxVirtualSlotByLinkage_.end()) return nullptr;
        // Mirror MicrosoftCXXABI::adjustThisArgumentForVirtualFunctionCall for secondary vfptrs.
        // Both the vptr load and the virtual call use the adjusted subobject pointer.
        llvm::Value* dispatchThis = EmitCxxBaseAdjust(
            thisPtr, static_cast<uint64_t>(it->second.second));
        if (adjustedThis != nullptr) *adjustedThis = dispatchThis;
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        llvm::Value* vptr = builder->CreateLoad(ptrTy, dispatchThis, "vtable");
        llvm::Value* slot = builder->CreateGEP(ptrTy, vptr, builder->getInt64(it->second.first),
                                               "vfn.slot");
        return builder->CreateLoad(ptrTy, slot, "vfn");
    }

bool LLVMBackend::EmitCxxVirtualDelete(const std::string& typeName, llvm::Value* ptr)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || info->dtorDeletingVtableIndex < 0 || ptr == nullptr) return false;
        // Refused: emit nothing, so the destructor never reaches the demand companion either.
        if (const std::string error = ValidateCxxVTableDeleteDemand(typeName); !error.empty())
        {
            LogError(error);
            return true;
        }
        AbiRecipe recipe;
        llvm::Function* proto = GetOrCreateCxxStructor(typeName, info->destructor, recipe);
        if (proto == nullptr) return false;
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        llvm::Value* vptr = builder->CreateLoad(ptrTy, ptr, "vtable");
        llvm::Value* slot = builder->CreateGEP(ptrTy, vptr,
                                               builder->getInt64(info->dtorDeletingVtableIndex),
                                               "vdel.slot");
        llvm::Value* fn = builder->CreateLoad(ptrTy, slot, "vdel");
        // The deleting destructor destroys the object AND releases its storage through the C++
        // deallocator, so no separate destructor call and no operator delete may follow. On
        // Itanium it has the complete-object destructor's signature; the Microsoft scalar
        // deleting destructor takes a flags word (bit 0 = release the storage) and returns this.
        if (targetWindows_)
        {
            auto* deletingTy = llvm::FunctionType::get(
                ptrTy, { ptrTy, builder->getInt32Ty() }, /*isVarArg*/ false);
            builder->CreateCall(deletingTy, fn, { ptr, builder->getInt32(1) });
        }
        else
            builder->CreateCall(proto->getFunctionType(), fn, { ptr });
        return true;
    }

// A specialization's CFlat identity is a MANGLED key ('std.vector$.p$.p$nest.Cell'), which is
// not writable source. Every user-facing message spells it back out.
/*
 * The namespace-scope operators of an in-scope header are harvested when it is walked; a friend
 * operator may not be (a record requested on demand publishes only its comparisons). So only a
 * specialization, an out-of-scope class, a class declaring a friend operator, or a class with a
 * base (whose namespace and friends ADL also searches) can hide one.
 */
bool LLVMBackend::CxxClassMayHaveUnregisteredOperators(const std::string& typeName) const
{
        if (typeName.find('$') != std::string::npos || typeName.find('<') != std::string::npos
            || generatedCxxRecords_.count(typeName) != 0)
            return true;
        auto it = cxxRecordEntries_.find(typeName);
        // A base class's namespace and hidden friends are associated too (ADL); stay conservative.
        return it == cxxRecordEntries_.end() || !it->second.inScope
            || it->second.hasFriendOperators || it->second.hasBases || it->second.hasVirtualBases;
}

// A class declaring a friend operator publishes only its homogeneous comparisons; a mixed-type
// friend (`operator==(const error_code&, const error_condition&)`) is found by ADL alone.
bool LLVMBackend::CxxClassDeclaresFriendOperators(const std::string& typeName) const
{
        auto it = cxxRecordEntries_.find(typeName);
        return it != cxxRecordEntries_.end() && it->second.hasFriendOperators;
}

std::string LLVMBackend::CxxDeletedOperatorError(const std::string& typeName,
                                                 const std::string& operatorName) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return {};
        const std::string name = "operator" + operatorName;
        if (std::any_of(record->second.members.begin(), record->second.members.end(),
                        [&](const auto& member) {
                            return member.name == name && member.isDeleted;
                        }))
            return "clang: overload resolution selected deleted operator '" + operatorName + "'";
        return {};
}

bool LLVMBackend::CxxHasDeletedAssignment(const std::string& typeName, bool useMove) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return false;
        return std::any_of(record->second.members.begin(), record->second.members.end(),
                           [&](const auto& member) {
            return member.isDeleted && (useMove ? member.isMoveAssign : member.isCopyAssign);
        });
}

// The operator=(T&&) clang picks for any non-const rvalue when an lvalue destination can use it: a
// deleted, not defaulted, non-const T&& with no `&&` ref-qualifier. A defaulted move that a member
// deletes is ignored by overload resolution, so it is not counted.
bool LLVMBackend::CxxHasDeletedRvalueAssignment(const std::string& typeName) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return false;
        return std::any_of(record->second.members.begin(), record->second.members.end(),
                           [&](const auto& member) {
            return member.isMoveAssign && member.isDeleted && !member.isDefaulted
                && (member.refQualifier == cflat_cinterop::CxxRefQualifierNone
                    || member.refQualifier == cflat_cinterop::CxxRefQualifierLValue)
                && member.paramTypes.size() > 1 && member.paramTypes[1].ends_with("&&")
                && !member.paramTypes[1].starts_with("const ");
        });
}

// Top-level cv on a by-value parameter spelling ("const T", "T const") does not change its type.
static std::string StripTopLevelCvSpelling(std::string text)
{
        for (bool changed = true; changed;)
        {
            changed = false;
            for (const char* q : { "const ", "volatile " })
                if (text.rfind(q, 0) == 0) { text.erase(0, std::strlen(q)); changed = true; }
            for (const char* q : { " const", " volatile" })
            {
                const size_t n = std::strlen(q);
                if (text.size() > n && text.compare(text.size() - n, n, q) == 0)
                { text.erase(text.size() - n); changed = true; }
            }
        }
        return text;
}

// A usable operator=(T) taking the class by value: it ties with operator=(T&&) for a prvalue.
bool LLVMBackend::CxxHasByValueAssignment(const std::string& typeName) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return false;
        const std::string spelling = CxxRecordCanonicalSpelling(typeName);
        return std::any_of(record->second.members.begin(), record->second.members.end(),
                           [&](const auto& member) {
            // A by-value parameter is recorded as a copy assignment, so only the spelling decides.
            return member.name == "operator=" && !member.isDeleted && !member.isMoveAssign
                && member.paramTypes.size() == 2
                && StripTopLevelCvSpelling(member.paramTypes[1]) == spelling
                && (member.refQualifier == cflat_cinterop::CxxRefQualifierNone
                    || member.refQualifier == cflat_cinterop::CxxRefQualifierLValue);
        });
}

std::string LLVMBackend::DisplayCxxClassName(const std::string& typeName) const
{
        TypeAndValue probe;
        probe.TypeName = typeName;
        return SpellType(*this, probe);
    }

bool LLVMBackend::RejectInaccessibleCxxMember(const std::string& typeName,
                                              const std::string& memberName,
                                              bool accessedThroughCurrentObject,
                                              bool deferElementCopySink,
                                              bool protectedOnly)
{
        EnsureCxxMemberProjected(typeName, memberName);
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return false;
        // A member of a class whose LAYOUT cflat could not reproduce reports the class, not the
        // member: nothing about such a type is bindable, so naming the reason is the useful answer.
        if (!info->layoutRefusal.empty()
            && (info->fieldAccess.count(memberName) != 0 || info->memberAccess.count(memberName) != 0))
            return RejectUnsupportedCxxLayout(typeName);
        if (auto f = info->fieldAccess.find(memberName); f != info->fieldAccess.end())
        {
            if (info->ambiguousFieldNames.count(memberName) != 0)
            {
                const std::string className = DisplayCxxClassName(typeName);
                LogError(std::format(
                    "member '{}' is ambiguous in C++ class '{}': found in more than one base class "
                    "(add a 'using' declaration in '{}' or qualify through a base)",
                    memberName, className, className));
                return true;
            }
            if (f->second == cflat_cinterop::AccessPublic) return false;
            if (f->second == cflat_cinterop::AccessProtected
                && CxxProtectedAccessAllowed(typeName, accessedThroughCurrentObject)) return false;
            // An inherited field behind a private/protected base: name the base, as clang does.
            if (ReportCxxMemberThroughNonPublicBase(typeName, memberName)) return true;
            LogError(std::format("field '{}' of C++ class '{}' is {}",
                                 memberName, DisplayCxxClassName(typeName),
                                 f->second == cflat_cinterop::AccessPrivate ? "private" : "protected"));
            return true;
        }
        if (auto ma = info->memberAccess.find(memberName); ma != info->memberAccess.end()
            && ma->second == cflat_cinterop::AccessProtected)
        {
            if (CxxProtectedAccessAllowed(typeName, accessedThroughCurrentObject)) return false;
            if (ReportCxxMemberThroughNonPublicBase(typeName, memberName)) return true;
            LogErrorMessage("member '{}' of C++ class '{}' is protected",
                            { memberName, DisplayCxxClassName(typeName) });
            return true;
        }
        if (protectedOnly) return false;
        if (auto m = info->refusedMembers.find(memberName); m != info->refusedMembers.end())
        {
            const std::string refusal = m->second;
            if (TryBindRefusedCxxMember(typeName, memberName)) return false;
            /*
             * A refusal only speaks when NO overload of that name was bound. A libc++ class
             * commonly has one bindable overload and one cflat cannot express (`append(const
             * char*)` next to `append(initializer_list<char>)`); reporting the refused one would
             * make the callable overload unreachable.
             */
            const CxxClassInfo* currentInfo = GetCxxClassInfo(typeName);
            if (currentInfo != nullptr
                && std::find(currentInfo->instanceMethodNames.begin(),
                             currentInfo->instanceMethodNames.end(), memberName)
                       != currentInfo->instanceMethodNames.end())
                return false;
            if (functionTable.count(typeName + "." + memberName) != 0) return false;   // static
            // The call site names the deleted element copy once it has seen the argument.
            if (deferElementCopySink && IsCxxElementCopySinkRefusal(typeName, memberName))
                return false;
            if (refusal == "is deleted")
                LogError("clang: attempt to use a deleted function");
            LogError(std::format("member '{}' of C++ class '{}' {}", memberName,
                                 DisplayCxxClassName(typeName), refusal));
            return true;
        }
        // An inherited member refused on its base (a lazy std::function signature) binds here.
        if (info->hasBases && !FindCxxBaseMethods(typeName, memberName).empty())
            TryBindRefusedCxxBaseMember(typeName, memberName);
        return false;
    }

/*
 * `struct D : private B` with a public `B::K`: C++ finds K and refuses the ACCESS, so "'K' is not
 * a member" is wrong. Called only where a use of `memberName` on `typeName` already failed; logs
 * the inaccessible-base diagnostic and returns true when the name is declared in a base reached
 * only through a private or protected base-specifier.
 */
std::string LLVMBackend::CxxMemberThroughNonPublicBaseNote(const std::string& typeName,
                                                           const std::string& memberName)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || info->bases.empty()) return {};
        auto declares = [&](const std::string& owner) {
            EnsureCxxMemberProjected(owner, memberName);
            const CxxClassInfo* ownerInfo = GetCxxClassInfo(owner);
            if (ownerInfo == nullptr) return false;
            // Declared BY owner: the access maps also hold inherited entries, which must not hide
            // the base that declares them.
            if (auto record = cxxRecordEntries_.find(owner); record != cxxRecordEntries_.end())
            {
                const CRecordEntry& r = record->second;
                if (std::any_of(r.fields.begin(), r.fields.end(), [&](const CRecordFieldEntry& f) {
                        return !f.inherited && f.name == memberName; })
                    || std::any_of(r.members.begin(), r.members.end(), [&](const auto& m) {
                        return m.name == memberName; })
                    || std::any_of(r.staticVars.begin(), r.staticVars.end(), [&](const auto& v) {
                        return v.name == memberName; }))
                    return true;
            }
            if (std::any_of(ownerInfo->directMethods.begin(), ownerInfo->directMethods.end(),
                            [&](const CxxClassInfo::Method& m) { return m.raw.name == memberName; }))
                return true;
            if (std::find(ownerInfo->staticMethodNames.begin(), ownerInfo->staticMethodNames.end(),
                          memberName) != ownerInfo->staticMethodNames.end())
                return true;
            const std::string qualified = owner + "." + memberName;
            return GetGlobalVariableNV(qualified).Storage != nullptr || GetFunction(qualified) != nullptr;
        };
        // First base-specifier on the path that is not public: (derived, base, access).
        struct Blocked { std::string derived, base; int access; };
        std::optional<Blocked> found;
        bool publicPath = false;
        std::unordered_set<std::string> visited;
        std::function<void(const std::string&, const std::optional<Blocked>&)> walk =
            [&](const std::string& current, const std::optional<Blocked>& blocked) {
            if (publicPath || !visited.insert(current).second) return;
            const CxxClassInfo* currentInfo = GetCxxClassInfo(current);
            if (currentInfo == nullptr) return;
            for (const CxxClassInfo::BaseRef& base : currentInfo->bases)
            {
                const std::string baseName = ResolveCxxBaseIdentity(base);
                if (baseName.empty()) continue;
                std::optional<Blocked> next = blocked;
                if (!next && base.access != cflat_cinterop::AccessPublic)
                    next = Blocked{ current, baseName, base.access };
                if (declares(baseName))
                {
                    if (!next) publicPath = true;
                    else if (!found) found = next;
                    continue;   // the nearest declaration hides anything further up
                }
                walk(baseName, next);
            }
        };
        // A declaration directly in typeName hides every base declaration of the name, so the
        // failure is the derived member's own (private field, unmatched overload), not the base.
        if (declares(typeName)) return {};
        walk(typeName, std::nullopt);
        if (publicPath || !found) return {};
        return std::format("member '{}' of C++ class '{}' is inaccessible: '{}' is a {} base of '{}'",
                           memberName, DisplayCxxClassName(typeName),
                           DisplayCxxClassName(found->base),
                           found->access == cflat_cinterop::AccessPrivate ? "private" : "protected",
                           DisplayCxxClassName(found->derived));
    }

bool LLVMBackend::ReportCxxMemberThroughNonPublicBase(const std::string& typeName,
                                                      const std::string& memberName)
{
        std::string note = CxxMemberThroughNonPublicBaseNote(typeName, memberName);
        if (note.empty()) return false;
        LogError(note);
        return true;
    }

bool LLVMBackend::CxxProtectedAccessAllowed(const std::string& typeName,
                                            bool accessedThroughCurrentObject) const
{
        if (!accessedThroughCurrentObject || cppStructAccessContext_.empty()) return false;
        uint64_t offset = 0;
        bool inaccessible = false;
        return FindCxxBaseOffset(cppStructAccessContext_, typeName, offset, inaccessible)
            && !inaccessible;
    }

int LLVMBackend::CxxSelectedMethodAccess(const std::string& typeName, const std::string& linkageName,
                                         std::set<std::string>& visited) const
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || !visited.insert(typeName).second) return -1;
        for (const auto& method : info->directMethods)
            if (!linkageName.empty() && method.raw.linkageName == linkageName) return method.raw.access;
        for (const auto& base : info->bases)
            if (const int access = CxxSelectedMethodAccess(base.name, linkageName, visited); access >= 0)
                return access;
        return -1;
    }

size_t LLVMBackend::CxxBaseSubobjectCount(const std::string& derivedType,
                                          const std::string& baseType) const
{
        if (derivedType.empty() || baseType.empty()) return 0;
        const CxxClassInfo* derived = GetCxxClassInfo(derivedType);
        if (derived == nullptr || !derived->layoutRefusal.empty()) return 0;
        std::unordered_set<std::string> subobjects;
        std::vector<std::string> active;
        std::function<void(const std::string&, const std::string&)> visit =
            [&](const std::string& current, const std::string& identity) {
            if (current == baseType)
            {
                subobjects.insert(identity);
                return;
            }
            if (std::find(active.begin(), active.end(), current) != active.end()) return;
            const CxxClassInfo* info = GetCxxClassInfo(current);
            if (info == nullptr || !info->layoutRefusal.empty()) return;
            active.push_back(current);
            for (const CxxClassInfo::BaseRef& base : info->bases)
            {
                const std::string name = ResolveCxxBaseIdentity(base);
                if (name.empty()) continue;
                // Repeated routes to a virtual base share one subobject; later non-virtual
                // edges still distinguish separate descendants within that virtual base.
                const std::string nextIdentity = base.isVirtual
                    ? "virtual:" + name
                    : identity + "/" + current + "->" + name;
                visit(name, nextIdentity);
            }
            active.pop_back();
        };
        visit(derivedType, "object:" + derivedType);
        return std::min<size_t>(2, subobjects.size());
}

std::string LLVMBackend::CxxInheritedMemberOwner(const std::string& receiverType,
                                                  const FunctionSymbol& candidate) const
{
        if (!candidate.IsCxx || !candidate.IsMethod || candidate.UniqueName.empty()) return {};
        auto declaresCandidate = [&](const std::string& owner) {
            const CxxClassInfo* info = GetCxxClassInfo(owner);
            return info != nullptr
                && std::any_of(info->directMethods.begin(), info->directMethods.end(),
                    [&](const CxxClassInfo::Method& method) {
                        return !method.raw.isUsingShadow
                            && method.raw.linkageName == candidate.UniqueName;
                    });
        };
        if (!candidate.CxxInheritedOwner.empty()
            && candidate.CxxInheritedOwner != receiverType
            && declaresCandidate(candidate.CxxInheritedOwner))
            return candidate.CxxInheritedOwner;
        const CxxClassInfo* receiver = GetCxxClassInfo(receiverType);
        const bool usingCandidate = receiver != nullptr
            && std::any_of(receiver->directMethods.begin(), receiver->directMethods.end(),
                [&](const CxxClassInfo::Method& method) {
                    return method.raw.isUsingShadow
                        && method.raw.linkageName == candidate.UniqueName;
                });
        if (!usingCandidate) return {};
        for (const auto& [owner, info] : cxxClasses_)
            if (owner != receiverType && IsCxxBaseOf(owner, receiverType)
                && declaresCandidate(owner))
                return owner;
        return {};
}

/*
 * Publish one imported C++ class's callable surface.
 *
 * Instance methods land in functionTable under their BARE name with 'this' as the first
 * parameter, which is exactly the shape CFlat's own struct methods have - so `obj.method(a)` and
 * `ptr.method(a)` dispatch through the existing member-call path with no new lowering. Static
 * methods and out-of-line static data members are published under the dotted type name
 * ("ns.Class.member"), the same spelling the free-function path already uses for namespaces.
 * Constructors and the destructor are NOT user-callable names; they go into CxxClassInfo, which
 * lifetime codegen reads.
 *
 * Nothing is refused silently: a member CFlat cannot bind (virtual, deleted, inaccessible, only
 * inline-defined in the header) is recorded in refusedMembers so the use site can say why.
 */
// Record <-> C++ spelling maps for one imported record (forward map first-wins).
void LLVMBackend::RememberCxxRecordSpellings(const CRecordEntry& r)
{
        if (!r.canonicalCtype.empty())
        {
            std::string spelling = r.canonicalCtype;
            for (const char* tag : { "class ", "struct ", "union ", "enum " })
                if (spelling.rfind(tag, 0) == 0) { spelling.erase(0, strlen(tag)); break; }
            if (!spelling.empty() && cxxCflatToCxxSpelling_.count(r.name) == 0)
                cxxCflatToCxxSpelling_[r.name] = spelling;
            // Reverse direction: clang spells this class canonically in every member signature it
            // appears in (`push_back(const cppi::Tracked&)` inside a requested specialization), so
            // the C type mapper needs the spelling -> CFlat identity entry as well. Both the
            // tagged and untagged forms, since a signature may carry either.
            auto rememberSpelling = [&](const std::string& key) {
                auto found = cxxForeignTypeSpellings_.find(key);
                if (found == cxxForeignTypeSpellings_.end() || found->second == r.name)
                {
                    cxxForeignTypeSpellings_[key] = r.name;
                    return;
                }
                // A header walk can register several complete specializations under the same
                // argument-less shell. Once the exact '$' identity is available, prefer it for
                // this specialization unless the shell already spells the same arguments (the
                // intentional std.vector placeholder alias path).
                const size_t separator = r.name.find('$');
                if (separator == std::string::npos
                    || found->second != r.name.substr(0, separator)) return;
                auto shell = cxxCflatToCxxSpelling_.find(found->second);
                if (shell == cxxCflatToCxxSpelling_.end()
                    || StripCxxRecordTag(shell->second) != StripCxxRecordTag(spelling))
                    found->second = r.name;
            };
            rememberSpelling(SqueezeCxxSpelling(r.canonicalCtype));
            if (!spelling.empty()) rememberSpelling(SqueezeCxxSpelling(spelling));
        }
    }

void LLVMBackend::RegisterCxxClassMembers(const CRecordEntry& r, const std::string& fileForLsp,
                                          const std::string& memberFilter)
{
        if (!r.isCxx) return;
        const bool metadataOnly = memberFilter == "__metadata";
        StoreCxxRecordEntry(r.name, r);
        std::vector<bool> constrainedTwinMembers(r.members.size(), false);
        auto hasDefaults = [](const cflat_cinterop::RawCxxMember& member) {
            return std::any_of(member.defaultArgs.begin(), member.defaultArgs.end(),
                [](const cflat_cinterop::RawDefaultArg& arg) { return !arg.kind.empty(); });
        };
        auto sameDefaults = [](const auto& a, const auto& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].kind != b[i].kind || a[i].value != b[i].value) return false;
            return true;
        };
        for (size_t i = 0; i < r.members.size(); ++i)
        {
            const auto& a = r.members[i];
            if ((a.kind != cflat_cinterop::RawCxxMember::Instance
                    && a.kind != cflat_cinterop::RawCxxMember::StaticMethod)
                || a.isDeleted || a.variadic || a.linkageName.empty()
                || a.defaultArgs.size() != a.paramTypes.size()) continue;
            for (size_t j = i + 1; j < r.members.size(); ++j)
            {
                const auto& b = r.members[j];
                if (a.kind != b.kind || a.name != b.name || a.paramTypes != b.paramTypes
                    || a.retType != b.retType || a.isConst != b.isConst
                    || a.isVolatile != b.isVolatile || a.refQualifier != b.refQualifier
                    || a.isNoexcept != b.isNoexcept || a.isDeleted || b.isDeleted
                    || b.variadic || b.linkageName.empty()
                    || b.defaultArgs.size() != b.paramTypes.size()
                    || (a.access != cflat_cinterop::AccessPublic
                        || b.access != cflat_cinterop::AccessPublic)
                    || (a.linkageName == b.linkageName && a.line == b.line
                        && a.col == b.col && sameDefaults(a.defaultArgs, b.defaultArgs))
                    || (!hasDefaults(a) && !hasDefaults(b))) continue;
                constrainedTwinMembers[i] = true;
                constrainedTwinMembers[j] = true;
            }
        }
        for (const auto& member : r.members)
            if ((member.kind == cflat_cinterop::RawCxxMember::Instance
                    || member.kind == cflat_cinterop::RawCxxMember::StaticMethod)
                && member.access == cflat_cinterop::AccessPublic
                && !member.linkageName.empty()
                && member.defaultArgs.size() == member.paramTypes.size())
                for (size_t n = 0; n < member.defaultArgs.size(); ++n)
                    if (member.defaultArgs[n].kind != "unsupported"
                        && HasNonConstDefaultSuffix(member.defaultArgs, n))
                        RememberCxxDefaultWrapper(r.name, member.linkageName, n, true);
        // A class whose layout was REFUSED still needs its CxxClassInfo: that is where the refusal
        // text and the field list the diagnostic reads from live.
        // Empty C++ classes still have constructors/destructors and are valid foreign types.
        // Keep the initial header walk bounded; requested specializations carry a '$' in their
        // CFlat identity and are retained even when Clang reports no callable members.
        // Registered BEFORE the memberless early-out below: a plain aggregate with no members
        // of its own is still named by other signatures (`const cppi::RevOther&`), and without
        // this entry the type mapper cannot resolve that spelling and refuses the member.
        // A class imported from a C++ HEADER group also gets a C++ spelling, so it can be a
        // template argument (`cppt.Box<cppi.Tracked>`). A request already recorded its own
        // spelling, which is the one the request TU was built with - never overwrite it.
        // The REVERSE map is keyed by the full canonical spelling, so it is populated for every
        // record even when several records share one CFlat identity. Class template
        // specializations do: clang's qualified name for a specialization decl drops the
        // arguments, so every torch::nn::Cloneable<T> registers as 'torch.nn.Cloneable'. Only
        // the forward map (identity -> spelling) is first-wins.
        RememberCxxRecordSpellings(r);
        if (!metadataOnly && r.members.empty() && r.staticVars.empty() && r.layoutRefusal.empty()
            && r.name.find('$') == std::string::npos && !IsCxxForeignTypeRegistered(r.name)) return;

        CxxClassInfo info;
        std::set<std::string> alreadyRegisteredConstructorLinkages;
        if (!memberFilter.empty() && !metadataOnly)
        {
            auto existing = cxxClasses_.find(r.name);
            if (existing == cxxClasses_.end()) return;
            info = existing->second;
            if (memberFilter == "__ctor" || memberFilter == "__copy_move_ctor")
                for (const auto& ctor : info.constructors)
                    alreadyRegisteredConstructorLinkages.insert(ctor.linkageName);
        }
        else
        {
            info.isPolymorphic         = r.isPolymorphic;
            info.vtableDeleteVerdict   = r.vtableDeleteVerdict;
            info.hasBases              = r.hasBases;
            info.hasVirtualBases       = r.hasVirtualBases;
            info.isFinal               = r.isFinal;
            info.isAbstract            = r.isAbstract;
            info.layoutRefusal         = r.layoutRefusal;
            for (const auto& b : r.bases)
            {
                CxxClassInfo::BaseRef br;
                br.name = b.name; br.canonicalType = b.canonicalType;
                br.offsetBytes = b.offsetBytes; br.access = b.access;
                br.isVirtual = b.isVirtual;
                info.bases.push_back(std::move(br));
            }
            for (const auto& b : r.virtualBases)
            {
                CxxClassInfo::BaseRef br;
                br.name = b.name; br.canonicalType = b.canonicalType;
                br.offsetBytes = b.offsetBytes; br.access = b.access;
                br.isVirtual = true;
                info.virtualBases.push_back(std::move(br));
            }
            info.hasTrivialDefaultCtor = r.hasTrivialDefaultCtor;
            info.hasTrivialCopyCtor    = r.hasTrivialCopyCtor;
            info.hasTrivialCopyAssign  = r.hasTrivialCopyAssign;
            info.hasTrivialMoveAssign  = r.hasTrivialMoveAssign;
            info.hasTrivialDtor        = r.hasTrivialDtor;
            info.isTriviallyRelocatable = r.isTriviallyRelocatable;
            info.paramDestroyedInCallee = r.paramDestroyedInCallee;
            info.hasDeletedDefaultCtor = r.hasDeletedDefaultCtor;
            info.hasDeletedCopyCtor    = r.hasDeletedCopyCtor;
            info.hasDefaultCtor        = r.hasDefaultCtor;
            info.hasCopyCtor           = r.hasCopyCtor;
            info.hasCtorTemplate       = r.hasCtorTemplate;
            info.hasAssignTemplate     = r.hasAssignTemplate;
            info.moveAssignBindsConstRef = r.moveAssignBindsConstRef;
            info.moveAssignConstSourceBindsConstRef = r.moveAssignConstSourceBindsConstRef;
            info.isAggregate           = r.isAggregate;
            info.ambiguousFieldNames.insert(r.ambiguousFieldNames.begin(),
                                            r.ambiguousFieldNames.end());
            for (const auto& f : r.fields)
            {
                if (f.name.empty()) continue;
                info.fieldAccess[f.name] = f.access;
            }
            for (const auto& member : r.members)
            {
                if (member.kind != cflat_cinterop::RawCxxMember::Instance
                    && member.kind != cflat_cinterop::RawCxxMember::StaticMethod)
                    continue;
                auto access = info.memberAccess.find(member.name);
                if (access == info.memberAccess.end()) info.memberAccess[member.name] = member.access;
                else if (member.access < access->second) access->second = member.access;
            }
            for (const auto& variable : r.staticVars)
                info.memberAccess[variable.name] = variable.access;
        }
        if (metadataOnly)
        {
            cxxClasses_[r.name] = std::move(info);
            return;
        }

        // A member's declared type, mapped through the shared C spelling mapper. A pointer to a
        // KNOWN aggregate keeps its pointee (the mapper decays struct pointers to void*), which is
        // what makes a method returning `Other*` chain into `Other`'s own members.
        auto mapType = [&](const std::string& spelling, TypeAndValue& tv,
                           bool isReturn = false) -> bool {
            std::vector<uint64_t> dims;
            std::string elem = StripFixedArrayDims(spelling, dims);
            if (!MapCTypeToTypeAndValue(elem, tv, true)) return false;
            if (!tv.IsFunctionPointer && tv.Pointer && tv.TypeName == "void")
            {
                int ptrLevels = 0;
                std::string tag = AggregatePointeeTag(elem, ptrLevels);
                if (!tag.empty() && ptrLevels <= 2 && dataStructures.find(tag) != dataStructures.end())
                    tv.TypeName = tag;
            }
            if (!dims.empty())
            {
                tv.ConstArraySize = dims[0];
                tv.ConstInnerDimensions.assign(dims.begin() + 1, dims.end());
            }
            if (isReturn && !tv.Pointer && !tv.IsAlias && !tv.IsFunctionPointer
                && IsCxxRecord(tv.TypeName) && CxxSpellingHasTopLevelConst(spelling))
                tv.IsCxxConstValueReturn = true;
            return true;
        };

        /*
         * A `std::function<R(Args)>` member parameter or return is its requested std.function
         * specialization - the same mapping RegisterCSignatures gives a FREE function's
         * std::function parameter. It has to run BEFORE mapType, which answers void* for an
         * unknown class reference and would bind a parameter of the wrong shape.
         */
        auto mapStdFunction = [&](const std::string& spelling, bool isReturn, bool structorParam,
                                  TypeAndValue& tv, bool ownNamedMethod = false) {
            // std.function's OWN members (its copy/move constructors and assignment) keep the
            // shapes the specialization was registered with; retyping them here would rewrite
            // the surface the cast and copy paths already resolve against. A named method
            // (`swap(function&)`) has no such shape and maps like any other member.
            if (IsStdFunctionSpecialization(r.name) && !ownNamedMethod) return false;
            const std::string className = StdFunctionSpecializationForSpelling(spelling);
            if (className.empty()) return false;
            CxxReferenceKind refKind = CxxReferenceKind::None;
            CxxSpellingWithoutRef(spelling, &refKind);
            tv = TypeAndValue{};
            tv.TypeName = className;
            // A STRUCTOR's reference parameter keeps the pointer shape the construct-into-slot
            // path expects (markConstructorReference marks the lvalue one); a by-value one, and
            // every non-structor member parameter, crosses as the class the free path passes.
            if (structorParam && refKind != CxxReferenceKind::None) tv.Pointer = true;
            else tv.IsAlias = !isReturn || refKind == CxxReferenceKind::Lvalue;
            return true;
        };

        using Member = cflat_cinterop::RawCxxMember;

        // const/non-const overload pair: CFlat drops const, so both spell the same CFlat
        // signature. RULING: an lvalue prefers the non-const overload, and the const one is bound
        // only when it is the sole candidate. Decide that here, once, by signature key.
        // The key is the signature CFlat SEES: const is dropped, and `const T&` / `T&` / `T&&`
        // all lvalue references collapse onto `alias T`; T&& keeps a distinct rvalue-alias key.
        /*
         * A conversion function is published under the CFlat spelling of its target, which is the
         * name the explicit-cast path looks up ("operator int", "operator double"). A target the
         * type map cannot express keeps the C++ source name: CFlat has no way to spell that cast,
         * so no use site can reach the entry - it exists only as a recorded refusal.
         */
        auto memberRegName = [&](const Member& m) -> std::string {
            if (!m.isConversion) return m.name;
            TypeAndValue convRet;
            if (!mapType(m.retType, convRet)) return m.name;
            std::string spelling = convRet.IsPrimitive() && !convRet.Pointer
                ? CanonicalPrimitiveTypeName(ResolveTypeAlias(convRet.TypeName))
                : SpellType(*this, convRet);
            return spelling.empty() ? m.name : "operator " + spelling;
        };
        auto cflatSigKey = [&](const Member& m) {
            std::string key = memberRegName(m);
            key += "|thisrq=" + std::to_string(m.refQualifier);
            for (size_t p = 1; p < m.paramTypes.size(); ++p)
            {
                CxxReferenceKind refKind = CxxReferenceKind::None;
                std::string t = CxxSpellingWithoutRef(m.paramTypes[p], &refKind);
                // Top-level const of a by-value parameter is not part of the signature; the
                // referent const of `T&` / `T&&` is (`f(R&&)` and `f(const R&&)` both rank).
                const bool constReferent = t.rfind("const ", 0) == 0;
                if (constReferent) t = t.substr(6);
                if (constReferent && (refKind == CxxReferenceKind::Lvalue
                                      || refKind == CxxReferenceKind::Rvalue))
                    t += "|const-referent";
                // CFlat drops pointee cv from its surface type, but C++ distinguishes `T*`
                // from `const T*` overloads. Keep both in the candidate set for C++ ranking.
                if (CxxSpellingHasConstPointee(m.paramTypes[p])) t += "|const-pointee";
                if (CxxSpellingHasVolatileTarget(m.paramTypes[p])) t += "|volatile-pointee";
                // A non-const lvalue `T&` and a by-value `T` share one CFlat surface type, but C++
                // ranks them apart (clang: ambiguous for an lvalue), so both stay in the set.
                if (refKind == CxxReferenceKind::Lvalue && !constReferent) t += "|lref";
                if (refKind == CxxReferenceKind::Rvalue) t += "|rvalue";
                else if (refKind == CxxReferenceKind::RefToPointer)
                {
                    // `T*&` and `T*const&` are distinct overloads once the cv under the ref is gone.
                    t += CxxParamIsConstRefToPointer(m.paramTypes[p]) ? "|constrefptr" : "|refptr";
                }
                key += "|" + t;
            }
            // `f(int)` and `f(int, ...)` are distinct overloads (both reach C++ ranking).
            if (m.variadic) key += "|va";
            return key;
        };
        using TwinWrapperKey = std::pair<std::string, size_t>;
        std::map<TwinWrapperKey, size_t> twinWrapperSourceByArity;
        auto hasDefaultSuffix = [](const Member& member, size_t first) {
            if (first >= member.defaultArgs.size()) return false;
            for (size_t i = first; i < member.defaultArgs.size(); ++i)
                if (member.defaultArgs[i].kind.empty()) return false;
            return true;
        };
        for (size_t i = 0; i < r.members.size(); ++i)
        {
            if (!constrainedTwinMembers[i]) continue;
            const Member& member = r.members[i];
            const std::string key = cflatSigKey(member);
            for (size_t n = 0; n < member.paramTypes.size(); ++n)
                if (hasDefaultSuffix(member, n))
                    twinWrapperSourceByArity.try_emplace({ key, n }, i);
            twinWrapperSourceByArity.try_emplace({ key, member.paramTypes.size() }, i);
        }
        for (const auto& [key, i] : twinWrapperSourceByArity)
        {
            const Member& member = r.members[i];
            RememberCxxDefaultWrapper(r.name, member.linkageName, key.second, true);
        }
        auto directMethodKey = [](const Member& m) {
            std::string key = m.name + "|" + m.retType + (m.isConst ? "|const" : "|mut")
                + (m.isVolatile ? "|volatile" : "")
                + "|rq=" + std::to_string(m.refQualifier);
            for (const auto& param : m.paramTypes) key += "|" + param;
            return key;
        };
        auto recordDirectMethod = [&](const Member& m) {
            if (m.kind != Member::Instance || m.isCopyAssign || m.isMoveAssign) return;
            const std::string key = directMethodKey(m);
            for (const auto& existing : info.directMethods)
                if (directMethodKey(existing.raw) == key) return;

            CxxClassInfo::Method method;
            method.ownerType = r.name;
            method.raw = m;
            TypeAndValue ret;
            if (!mapType(m.retType, ret, true))
            {
                method.spellable = false;
                info.directMethods.push_back(std::move(method));
                return;
            }
            std::vector<TypeAndValue> params;
            params.reserve(m.paramTypes.size());
            for (size_t p = 0; p < m.paramTypes.size(); ++p)
            {
                TypeAndValue tv;
                if (p == 0)
                {
                    tv.TypeName = r.name;
                    tv.Pointer = true;
                }
                else if (!mapType(m.paramTypes[p], tv))
                {
                    method.spellable = false;
                    info.directMethods.push_back(std::move(method));
                    return;
                }
                if (p != 0 && tv.IsFunctionPointer && tv.Pointer
                    && (tv.IsAlias || tv.IsRvalueRef))
                {
                    method.spellable = false;
                    info.directMethods.push_back(std::move(method));
                    return;
                }
                params.push_back(std::move(tv));
            }
            method.ret = ret;
            method.params = std::move(params);
            info.directMethods.push_back(std::move(method));
        };
        auto markConstructorReference = [&](const std::string& spelling, TypeAndValue& tv) {
            if (tv.IsFunctionPointer && tv.Pointer && (tv.IsAlias || tv.IsRvalueRef))
                tv.IsCxxRefToPointer = true;
            if (tv.Pointer && tv.TypeName == "void")
            {
                TypeAndValue remapped;
                bool representable = false;
                if (TryMapCxxForeignSpelling(spelling, remapped, representable) && representable)
                    tv.TypeName = remapped.TypeName;
                int ptrLevels = 0;
                const std::string tag = CxxRecordPointeeTag(spelling, ptrLevels);
                if (ptrLevels == 1
                    && (IsCxxRecord(tag) || IsCxxLazyAliasSpecialization(tag)))
                    tv.TypeName = tag;
            }
            CxxReferenceKind refKind = CxxReferenceKind::None;
            CxxSpellingWithoutRef(spelling, &refKind);
            if (refKind == CxxReferenceKind::RefToPointer && tv.Pointer
                && !tv.IsFunctionPointer)
            {
                if (tv.ElemPointer && !tv.IsCxxRefToPointer) tv.ElemPointer = false;
                tv.IsCxxRefToPointer = true;
                tv.IsAlias = true;
                tv.MarkCxxRef();
                tv.SetCxxConstRef(CxxParamIsConstRefToPointer(spelling));
                tv.ConstMask |= CxxSpellingPointeeConstMask(spelling, tv.ValuePointerDepth());
            }
            if (refKind == CxxReferenceKind::Lvalue && tv.Pointer
                && !tv.ElemPointer && !tv.IsFunctionPointer)
            {
                tv.IsAlias = true;
                tv.MarkCxxRef();
                tv.SetCxxConstRef(CxxParamIsConstLvalueReference(spelling));
            }
        };
        /*
         * Does this overload have a symbol cflat could call at all? CFlat drops `volatile` the way
         * it drops `const`, so libc++'s `volatile` and non-volatile twins (atomic's load / store /
         * exchange / fetch_add) collapse onto one CFlat signature - and only the twin that was
         * ODR-used has a body, the other losing its linkage name in the extractor. Electing the
         * bodiless one would refuse the member outright, so bindability outranks every other
         * preference below. Mirrors the per-member rejects in the registration loop that depend on
         * the overload rather than on its class.
         */
        auto hasReachableSymbol = [](const Member& m) {
            return m.bindRefusal.empty() && !m.isDeleted
                && !m.requiresConstructorWrapper && !m.needsLocalDefinition
                && m.abi.valid && !m.linkageName.empty();
        };
        std::map<std::string, size_t> instanceBySig;
        for (size_t i = 0; i < r.members.size(); ++i)
        {
            const Member& m = r.members[i];
            // Copy and move assignment are captured by FLAG, not by CFlat signature, and they
            // collapse onto the same key (`const T&` and `T&&` both drop to `T`) - deduping them
            // here would delete the move leg of every class that has one.
            if (m.kind != Member::Instance || m.isCopyAssign || m.isMoveAssign) continue;
            const std::string key = cflatSigKey(m);
            auto it = instanceBySig.find(key);
            if (it == instanceBySig.end()) { instanceBySig[key] = i; continue; }
            const Member& kept = r.members[it->second];
            // Prefer a non-const method over its const twin, and an lvalue-reference
            // parameterization over the rvalue-reference one (a const T& overload accepts both an
            // lvalue and a materialized temporary; a T&& overload accepts only the temporary).
            bool keptRvalue = false, mineRvalue = false;
            for (size_t p = 1; p < kept.paramTypes.size(); ++p)
                if (kept.paramTypes[p].size() > 1
                    && kept.paramTypes[p].compare(kept.paramTypes[p].size() - 2, 2, "&&") == 0)
                    keptRvalue = true;
            for (size_t p = 1; p < m.paramTypes.size(); ++p)
                if (m.paramTypes[p].size() > 1
                    && m.paramTypes[p].compare(m.paramTypes[p].size() - 2, 2, "&&") == 0)
                    mineRvalue = true;
            const bool keptBindable = hasReachableSymbol(kept);
            const bool mineBindable = hasReachableSymbol(m);
            if (keptBindable != mineBindable)         { if (mineBindable) it->second = i; }
            else if (kept.isConst && !m.isConst)      it->second = i;
            else if (kept.isVolatile && !m.isVolatile) it->second = i;
            else if (keptRvalue && !mineRvalue)       it->second = i;
        }

        // Registration names that have at least ONE implicit conversion declared: the set below
        // is explicit-only, so a mixed overload set keeps the implicit path open.
        std::set<std::string> implicitConversionNames;
        for (const Member& m : r.members)
            if (m.isConversion && !m.isExplicit)
                implicitConversionNames.insert(memberRegName(m));

        for (size_t i = 0; i < r.members.size(); ++i)
        {
            const Member& m = r.members[i];
            const bool isStructor = m.kind == Member::Constructor || m.kind == Member::Destructor;

            // Settled BEFORE the refusal checks so a private, deleted or otherwise unbindable
            // conversion is recorded under the same key the cast site will ask for.
            const std::string cflatName = memberRegName(m);
            const bool constrainedTwin = constrainedTwinMembers[i];
            if (memberFilter == "__copy_move_ctor" && m.kind == Member::Constructor
                && !m.isCopyCtor && !m.isMoveCtor)
                continue;
            if ((memberFilter == "__ctor" || memberFilter == "__copy_move_ctor")
                && m.kind == Member::Constructor
                && alreadyRegisteredConstructorLinkages.count(m.linkageName) != 0)
                continue;
            const bool registerConstViewData = m.kind == Member::Instance
                && m.name == "data" && m.isConst;
            bool constViewDataTwin = false;
            // A cv-qualified member twin uses the synthetic group name so receiver cv can rank it.
            bool cvOverloadTwin = false;
            if (m.isConversion && m.isExplicit
                && implicitConversionNames.count(cflatName) == 0)
                info.explicitConversions.insert(cflatName);
            if (!memberFilter.empty() && cflatName != memberFilter
                && !(memberFilter == "__copy_move_ctor"
                     && m.kind == Member::Constructor)) continue;
            if (m.kind == Member::Instance && generatedCxxRecords_.count(r.name) != 0)
            {
                const auto generated = generatedCxxRecords_.find(r.name);
                if (generated->second.overrideNames.count(m.name) != 0) continue;
            }

            if (m.kind == Member::Instance || m.kind == Member::StaticMethod)
            {
                auto ma = info.memberAccess.find(cflatName);
                if (ma == info.memberAccess.end()) info.memberAccess[cflatName] = m.access;
                else if (m.access < ma->second)    ma->second = m.access;
                if (m.kind == Member::StaticMethod
                    && std::find(info.staticMethodNames.begin(), info.staticMethodNames.end(),
                                 cflatName) == info.staticMethodNames.end())
                    info.staticMethodNames.push_back(cflatName);
            }

            auto refuse = [&](const std::string& why) {
                std::string displayedWhy = why;
                if (displayedWhy.starts_with("cannot be instantiated")
                    && !m.refusalCause.empty())
                {
                    const std::string causeLine = CxxFirstDiagnosticLine(m.refusalCause);
                    if (!causeLine.empty())
                        displayedWhy += std::format(" (clang: {})", causeLine);
                }
                if (verbose)
                    std::cout << std::format("[verbose]   C++ member {}.{} not bound: {}\n",
                                             r.name, m.name, displayedWhy);
                if (isStructor) return;
                // An overload clang could not instantiate must not hide a sibling's retryable
                // refusal (an unrequested return type): the use site rebinds only on the latter.
                auto known = info.refusedMembers.find(cflatName);
                if (known == info.refusedMembers.end()) info.refusedMembers[cflatName] = displayedWhy;
                else if (known->second.starts_with("cannot be instantiated")
                         && !displayedWhy.starts_with("cannot be instantiated"))
                    known->second = displayedWhy;
            };
            recordDirectMethod(m);
            // Private ordinary instance methods follow the protected rule: a named lookup registers
            // them, so the selected-overload access check refuses a private pick. Special assignments
            // stay refused (an inaccessible copy-assign has no symbol to call).
            if (m.access == cflat_cinterop::AccessPrivate
                && (m.kind != Member::Instance || m.isCopyAssign || m.isMoveAssign
                    || (memberFilter.empty() && !m.isVirtual)))
            { refuse("is private");   continue; }
            if (m.access == cflat_cinterop::AccessProtected
                && (m.kind != Member::Instance || (memberFilter.empty() && !m.isVirtual)))
            { refuse("is protected"); continue; }
            if (m.isDeleted)
            {
                if (m.kind == Member::Constructor && m.isMoveCtor) info.hasDeletedMoveCtor = true;
                refuse("is deleted");
                // Ordinary deleted methods must participate in overload ranking. Special
                // members use their dedicated constructor/assignment metadata below.
                if (m.kind == Member::Constructor || m.isCopyAssign || m.isMoveAssign)
                    continue;
            }
            if (!m.bindRefusal.empty())   { refuse(m.bindRefusal);                       continue; }
            if (!r.layoutRefusal.empty() && m.kind != Member::StaticMethod)
            {
                refuse("belongs to a class whose layout cflat cannot reproduce");
                continue;
            }
            const std::string lazyStdFunctionKey = r.name + "."
                + (m.kind == Member::Constructor ? "__ctor#" + m.linkageName : m.name);
            // Operators stay eager: operator, compound and conversion lookups never rebind.
            const bool lazyStdFunctionCandidate = m.kind == Member::Constructor
                || ((m.kind == Member::Instance || m.kind == Member::StaticMethod)
                    && !m.name.starts_with("operator"));
            if (lazyStdFunctionCandidate
                && cxxLazyStdFunctionMemberBinds_.count(lazyStdFunctionKey) == 0
                && (IsTopLevelStdFunctionSpelling(m.retType)
                    || std::any_of(m.paramTypes.begin(), m.paramTypes.end(),
                        [](const std::string& spelling) {
                            return IsTopLevelStdFunctionSpelling(spelling);
                        })))
            {
                const std::string refusal = "has a std::function signature that could not be bound";
                if (m.kind == Member::Constructor)
                    info.refusedMembers["__ctor"] = refusal;
                else
                    refuse(refusal);
                continue;
            }
            // Variadic constructors take the wrapper path below (out-of-line or not). A variadic
            // ordinary member binds like an in-class one, whether or not its definition is inline.
            if (m.requiresConstructorWrapper
                || (m.variadic && m.kind == Member::Constructor))
            {
                continue;
            }
            /*
             * A class with VIRTUAL BASES has an implicit extra CONSTRUCTOR argument in every ABI
             * cflat targets - the MS is-most-derived flag, the Itanium VTT - and a direct call
             * passes neither. The extractor redirects such a constructor to a placement-new thunk
             * (CxxCtorNeedsVbaseThunk / CxxVbaseCtorThunkName), which is recognizable by its
             * linkage name. Anything that did NOT get one still has the raw symbol, so it stays
             * refused rather than emitting a call that seeds the vbtable from a garbage register.
             */
            if (m.kind == Member::Constructor && r.hasVirtualBases
                && !m.linkageName.starts_with(cflat_cinterop::kCxxVbaseCtorThunkPrefix))
            {
                refuse("belongs to a class with virtual bases, whose constructor takes an implicit "
                       "most-derived argument cflat does not pass");
                continue;
            }
            // A POINTER TO MEMBER has an ABI representation of its own (Itanium: a two-word
            // {ptr, adj} pair for a member function, a byte offset for a data member) and its own
            // invocation sequence; an ordinary function pointer is not a substitute. Name it
            // explicitly rather than letting the type mapper report "unsupported type".
            {
                bool memberPtr = m.retType.find("::*") != std::string::npos;
                for (const auto& pt : m.paramTypes)
                    if (pt.find("::*") != std::string::npos) memberPtr = true;
                if (memberPtr)
                {
                    refuse("uses a pointer to member, whose ABI representation and invocation "
                           "are not implemented (an ordinary function pointer is not equivalent)");
                    continue;
                }
            }
            // A VIRTUAL member is dispatched through the vtable. An all-inline hierarchy is
            // supported now (M7): clang emits the bodies AND the vtable into the companion module.
            // This only fires when no definition exists anywhere - no symbol in the bound library
            // and no body clang could emit - in which case the vtable slot would be empty.
            if (m.isVirtual && m.needsLocalDefinition)
                                          { refuse("is virtual and has no definition cflat can reach: neither an out-of-line symbol in the bound library nor a body clang could emit, so its vtable slot is empty"); continue; }
            if (m.isVirtual && m.covariantReturnNeedsAdjust)
                                          { refuse("has a covariant return type whose base conversion is not at offset zero, which needs a return-adjusting thunk cflat cannot synthesize (not supported yet)"); continue; }
            if (m.isVirtual && m.vtableIndex < 0)
                                          { refuse("is virtual but cflat could not determine its vtable slot"); continue; }
            // Inline bodies ARE emitted (M7). What is left here is a member with no body at all:
            // an implicit or defaulted special member Sema declined to define, or a template
            // member awaiting instantiation support.
            if (m.needsLocalDefinition && !m.isDeleted)
                                          { refuse("has no definition cflat can reach: clang emitted no body for it (an implicit, defaulted or template member)"); continue; }
            if (!m.abi.valid && !m.isDeleted)
                                          { refuse("has a signature clang could not arrange for this target"); continue; }
            if (m.linkageName.empty() && !m.isDeleted)
                                          { refuse("has no external linkage");            continue; }
            if (m.kind == Member::Instance && !m.isCopyAssign && !m.isMoveAssign)
            {
                auto it = instanceBySig.find(cflatSigKey(m));
                constViewDataTwin = it != instanceBySig.end() && it->second != i;
                if (constViewDataTwin && !registerConstViewData)
                {
                    const Member& kept = r.members[it->second];
                    cvOverloadTwin = (m.isConst != kept.isConst
                        || m.isVolatile != kept.isVolatile);
                    if (!cvOverloadTwin && !constrainedTwin)
                        continue;
                }
            }

            TypeAndValue ret;
            CxxReferenceKind returnRefKind = CxxReferenceKind::None;
            CxxSpellingWithoutRef(m.retType, &returnRefKind);
            const bool stdFunctionRet = !isStructor
                && mapStdFunction(m.retType, /*isReturn*/ true, /*structorParam*/ false, ret);
            if (!stdFunctionRet && !mapType(m.retType, ret, true))
            {
                refuse(IsLongDoubleSpelling(m.retType) && !IsCInteropLongDoubleSupported()
                    ? CInteropLongDoubleRefusal()
                    : std::format("returns unsupported type '{}'", m.retType));
                continue;
            }
            if ((returnRefKind == CxxReferenceKind::Lvalue
                 || returnRefKind == CxxReferenceKind::RefToPointer)
                && ret.Pointer
                && ret.TypeName == "void")
            {
                // An unknown T& or T*& is represented as void* by the scalar mapper. Do not turn
                // it into an alias void result: that would declare a different LLVM return type
                // from clang and report a misleading ABI mismatch.
                refuse(std::format("returns a reference to unsupported type '{}'", m.retType));
                continue;
            }
            /*
             * A C++ LVALUE REFERENCE return (`int &`, `std::string &` - what `operator[]` and
             * `back()` hand out) is a borrowed lvalue, which is exactly CFlat's `alias T`: the
             * callee returns a pointer (the Itanium representation, so the ABI is unchanged) and
             * the call site turns it back into storage, so `int x = v[0];` reads and `v[0] = 5;`
             * stores. Without this the result would be a bare `int *` and every use would need a
             * manual dereference. Structors and assignment operators keep their own shapes.
             */
            // A structor's parameters and an assignment operator's result keep the pointer shape
            // the construct-into-slot and assignment paths already expect.
            const bool aliasRefs = !isStructor && !m.isCopyAssign && !m.isMoveAssign;
            auto asAliasIfRef = [&](const std::string& spelling, TypeAndValue& tv,
                                    bool isReturn = false) {
                if (tv.Pointer && tv.TypeName == "void")
                {
                    TypeAndValue remapped;
                    bool representable = false;
                    if (TryMapCxxForeignSpelling(spelling, remapped, representable) && representable)
                        tv.TypeName = remapped.TypeName;
                    int ptrLevels = 0;
                    const std::string tag = CxxRecordPointeeTag(spelling, ptrLevels);
                    if (ptrLevels == 1
                        && (IsCxxRecord(tag) || IsCxxLazyAliasSpecialization(tag)))
                        tv.TypeName = tag;
                }
                CxxReferenceKind refKind = CxxReferenceKind::None;
                const std::string bare = CxxSpellingWithoutRef(spelling, &refKind);
                if (refKind == CxxReferenceKind::None) return;
                if (refKind == CxxReferenceKind::Rvalue
                    && (!isReturn || IsCxxRecord(tv.TypeName)
                        || IsCxxLazyAliasSpecialization(tv.TypeName))) return;
                if (!tv.Pointer || tv.IsFunctionPointer || tv.IsArrayView) return;
                // A T**& is already at its CFlat depth with the flag set by the mapper; peeling a
                // level here would turn it into a T*.
                if (tv.IsCxxRefToPointer)
                {
                }
                else if (tv.ElemPointer)
                {
                    std::string bareWithoutCv = bare;
                    for (const char* q : { "const", "volatile", "restrict", "__restrict",
                                           "__restrict__" })
                        EraseDeclaratorToken(bareWithoutCv, q);
                    if (bareWithoutCv.empty() || bareWithoutCv.back() != '*'
                        || std::count(bareWithoutCv.begin(), bareWithoutCv.end(), '*') != 1)
                        return;
                    tv.ElemPointer = false;
                    tv.IsCxxRefToPointer = true;
                }
                // A std::nullptr_t referent is the pointer-sized null itself: keep void* (the
                // free-function shape), never a `void` alias no LLVM parameter can carry.
                else if (tv.TypeName == "void" && CxxSpellingIsBareNullptrT(bare))
                    tv.IsCxxNullptrT = true;
                else
                    tv.Pointer = false;
                tv.IsAlias = true;
                tv.MarkCxxRef();
                tv.SetCxxConstRef(CxxParamIsConstLvalueReference(spelling)
                    || (refKind == CxxReferenceKind::Rvalue
                        && CxxRvalueReferenceIsConst(spelling))
                    || (tv.IsCxxRefToPointer && CxxParamIsConstRefToPointer(spelling)));
            };
            if (aliasRefs) asAliasIfRef(m.retType, ret, true);
            if (ret.Pointer && !ret.IsFunctionPointer)
                ret.ConstMask |= CxxSpellingPointeeConstMask(m.retType, ret.ValuePointerDepth());
            if (CxxSpellingHasVolatileTarget(m.retType))
                ret.IsCxxPointeeVolatile = true;
            std::vector<TypeAndValue> params;
            bool paramsOk = true;
            size_t unmappableParam = m.paramTypes.size();
            for (size_t p = 0; p < m.paramTypes.size(); ++p)
            {
                TypeAndValue tv;
                if (p == 0 && m.kind != Member::StaticMethod)
                {
                    // 'this' is spelled as a pointer to the record itself, never as the decayed
                    // void* the string mapper produces for a struct pointer - the member-call path
                    // matches the receiver against Parameters[0].TypeName.
                    tv = TypeAndValue{};
                    tv.TypeName = r.name;
                    tv.Pointer = true;
                }
                // A std::function parameter crosses as its specialization by alias, exactly as
                // the free-function path passes one; the general mapper cannot express it.
                else if (mapStdFunction(m.paramTypes[p], /*isReturn*/ false, isStructor, tv,
                                        m.kind == Member::Instance && !m.name.starts_with("operator")))
                {
                }
                else if (!mapType(m.paramTypes[p], tv)
                         || (!tv.Pointer && !tv.IsFunctionPointer && tv.TypeName == "void")
                         || (!aliasRefs && tv.IsCxxRefToPointer && !tv.IsFunctionPointer))
                {
                    // A structor and an assignment operator keep a reference parameter in the raw
                    // extra-pointer shape, which a reference to a T** would need three levels for.
                    // A by-value spelling the mapper cannot express arrives as bare "void";
                    // an LLVM function type with a void parameter asserts (garbage in Release).
                    refuse(IsLongDoubleSpelling(m.paramTypes[p])
                            && !IsCInteropLongDoubleSupported()
                        ? CInteropLongDoubleRefusal()
                        : std::format("takes unsupported type '{}'", m.paramTypes[p]));
                    paramsOk = false;
                    unmappableParam = p;
                    break;
                }
                if (isStructor && m.kind == Member::Constructor)
                    markConstructorReference(m.paramTypes[p], tv);
                else if (aliasRefs) asAliasIfRef(m.paramTypes[p], tv);
                // An R&& to a record not yet projectable stays void* (asAliasIfRef keeps rvalue
                // params as-is); refuse it like the const R& twin so a later projection retries.
                if (aliasRefs && tv.Pointer && !tv.ElemPointer && !tv.IsFunctionPointer
                    && tv.TypeName == "void" && !tv.IsCxxNullptrT)
                {
                    CxxReferenceKind rk = CxxReferenceKind::None;
                    const std::string rb = CxxSpellingWithoutRef(m.paramTypes[p], &rk);
                    if (rk == CxxReferenceKind::Rvalue && rb.find('*') == std::string::npos
                        && !CxxSpellingIsBareNullptrT(rb) && rb.find("void") == std::string::npos)
                    {
                        refuse(std::format("takes unsupported type '{}'", m.paramTypes[p]));
                        paramsOk = false;
                        unmappableParam = p;
                        break;
                    }
                }
                // libc++ keeps vector<T*>::push_back's reference in the dependent
                // value_type spelling on some Clang versions, which canonicalizes without
                // exposing '&'. Its ABI is still the T*& slot shape.
                if (aliasRefs && !tv.IsCxxRefToPointer && tv.Pointer && tv.ElemPointer
                    && r.name.starts_with("std.vector$") && m.name == "push_back")
                {
                    tv.ElemPointer = false;
                    tv.IsCxxRefToPointer = true;
                    tv.IsAlias = true;
                }
                tv.VariableName = p < m.paramNames.size() && !m.paramNames[p].empty()
                    ? m.paramNames[p] : std::format("p{}", p);
                if (tv.Pointer && !tv.IsFunctionPointer)
                    tv.ConstMask |= CxxSpellingPointeeConstMask(m.paramTypes[p], tv.ValuePointerDepth());
                if (CxxSpellingHasVolatileTarget(m.paramTypes[p]))
                    tv.IsCxxPointeeVolatile = true;
                // The mapped LLVM type must be a legal function argument (never void, a bare
                // function type, or a non-first-class type): LLVM asserts, Release miscompiles.
                if (llvm::Type* lt = GetType(tv); lt == nullptr || !llvm::FunctionType::isValidArgumentType(lt))
                {
                    refuse(std::format("takes unsupported type '{}'", m.paramTypes[p]));
                    paramsOk = false;
                    unmappableParam = p;
                    break;
                }
                params.push_back(std::move(tv));
            }
            // A member whose omitted suffix contains a non-constant default gets an exact-arity
            // receiver-prefixed overload backed by a C++ forwarding body. The generated body is
            // outside the class, so only public members are eligible and const receivers retain
            // their declared constness in the wrapper's C++ parameter.
            const auto registerDefaultArityWrapper = [&](const std::vector<TypeAndValue>& prefix,
                                                         size_t n, bool forTwin) -> bool
            {
                if (m.variadic || m.linkageName.empty()
                    || m.defaultArgs.size() != m.paramTypes.size()) return false;
                if (m.kind != Member::Instance && m.kind != Member::StaticMethod) return false;
                const bool fullArity = n == m.paramTypes.size();
                if (n > m.defaultArgs.size() || (fullArity && !forTwin)) return false;
                if (!fullArity)
                {
                    if (n >= m.defaultArgs.size()) return false;
                    if (forTwin)
                    {
                        for (size_t p = n; p < m.defaultArgs.size(); ++p)
                            if (m.defaultArgs[p].kind.empty()) return false;
                    }
                    else if (m.defaultArgs[n].kind == "unsupported"
                             || !HasNonConstDefaultSuffix(m.defaultArgs, n)) return false;
                }
                const bool havePlan = m.abi.valid && m.abi.params.size() == m.paramTypes.size();
                const auto canWrap = [&](const TypeAndValue& t) {
                    return havePlan || t.Pointer || !dataStructures.count(t.TypeName);
                };
                if (!canWrap(ret)) return false;
                for (size_t i = 0; i < n; ++i)
                    if (!canWrap(prefix[i])) return false;
                const std::string wrapper = CxxDefaultWrapperName(m.linkageName, n);
                // Same spelling the full-arity registration below uses; computed here because the
                // unmappable-parameter path reaches this lambda before that declaration.
                const std::string wrapperRegName = m.kind == Member::StaticMethod
                    ? r.name + "." + cflatName
                    : cvOverloadTwin ? CxxConstTwinName(cflatName) : cflatName;
                if (m.kind == Member::StaticMethod)
                {
                    NoteCxxForeignNamespace(wrapperRegName);
                    for (size_t pos = 0;
                         (pos = wrapperRegName.find('.', pos)) != std::string::npos; ++pos)
                        RegisterNamespace(wrapperRegName.substr(0, pos));
                }
                cflat_cinterop::RawAbi wrapperPlan;
                if (havePlan)
                {
                    wrapperPlan = m.abi;
                    wrapperPlan.params.resize(n);
                    // The cross-check text describes the FULL arity; the truncated plan has no
                    // matching text, so the slot-by-slot build stands on its own.
                    wrapperPlan.fnTypeText.clear();
                    // The wrapper is a free extern "C" function: MS ABI puts its sret slot first,
                    // not after the receiver as for the member it forwards to.
                    wrapperPlan.ret.sretAfterThis = false;
                }
                std::string wrapperMismatch;
                CInteropDeclarationScope declaringFile(*this,
                    m.file.empty() ? fileForLsp : m.file);
                {
                    CxxAbiPlanScope wrapperAbi(*this, havePlan ? &wrapperPlan : nullptr,
                                               &wrapperMismatch);
                    CreateFunctionDeclaration(wrapperRegName, ret, prefix, /*external=*/true,
                        /*varargs=*/false, /*returnsOwned=*/false,
                        /*isMethod=*/m.kind == Member::Instance,
                        CallingConv::Cdecl, wrapper, /*isCxx=*/true, m.isNoexcept,
                        m.refQualifier);
                }
                if (!wrapperMismatch.empty())
                {
                    if (verbose)
                        std::cout << std::format(
                            "[verbose]   C++ default wrapper {} not bound: {}\n",
                            wrapper, wrapperMismatch);
                    return false;
                }
                if (auto wit = functionTable.find(wrapperRegName); wit != functionTable.end())
                    for (FunctionSymbol& sym : wit->second)
                        if (sym.External && sym.UniqueName == wrapper)
                        {
                            sym.IsCInteropDeclaration = true;
                            sym.IsCxxTemplateSpecialization = m.isTemplateSpecialization;
                            sym.DefaultArguments.clear();
                            sym.CxxRefQualifier = m.refQualifier;
                            sym.CxxVolatile = m.isVolatile;
                            sym.CxxConst = m.isConst;
                        }
                return true;
            };
            /*
             * An unmappable parameter that is DEFAULTED does not have to sink the whole member:
             * the shorter arity never names the type. torch::optim::Optimizer::step takes a
             * LossClosure (std::function<at::Tensor()>) cflat has no spelling for, but `opt.step()`
             * passes nothing - the generated wrapper applies the C++ default on the C++ side. Only
             * the arity that WOULD name the type stays refused, so passing one still says why.
             */
            if (!paramsOk)
            {
                if (!registerDefaultArityWrapper(params, unmappableParam, false)) continue;
                if (m.kind == Member::Instance) info.instanceMethodNames.push_back(cflatName);
                else if (m.kind == Member::StaticMethod
                         && std::find(info.staticMethodNames.begin(), info.staticMethodNames.end(),
                                      cflatName) == info.staticMethodNames.end())
                    info.staticMethodNames.push_back(cflatName);
                if (info.memberAccess.find(cflatName) == info.memberAccess.end())
                    info.memberAccess[cflatName] = m.access;
                continue;
            }

            bool twinForwarderBound = false;
            std::string twinForwarderLinkage;
            if (constrainedTwin)
            {
                const std::string twinKey = cflatSigKey(m);
                bool defaultWrappersBound = true;
                for (size_t n = 0; n < params.size(); ++n)
                {
                    auto source = twinWrapperSourceByArity.find({ twinKey, n });
                    if (source == twinWrapperSourceByArity.end() || source->second != i) continue;
                    std::vector<TypeAndValue> prefix(params.begin(), params.begin() + n);
                    if (!registerDefaultArityWrapper(prefix, n, true))
                    {
                        refuse("has a constrained overload whose default wrapper could not be bound");
                        defaultWrappersBound = false;
                        break;
                    }
                }
                if (!defaultWrappersBound) continue;
                auto fullSource = twinWrapperSourceByArity.find({ twinKey, params.size() });
                if (fullSource == twinWrapperSourceByArity.end() || fullSource->second != i)
                    continue;
                if (!registerDefaultArityWrapper(params, params.size(), true))
                {
                    refuse("has a constrained overload whose full-arity wrapper could not be bound");
                    continue;
                }
                twinForwarderBound = true;
                twinForwarderLinkage = CxxDefaultWrapperName(m.linkageName, params.size());
            }

            if (isStructor || m.isCopyAssign || m.isMoveAssign)
            {
                CxxClassInfo::Structor st;
                st.linkageName = m.linkageName;
                st.params = params;
                // On Itanium/Darwin a structor hands 'this' back and an assignment operator
                // returns 'T&'. Declare the callee with the result clang actually emits: the
                // exported retType is "void" for a structor, which would mis-type the callee.
                if (isStructor && m.returnsThis)
                {
                    st.ret = TypeAndValue{};
                    st.ret.TypeName = r.name;
                    st.ret.Pointer = true;
                }
                else
                {
                    st.ret = ret;
                }
                st.isDefaultCtor = m.isDefaultCtor;
                st.isExplicit = m.isExplicit;
                st.isCopyCtor = m.isCopyCtor;
                st.isMoveCtor = m.isMoveCtor;
                st.isDeleted = m.isDeleted;
                st.isConstReceiver = m.isConst;
                st.rhsConstRef = m.isCopyAssign && m.paramTypes.size() > 1 && m.paramTypes[1].starts_with("const ");
                st.needsLocalDefinition = m.needsLocalDefinition;
                st.isNoexcept = m.isNoexcept;
                st.refQualifier = m.refQualifier;
                st.access = m.access;
                st.abi = m.abi;
                st.defaultArgs = m.defaultArgs;
                if (m.kind == Member::Constructor) info.constructors.push_back(std::move(st));
                else if (m.kind == Member::Destructor)
                {
                    info.destructor = std::move(st); info.hasDtor = true;
                    if (m.isVirtual)
                    {
                        info.dtorVtableIndex = m.vtableIndex;
                        info.dtorDeletingVtableIndex = m.vtableIndexDeleting;
                    }
                }
                else if (m.isCopyAssign)
                {
                    info.copyAssignOverloads.push_back(std::move(st));
                    info.copyAssign = info.copyAssignOverloads.back();
                    info.hasCopyAssign = true;
                }
                else
                {
                    info.moveAssignOverloads.push_back(std::move(st));
                    info.moveAssign = info.moveAssignOverloads.back();
                    info.hasMoveAssign = true;
                }
                continue;
            }

            const std::string regName = registerConstViewData && constViewDataTwin
                ? "__cflat_view_decay_const_data"
                : cvOverloadTwin ? CxxConstTwinName(cflatName)
                : m.kind == Member::StaticMethod ? r.name + "." + cflatName : cflatName;
            if (m.kind == Member::StaticMethod)
            {
                NoteCxxForeignNamespace(regName);
                for (size_t pos = 0; (pos = regName.find('.', pos)) != std::string::npos; ++pos)
                    RegisterNamespace(regName.substr(0, pos));
            }

            std::string abiMismatch;
            const std::string registrationLinkage = twinForwarderBound
                ? twinForwarderLinkage
                : m.linkageName.empty() && m.isDeleted
                ? std::format("__cflat_deleted_{}_{}", r.name, i) : m.linkageName;
            if (!twinForwarderBound)
            {
                CInteropDeclarationScope declaringFile(*this, m.file.empty() ? fileForLsp : m.file);
                CxxAbiPlanScope abiPlan(*this, m.isDeleted ? nullptr : &m.abi, &abiMismatch);
                CreateFunctionDeclaration(regName, ret, params, /*external=*/true, m.variadic,
                                          /*returnsOwned=*/false,
                                          /*isMethod=*/m.kind == Member::Instance,
                                          CallingConv::Cdecl, registrationLinkage, /*isCxx=*/true,
                                          m.isNoexcept, m.refQualifier, m.isDeleted);
            }
            // A member whose lowering clang disagrees with is refused HERE, at registration: the
            // use site reports why, and importing the class stays clean.
            if (!abiMismatch.empty()) { refuse(abiMismatch); continue; }
            if (auto fit = functionTable.find(regName); fit != functionTable.end())
                for (FunctionSymbol& sym : fit->second)
                    if (sym.External && sym.UniqueName == registrationLinkage)
                    {
                        sym.IsCInteropDeclaration = true;
                        sym.IsCxxTemplateSpecialization = m.isTemplateSpecialization;
                        if (twinForwarderBound) sym.DefaultArguments.clear();
                        else sym.DefaultArguments = m.defaultArgs;
                        sym.CxxRefQualifier = m.refQualifier;
                        sym.CxxVolatile = m.isVolatile;
                        sym.CxxConst = m.isConst;
                    }
            // A member whose omitted suffix contains a non-constant default gets an exact-arity
            // receiver-prefixed overload backed by a C++ forwarding body. The generated body is
            // outside the class, so only public members are eligible and const receivers retain
            // their declared constness in the wrapper's C++ parameter.
            if (!constrainedTwin)
                for (size_t n = 0; n < params.size(); ++n)
                {
                    std::vector<TypeAndValue> prefix(params.begin(), params.begin() + n);
                    registerDefaultArityWrapper(prefix, n, false);
                }
            // A virtual member is called through the receiver's vptr at this slot; the
            // declaration above exists only to carry clang's arrangement and LLVM type.
            if (m.kind == Member::Instance && m.isVirtual)
                cxxVirtualSlotByLinkage_[m.linkageName] = { m.vtableIndex, m.vtableOffsetBytes };
            if (cvOverloadTwin) continue;
            if (m.kind == Member::Instance)
                info.instanceMethodNames.push_back(cflatName);
            else if (m.kind == Member::StaticMethod
                     && std::find(info.staticMethodNames.begin(), info.staticMethodNames.end(),
                                  cflatName) == info.staticMethodNames.end())
                info.staticMethodNames.push_back(cflatName);

            if (auto* s = GetSymbolSink())
            {
                std::string sig = SpellType(*this, ret) + " " + r.name + "." + cflatName + "(";
                bool first = true;
                for (size_t p = (m.kind == Member::Instance ? 1u : 0u); p < params.size(); ++p)
                {
                    if (!first) sig += ", ";
                    first = false;
                    sig += SpellType(*this, params[p]);
                }
                sig += ")";
                s->Register(SymbolKind::Function, r.name + "." + cflatName,
                            m.file.empty() ? fileForLsp : m.file, m.line, m.col < 0 ? 0 : m.col, sig);
            }
        }

        if (memberFilter.empty() || std::any_of(r.staticVars.begin(), r.staticVars.end(),
                [&](const auto& variable) { return variable.name == memberFilter; }))
        for (const auto& sv : r.staticVars)
        {
            if (!memberFilter.empty() && sv.name != memberFilter) continue;
            if (sv.access != cflat_cinterop::AccessPublic) continue;
            if (!sv.linkageName.empty()) cxxStaticMemberSymbols_.insert(sv.linkageName);
            if (!sv.initializerFailure.empty() && !sv.linkageName.empty())
                // "pending:<qualified C++ name>" = no clang text survived (reported earlier).
                cxxBodyChecks_[sv.linkageName] = sv.initializerFailure.starts_with("pending:")
                    ? "clang: initializer of '" + sv.initializerFailure.substr(8)
                        + "' failed to instantiate"
                    : "clang: " + sv.initializerFailure;
            TypeAndValue tv;
            if (!mapType(sv.ctype, tv)) continue;
            tv.ConstMask |= CxxSpellingPointeeConstMask(sv.ctype, tv.ValuePointerDepth());
            MarkCxxDeclaredConstField(tv, CxxSpellingHasTopLevelObjectConst(sv.ctype));
            const std::string regName = r.name + "." + sv.name;
            if (globalNamedVariable.count(regName)) continue;
            for (size_t pos = 0; (pos = regName.find('.', pos)) != std::string::npos; ++pos)
                RegisterNamespace(regName.substr(0, pos));
            tv.VariableName = regName;
            llvm::Constant* init = nullptr;
            if (sv.isCompileTimeConstant)
            {
                llvm::Type* valueType = GetType(tv);
                if (valueType->isIntegerTy())
                    init = llvm::ConstantInt::get(
                        valueType,
                        llvm::APInt(valueType->getIntegerBitWidth(), (uint64_t)sv.constantValue,
                                    /*isSigned=*/true, /*implicitTrunc=*/true));
                else if (sv.isFloatConstant && valueType->isFloatingPointTy())
                    init = llvm::ConstantFP::get(valueType, sv.floatValue);
                else
                    continue;
            }
            auto* staticGlobal = CreateGlobalVariable(tv, init, /*threadLocal*/ false, /*userAlign*/ 0,
                                 /*externalDecl*/ !sv.isCompileTimeConstant,
                                 /*srcIsUnsigned*/ false, sv.linkageName);
            MarkCxxConstGlobal(staticGlobal);
            if (!sv.isCompileTimeConstant)
                cxxStaticMemberSymbols_.insert(staticGlobal->getName().str());
            if (sv.isCompileTimeConstant)
            {
                SetConstGlobalInt(regName, sv.constantValue);
                SetConstGlobalInt(staticGlobal->getName().str(), sv.constantValue);
            }
            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, regName,
                            sv.file.empty() ? fileForLsp : sv.file, sv.line, sv.col < 0 ? 0 : sv.col,
                            SpellType(*this, tv) + " " + regName
                                + (sv.isCompileTimeConstant
                                    ? ConstIntValueSuffix(tv.TypeName, sv.constantValue) : ""));
        }

        CxxClassInfo& slot = cxxClasses_[r.name];
        if (!memberFilter.empty())
        {
            /*
             * A filtered projection runs lazily inside use sites that may hold Structor or Method
             * references into this entry (a selected ctor across `operator new` lookup). The
             * groups are append-only, so an unchanged group keeps its existing storage.
             */
            if (info.constructors.size() == slot.constructors.size())
                info.constructors.swap(slot.constructors);
            if (info.directMethods.size() == slot.directMethods.size())
                info.directMethods.swap(slot.directMethods);
            if (info.copyAssignOverloads.size() == slot.copyAssignOverloads.size())
            {
                info.copyAssignOverloads.swap(slot.copyAssignOverloads);
                std::swap(info.copyAssign, slot.copyAssign);
            }
            if (info.moveAssignOverloads.size() == slot.moveAssignOverloads.size())
            {
                info.moveAssignOverloads.swap(slot.moveAssignOverloads);
                std::swap(info.moveAssign, slot.moveAssign);
            }
            if (memberFilter != "__dtor") std::swap(info.destructor, slot.destructor);
        }
        slot = std::move(info);
        // A requested specialization can expose a member whose first signature mapping failed
        // because a nested by-value specialization was not registered yet. The normal use-site
        // retry cannot run for members called only from an inline C++ body (Stack::apply_batch is
        // one), so retry signature refusals once the owner is fully registered - but ONLY those
        // whose signature types are already known. Retrying a member that needs a NEW request
        // here made registration the transitive closure of every member's return and parameter
        // types (libtorch: one `torch::ones` call -> 400+ requests, 600+ clang TUs); a member
        // the program actually calls is retried at its use site instead.
        if (memberFilter.empty() && r.name.find('$') != std::string::npos)
        {
            std::vector<std::string> refused;
            for (const auto& [memberName, refusal] : cxxClasses_[r.name].refusedMembers)
                if ((refusal.starts_with("returns unsupported type '")
                     || refusal.starts_with("returns a reference to unsupported type '")
                     || refusal.starts_with("takes unsupported type '"))
                    && CxxRefusedMemberSignatureKnown(r, memberName))
                    refused.push_back(memberName);
            for (const std::string& memberName : refused)
            {
                const std::string guardKey = r.name + "." + memberName;
                if (!cxxRefusedMemberRebindInFlight_.insert(guardKey).second) continue;
                TryBindRefusedCxxMember(r.name, memberName);
                cxxRefusedMemberRebindInFlight_.erase(guardKey);
            }
        }
        }

bool LLVMBackend::IsUnmappedGlobalCxxClassSpelling(const std::string& spelling)
{
        if (spelling.empty() || std::isdigit((unsigned char)spelling[0])) return false;
        for (char ch : spelling)
            if (!std::isalnum((unsigned char)ch) && ch != '_') return false;
        TypeAndValue mapped;
        if (!MapCTypeToTypeAndValue(spelling, mapped, true)) return true;
        return mapped.Pointer && (mapped.TypeName.empty() || mapped.TypeName == "void")
            && dataStructures.find(spelling) == dataStructures.end();
}

bool LLVMBackend::IsCxxGlobalClassTemplateSpecializationSpelling(
        const std::string& spelling) const
{
        if (spelling.find("::") != std::string::npos) return false;
        const size_t templateOpen = spelling.find('<');
        if (templateOpen == std::string::npos) return false;
        const std::string base = spelling.substr(0, templateOpen);
        // A CFlat generic of the same name owns the spelling; never map a C++ layout onto it.
        return HasCxxClassTemplate(base) && !AnyGenericTypeTemplateNamed(base)
            && dataStructures.find(spelling) == dataStructures.end();
}

bool LLVMBackend::CxxRefusedMemberSignatureKnown(const CRecordEntry& record,
                                                 const std::string& memberName) const
{
        for (const auto& member : record.members)
        {
            if (member.name != memberName) continue;
            std::vector<std::string> spellings = member.paramTypes;
            spellings.push_back(member.retType);
            for (const std::string& raw : spellings)
            {
                const std::string spelling = CxxMemberValueSpelling(raw);
                // A generated user record is always known; a C++ specialization merely taking
                // one as an argument (std::reverse_iterator<__cflat_user::T **>) is not.
                const bool generatedUserRecord = spelling.find("__cflat_user::") != std::string::npos
                    && (spelling.find('<') == std::string::npos
                        || spelling.starts_with("__cflat_user::")
                        || spelling.starts_with("const __cflat_user::"));
                if ((spelling.find("::") == std::string::npos
                     && !IsCxxGlobalClassTemplateSpecializationSpelling(spelling))
                    || generatedUserRecord
                    || spelling.find("type-parameter-") != std::string::npos)
                    continue;
                const std::string identity = CanonicalCxxForeignIdentity(spelling);
                if (identity.empty()) continue;
                if (cxxClasses_.count(identity) == 0 && cxxForeignRequests_.count(identity) == 0)
                    return false;
            }
        }
        return true;
}

bool LLVMBackend::TryBindRefusedCxxMember(const std::string& typeName,
                                          const std::string& memberName,
                                          size_t constructorArgumentCount,
                                          const std::set<std::string>* lazyNestedOnly)
{
        const bool lazyNested = lazyNestedOnly != nullptr;
        EnsureCxxRecordProjected(typeName, false);
        EnsureCxxMemberProjected(typeName, memberName);
        auto infoIt = cxxClasses_.find(typeName);
        auto recordIt = cxxRecordEntries_.find(typeName);
        if (infoIt == cxxClasses_.end() || recordIt == cxxRecordEntries_.end()) return false;
        auto refusalIt = infoIt->second.refusedMembers.find(memberName);
        const bool missingSpecialMember = (memberName == "__ctor"
                                           || memberName == "__dtor")
            && std::any_of(recordIt->second.members.begin(), recordIt->second.members.end(),
                [&](const auto& member) {
                    const bool wanted = memberName == "__ctor"
                        ? member.kind == cflat_cinterop::RawCxxMember::Constructor
                        : member.kind == cflat_cinterop::RawCxxMember::Destructor;
                    return wanted
                        && member.needsLocalDefinition;
                });
        if (refusalIt == infoIt->second.refusedMembers.end() && !missingSpecialMember
            && !lazyNested)
            return TryBindRefusedCxxBaseMember(typeName, memberName);

        // The lazy-nested retry never reads the stored text: it is the name's FIRST refusal only.
        const std::string refusal = lazyNested || refusalIt == infoIt->second.refusedMembers.end()
            ? std::string() : refusalIt->second;
        if (refusal == "has a std::function signature that could not be bound")
        {
            auto ownerIt = cxxTypeOwnerGroup_.find(typeName);
            if (ownerIt == cxxTypeOwnerGroup_.end()) return false;
            CxxRequestGroup group = MakeCxxClassRequestGroup(ownerIt->second, typeName);
            if (group.headers.empty()) return false;
            CxxRequestGroupScope groupScope(*this, &group);
            std::set<std::string> requestedSpellings;
            bool foundStdFunction = false;
            std::set<std::string> requestedConstructorKeys;
            for (const auto& member : recordIt->second.members)
            {
                if (member.name != memberName
                    || (member.kind != cflat_cinterop::RawCxxMember::Instance
                        && member.kind != cflat_cinterop::RawCxxMember::StaticMethod
                        && !(memberName == "__ctor"
                             && member.kind == cflat_cinterop::RawCxxMember::Constructor)))
                    continue;
                if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && constructorArgumentCount != static_cast<size_t>(-1))
                {
                    if (member.paramTypes.empty()) continue;
                    const size_t maxArgs = member.paramTypes.size() - 1;
                    size_t minArgs = maxArgs;
                    for (size_t p = 1; p < member.paramTypes.size(); ++p)
                        if (p < member.defaultArgs.size() && !member.defaultArgs[p].kind.empty())
                            --minArgs;
                    if (constructorArgumentCount < minArgs
                        || (!member.variadic && constructorArgumentCount > maxArgs))
                        continue;
                }
                if (member.kind == cflat_cinterop::RawCxxMember::Constructor)
                {
                    const bool hasStdFunction = IsTopLevelStdFunctionSpelling(member.retType)
                        || std::any_of(member.paramTypes.begin(), member.paramTypes.end(),
                            [](const std::string& spelling) {
                                return IsTopLevelStdFunctionSpelling(spelling);
                            });
                    const std::string key = typeName + ".__ctor#" + member.linkageName;
                    if (!hasStdFunction || cxxLazyStdFunctionMemberBinds_.count(key) != 0)
                        continue;
                    requestedConstructorKeys.insert(key);
                }
                auto requestIfStdFunction = [&](const std::string& spelling) {
                    if (!IsTopLevelStdFunctionSpelling(spelling)) return true;
                    foundStdFunction = true;
                    if (!requestedSpellings.insert(spelling).second) return true;
                    std::string requestError;
                    return !StdFunctionSpecializationForSpelling(spelling, &requestError).empty();
                };
                if (!requestIfStdFunction(member.retType)) return false;
                for (const std::string& spelling : member.paramTypes)
                    if (!requestIfStdFunction(spelling)) return false;
            }
            if (!foundStdFunction) return false;
            if (memberName == "__ctor")
                cxxLazyStdFunctionMemberBinds_.insert(requestedConstructorKeys.begin(),
                                                       requestedConstructorKeys.end());
            else
                cxxLazyStdFunctionMemberBinds_.insert(typeName + "." + memberName);
            const std::string file = recordIt->second.members.empty()
                ? std::string() : recordIt->second.members.front().file;
            RegisterCxxClassMembers(recordIt->second, file, memberName);
            auto rebound = cxxClasses_.find(typeName);
            if (rebound == cxxClasses_.end()) return false;
            const bool bound = memberName == "__ctor"
                ? std::any_of(rebound->second.constructors.begin(),
                              rebound->second.constructors.end(),
                    [&](const CxxClassInfo::Structor& ctor) {
                        return std::any_of(recordIt->second.members.begin(),
                                           recordIt->second.members.end(),
                            [&](const auto& raw) {
                                return raw.kind == cflat_cinterop::RawCxxMember::Constructor
                                    && raw.linkageName == ctor.linkageName
                                    && requestedConstructorKeys.count(
                                           typeName + ".__ctor#" + raw.linkageName) != 0;
                            });
                    })
                : std::find(rebound->second.instanceMethodNames.begin(),
                            rebound->second.instanceMethodNames.end(), memberName)
                        != rebound->second.instanceMethodNames.end()
                    || functionTable.count(typeName + "." + memberName) != 0;
            bool hasUnboundConstructor = false;
            if (memberName == "__ctor")
                for (const auto& raw : recordIt->second.members)
                    if (raw.kind == cflat_cinterop::RawCxxMember::Constructor
                        && (IsTopLevelStdFunctionSpelling(raw.retType)
                            || std::any_of(raw.paramTypes.begin(), raw.paramTypes.end(),
                                [](const std::string& spelling) {
                                    return IsTopLevelStdFunctionSpelling(spelling);
                                }))
                        && cxxLazyStdFunctionMemberBinds_.count(
                               typeName + ".__ctor#" + raw.linkageName) == 0)
                        hasUnboundConstructor = true;
            if (bound && !hasUnboundConstructor)
                rebound->second.refusedMembers.erase(memberName);
            return bound;
        }
        if (refusal == "is virtual but cflat could not determine its vtable slot")
            return TryBindRefusedCxxBaseMember(typeName, memberName);
        if (missingSpecialMember || refusal.starts_with("has no definition cflat can reach:"))
        {
            auto spellingIt = cxxCflatToCxxSpelling_.find(typeName);
            auto ownerIt = cxxTypeOwnerGroup_.find(typeName);
            if (spellingIt == cxxCflatToCxxSpelling_.end()
                || ownerIt == cxxTypeOwnerGroup_.end())
                return false;
            CxxRequestGroup group = MakeCxxClassRequestGroup(ownerIt->second, typeName);
            if (group.headers.empty()) return false;
            CxxRequestGroupScope groupScope(*this, &group);
            // The missing body may return or take a class-template specialization that the
            // initial header walk never needed to lay out. Request those member types first so
            // the definition-enabled owner extraction can map the now-complete signature.
            if (auto record = cxxRecordEntries_.find(typeName);
                record != cxxRecordEntries_.end())
            {
                for (const auto& member : record->second.members)
                {
                    if (member.name != memberName) continue;
                    std::vector<std::string> spellings = member.paramTypes;
                    spellings.push_back(member.retType);
                    for (const std::string& raw : spellings)
                    {
                        const std::string spelling = CxxMemberValueSpelling(raw);
                        if ((spelling.find("::") == std::string::npos
                             && !IsCxxGlobalClassTemplateSpecializationSpelling(spelling))
                            || spelling.find('<') == std::string::npos)
                            continue;
                        if (spelling.find("__cflat_user::") != std::string::npos
                            || spelling.find("type-parameter-") != std::string::npos)
                            continue;
                        TypeAndValue mapped;
                        bool mappedForeign = false;
                        if (TryMapCxxForeignSpelling(spelling, mapped, mappedForeign)
                            && mappedForeign)
                            continue;
                        const std::string identity = CanonicalCxxForeignIdentity(spelling);
                        if (identity.empty() || identity == typeName) continue;
                        std::string nestedError;
                        RequestCxxForeignType(identity, spelling, nestedError,
                                              /*needDefinitions*/ true,
                                              /*explicitInstantiation*/ true);
                    }
                }
            }
            auto instanceBound = [&]() {
                auto info = cxxClasses_.find(typeName);
                return info != cxxClasses_.end()
                    && std::find(info->second.instanceMethodNames.begin(),
                                 info->second.instanceMethodNames.end(), memberName)
                           != info->second.instanceMethodNames.end();
            };
            const bool wasBound = instanceBound();
            std::string error;
            const std::string previousBodyRequest = cxxMemberBodyRequest_;
            std::optional<std::string> previousRequest;
            const bool hadDefinition = cxxForeignDefinitions_.count(typeName) != 0;
            // An earlier definitions request (a by-value local) leaves an operator= body dormant:
            // force that body in incremental mode too.
            if (!UseCxxIncrementalRequests() || memberName == "operator=")
            {
                if (auto request = cxxForeignRequests_.find(typeName);
                    request != cxxForeignRequests_.end())
                    previousRequest = request->second;
                cxxForeignRequests_.erase(typeName);
                cxxForeignDefinitions_.erase(typeName);
                cxxMemberBodyRequest_ = memberName;
            }
            const bool requested = RequestCxxForeignType(typeName, spellingIt->second, error,
                                                         /*needDefinitions*/ true,
                                                         /*explicitInstantiation*/ true);
            cxxMemberBodyRequest_ = previousBodyRequest;
            if (!requested)
            {
                if (previousRequest) cxxForeignRequests_[typeName] = *previousRequest;
                if (hadDefinition) cxxForeignDefinitions_.insert(typeName);
                return false;
            }
            auto updated = cxxClasses_.find(typeName);
            if (updated == cxxClasses_.end()) return false;
            /*
             * One bound overload does not make the name bound: a sibling may now be refused for
             * a type no request registered yet (OrderedDict's operator[](size_t) returning its
             * nested Item &). Retry on that fresh refusal before reporting the name as rebound.
             */
            if (auto still = updated->second.refusedMembers.find(memberName);
                still != updated->second.refusedMembers.end()
                && !still->second.starts_with("has no definition cflat can reach:")
                && TryBindRefusedCxxMember(typeName, memberName, constructorArgumentCount))
                return true;
            updated = cxxClasses_.find(typeName);
            if (updated == cxxClasses_.end()) return false;
            if (instanceBound())
                return !wasBound || updated->second.refusedMembers.count(memberName) == 0;
            if (auto statics = functionTable.find(typeName + "." + memberName);
                statics != functionTable.end()
                && std::any_of(statics->second.begin(), statics->second.end(),
                               [](const FunctionSymbol& f) { return f.IsCxx && !f.IsMethod; }))
                return true;
            if (!UseCxxIncrementalRequests() && TryBindRefusedCxxBaseMember(typeName, memberName))
                return true;
            if (memberName == "__ctor") return !updated->second.constructors.empty();
            return memberName == "__dtor" && updated->second.hasDtor;
        }
        const bool unsupportedReturn = refusal.starts_with("returns unsupported type '")
            || refusal.starts_with("returns a reference to unsupported type '");
        const bool unsupportedParameter = refusal.starts_with("takes unsupported type '");
        const bool incompleteReturn = lazyNested || refusal.starts_with("returns '")
            && refusal.find("' by value, whose definition this translation unit does not have")
                   != std::string::npos;
        const bool incompleteParameter = refusal.starts_with("takes '")
            && refusal.find("' by value, whose definition this translation unit does not have")
                   != std::string::npos;
        if (!unsupportedReturn && !unsupportedParameter
            && !incompleteReturn && !incompleteParameter)
            return false;

        std::map<std::string, std::string> requests;
        std::set<std::string> successfulSpellings;
        // Return spelling -> the member returning it, for the access-free retry below.
        std::map<std::string, const cflat_cinterop::RawCxxMember*> returnMembers;
        // Clang's spelling -> the spelling a request (and every later wrapper) writes instead.
        std::map<std::string, std::string> requestSpellings;
        for (const auto& member : recordIt->second.members)
        {
            if (member.name != memberName
                || (member.kind != cflat_cinterop::RawCxxMember::Instance
                    && member.kind != cflat_cinterop::RawCxxMember::StaticMethod)
                || (lazyNested && lazyNestedOnly->count(member.lazyNestedSpelling) == 0))
                continue;
            auto addSpelling = [&](const std::string& raw) {
                const std::string spelling = CxxMemberValueSpelling(raw);
                // A global-namespace class (MSVC's ::type_info behind `using ::type_info;` in std)
                // is spelled unqualified; request it like a qualified one.
                if (spelling.find("::") == std::string::npos
                    && !IsUnmappedGlobalCxxClassSpelling(spelling)
                    && !IsCxxGlobalClassTemplateSpecializationSpelling(spelling))
                    return;
                if (spelling.find("__cflat_user::") != std::string::npos
                    && GeneratedCxxPrefixForSpelling(spelling).empty())
                    return;
                if (spelling.find("type-parameter-") != std::string::npos)
                    return;
                TypeAndValue mapped;
                bool mappedForeign = false;
                const bool mapFound = TryMapCxxForeignSpelling(spelling, mapped, mappedForeign);
                if (mapFound && mappedForeign)
                {
                    successfulSpellings.insert(spelling);
                    return;
                }
                const std::string identity = CanonicalCxxForeignIdentity(spelling);
                if (!identity.empty())
                    requests.emplace(identity, spelling);
                if (!identity.empty() && &raw == &member.retType && raw.find('*') == std::string::npos
                    && !member.isConversion)
                    returnMembers.emplace(spelling, &member);
            };
            // A refusal can name only the first unmappable part of a dependent signature. For
            // example, Stack::apply_batch first reports its Example return before its vector
            // parameter is mapped. Request every class-template specialization in the complete
            // signature so the refreshed owner can bind the member in one retry.
            if (lazyNested)
            {
                // Only the selected signature's nested class: other spellings may be unrepairable.
                if (member.retType == member.lazyNestedSpelling) addSpelling(member.retType);
                for (const auto& spelling : member.paramTypes)
                    if (spelling == member.lazyNestedSpelling) addSpelling(spelling);
            }
            else if (unsupportedReturn || unsupportedParameter || incompleteReturn
                     || incompleteParameter)
            {
                addSpelling(member.retType);
                for (const auto& spelling : member.paramTypes) addSpelling(spelling);
            }
        }
        if (requests.empty() && successfulSpellings.empty()) return false;

        auto ownerIt = cxxTypeOwnerGroup_.find(typeName);
        if (ownerIt == cxxTypeOwnerGroup_.end()) return false;
        std::vector<size_t> dependencyGroups;
        auto addDependencyGroup = [&](const std::string& identity) {
            auto dependencyIt = cxxTypeOwnerGroup_.find(identity);
            if (dependencyIt == cxxTypeOwnerGroup_.end()
                || dependencyIt->second == ownerIt->second
                || dependencyIt->second >= cxxImportGroups_.size()
                || cxxImportGroups_[dependencyIt->second].headers.empty())
                return;
            if (std::find(dependencyGroups.begin(), dependencyGroups.end(), dependencyIt->second)
                == dependencyGroups.end())
                dependencyGroups.push_back(dependencyIt->second);
        };
        for (const auto& [identity, spelling] : requests)
        {
            addDependencyGroup(identity);
            const std::string squeezed = SqueezeCxxSpelling(spelling);
            for (const auto& [knownIdentity, knownSpelling] : cxxCflatToCxxSpelling_)
                if (squeezed.find(SqueezeCxxSpelling(knownSpelling)) != std::string::npos)
                    addDependencyGroup(knownIdentity);
        }
        CxxRequestGroup group = MakeCxxRequestGroup(ownerIt->second, dependencyGroups);
        if (group.headers.empty()) return false;
        CxxRequestGroupScope groupScope(*this, &group);
        std::string ownerSpelling;
        if (auto spelling = cxxCflatToCxxSpelling_.find(typeName);
            spelling != cxxCflatToCxxSpelling_.end())
            ownerSpelling = spelling->second;
        if (ownerSpelling.empty())
        {
            ownerSpelling = typeName;
            const size_t templateSep = ownerSpelling.find('$');
            if (templateSep != std::string::npos) ownerSpelling.erase(templateSep);
            for (size_t pos = 0; (pos = ownerSpelling.find('.', pos)) != std::string::npos; )
            { ownerSpelling.replace(pos, 1, "::"); pos += 2; }
        }
        for (const auto& [identity, spelling] : requests)
        {
            std::string error;
            if (RequestCxxForeignType(identity, spelling, error, /*needDefinitions*/ true,
                                      /*explicitInstantiation*/ true))
            {
                successfulSpellings.insert(spelling);
                continue;
            }
            /*
             * A member may return a class nested in a specialization whose NAME is private to it
             * (a range view's __iterator): C++ code outside the owner can use the type but never
             * spell it (clang refuses the request). Name it by the member call instead; every
             * later wrapper writes that spelling too. Only a failed request pays for this retry.
             */
            auto returning = returnMembers.find(spelling);
            if (returning == returnMembers.end() || spelling.find(">::") == std::string::npos)
                continue;
            const cflat_cinterop::RawCxxMember& owner = *returning->second;
            std::string call = owner.kind == cflat_cinterop::RawCxxMember::StaticMethod
                ? ownerSpelling + "::" + owner.name + "("
                : std::string("((") + (owner.isConst ? "const " : "") + ownerSpelling
                    + " *)nullptr)->" + owner.name + "(";
            const size_t firstParam =
                owner.kind == cflat_cinterop::RawCxxMember::StaticMethod ? 0 : 1;
            for (size_t p = firstParam; p < owner.paramTypes.size(); ++p)
            {
                if (p != firstParam) call += ", ";
                call += "static_cast<" + owner.paramTypes[p] + " &&>(*(__remove_reference_t("
                    + owner.paramTypes[p] + ") *)nullptr)";
            }
            call += ")";
            const std::string accessFree =
                std::string(cflat_cinterop::kCxxAccessFreeSpellingPrefix) + call + "))";
            std::string retryError;
            cxxForeignRequests_.erase(identity);
            if (activeCxxRequestGroup_ != nullptr)
                cxxForeignRequests_.erase(
                    identity + "|G" + CxxIncrementalGroupKey(*activeCxxRequestGroup_));
            if (RequestCxxForeignType(identity, accessFree, retryError, /*needDefinitions*/ true,
                                      /*explicitInstantiation*/ true))
            {
                successfulSpellings.insert(spelling);
                requestSpellings[spelling] = accessFree;
            }
        }
        if (successfulSpellings.empty()) return false;
        const std::string fileForLsp = recordIt->second.members.empty()
            ? std::string() : recordIt->second.members.front().file;
        const CxxClassInfo previousInfo = infoIt->second;
        const CRecordEntry previousRecord = recordIt->second;
        CRecordEntry reboundRecord = recordIt->second;
        if (incompleteReturn || incompleteParameter)
        {
            std::vector<CxxRequestItem> refreshItems;
            std::set<std::string> refreshNames;
            auto addRefreshItem = [&](const std::string& spelling) {
                if (spelling.find("::") == std::string::npos
                    || spelling.find('<') == std::string::npos)
                    return;
                const std::string identity = CanonicalCxxForeignIdentity(spelling);
                if (identity.empty() || identity == typeName || !refreshNames.insert(identity).second)
                    return;
                auto written = requestSpellings.find(spelling);
                refreshItems.push_back({ identity,
                    written == requestSpellings.end() ? spelling : written->second, true, true });
            };
            for (const auto& [identity, spelling] : requests)
            {
                auto written = requestSpellings.find(spelling);
                if (refreshNames.insert(identity).second)
                    refreshItems.push_back({ identity,
                        written == requestSpellings.end() ? spelling : written->second, true, true });
            }
            // A type used explicitly at the call site may already be registered by the time this
            // retry runs. It still must be a marker in the refreshed TU: otherwise Clang can keep
            // the owner's nested by-value return incomplete while recomputing its ABI.
            for (const std::string& spelling : successfulSpellings)
                addRefreshItem(spelling);
            refreshItems.push_back({ typeName, ownerSpelling, true, true });
            /*
             * A marker naming a generated [cpp] record needs its declaration in the TU. The live
             * group has it only if an earlier request parsed it there; when every earlier request
             * was served from the cache (a warm LSP analysis), it never did.
             */
            std::string refreshSpellings;
            for (const CxxRequestItem& item : refreshItems)
                refreshSpellings += item.cxxSpelling + "\n";
            const std::string refreshPrefix = GeneratedCxxPrefixForSpelling(refreshSpellings);
            cflat_cinterop::ExtractResult refreshed;
            std::string refreshError;
            if (RunCxxTypeRequests(group, refreshItems, {}, /*emitDefinitions*/ true,
                                    refreshed, refreshError, refreshPrefix))
            {
                std::vector<CRecordEntry> mapped;
                MapRawRecords(refreshed, mapped);
                for (const CRecordEntry& candidate : mapped)
                    if (candidate.name == typeName)
                    { reboundRecord = candidate; break; }
            }
        }
        for (auto& member : reboundRecord.members)
        {
            if (member.name != memberName
                || (member.kind != cflat_cinterop::RawCxxMember::Instance
                    && member.kind != cflat_cinterop::RawCxxMember::StaticMethod))
                continue;
            auto wasRequested = [&](const std::string& raw) {
                const std::string spelling = CxxMemberValueSpelling(raw);
                return successfulSpellings.count(spelling) != 0;
            };
            const bool requestedSignatureType = wasRequested(member.retType)
                || std::any_of(member.paramTypes.begin(), member.paramTypes.end(), wasRequested);
            if ((unsupportedReturn || unsupportedParameter || incompleteReturn || incompleteParameter)
                && requestedSignatureType)
                member.bindRefusal.clear();
        }

        // Refreshed members register the same deferred wrappers as an imported record.
        EnsureCxxRecordLayoutsForMemberSignatures(reboundRecord, memberName);
        // Re-registration records the siblings that stay refused (private, deleted, unrelated).
        if (auto live = cxxClasses_.find(typeName); lazyNested && live != cxxClasses_.end())
            live->second.refusedMembers.erase(memberName);
        RegisterCxxClassMembers(reboundRecord, fileForLsp, memberName);
        auto updated = cxxClasses_.find(typeName);
        if (updated == cxxClasses_.end()) return false;
        const bool instanceBound = std::find(updated->second.instanceMethodNames.begin(),
                                             updated->second.instanceMethodNames.end(),
                                             memberName)
                                != updated->second.instanceMethodNames.end();
        bool staticBound = false;
        if (!instanceBound)
        {
            auto functions = functionTable.find(typeName + "." + memberName);
            if (functions != functionTable.end())
                for (const auto& function : functions->second)
                    if (function.IsCxx && function.External) { staticBound = true; break; }
        }
        if (!instanceBound && !staticBound)
        {
            cxxClasses_[typeName] = previousInfo;
            StoreCxxRecordEntry(typeName, previousRecord);
            return false;
        }

        bool classTypedDefault = false;
        for (const auto& member : reboundRecord.members)
        {
            if (member.name != memberName
                || (member.kind != cflat_cinterop::RawCxxMember::Instance
                    && member.kind != cflat_cinterop::RawCxxMember::StaticMethod))
                continue;
            for (size_t n = 0; n < member.paramTypes.size()
                              && n < member.defaultArgs.size(); ++n)
            {
                if (member.defaultArgs[n].kind != "nonconst"
                    || !HasNonConstDefaultSuffix(member.defaultArgs, n))
                    continue;
                TypeAndValue param;
                if (MapCTypeToTypeAndValue(member.paramTypes[n], param, true)
                    && dataStructures.count(param.TypeName) != 0)
                    classTypedDefault = true;
            }
        }
        bool defaultWrapperBound = false;
        const std::string functionName = instanceBound ? memberName : typeName + "." + memberName;
        if (auto functions = functionTable.find(functionName); functions != functionTable.end())
            for (const auto& function : functions->second)
                if (function.UniqueName.starts_with("__cflat_dflt_")
                    && (!instanceBound || (!function.Parameters.empty()
                        && function.Parameters[0].TypeName == typeName)))
                {
                    defaultWrapperBound = true;
                    break;
                }
        if (classTypedDefault && defaultWrapperBound && verbose)
            std::cout << std::format(
                "[verbose]   C++ member {}.{} default wrapper bound on first use\n",
                typeName, memberName);

        if (!lazyNested) updated->second.refusedMembers.erase(memberName);
        if (instanceBound
            && std::find(updated->second.instanceMethodNames.begin(),
                         updated->second.instanceMethodNames.end(), memberName)
                   == updated->second.instanceMethodNames.end())
            updated->second.instanceMethodNames.push_back(memberName);
        return true;
    }

/*
 * M6 - make every method a class INHERITS callable on the derived type.
 *
 * A CFlat instance method is resolved by matching the receiver against Parameters[0].TypeName, so
 * a base's method registered with `this` typed as the BASE is invisible on a derived receiver.
 * Rather than teach the member-call path to walk a base graph, each public base's methods are
 * cloned into the derived class's overload set with `this` retyped - same llvm::Function, same
 * clang arrangement, same vtable slot - and the base subobject offset is recorded in
 * cxxThisAdjust_, which the call path adds to `this` right before the call. A non-primary base
 * therefore gets the correct adjusted `this` (Clang's own `getelementptr i8, ptr %obj, N`), and a
 * method the derived class OVERRIDES is left alone: its own registration already won.
 *
 * Records arrive base-before-derived (clang visits them in source order), so a base's own
 * inherited clones are already present when its derived class is processed - which is what makes
 * this work transitively without a second pass.
 */
/*
 * The CFlat identity of a C++ base, which is not always `b.name`.
 * A base that is a class template SPECIALIZATION is referenced by its '$'-mangled identity
 * (torch.nn.Cloneable$torch.nn.LinearImpl) but, unless the specialization was explicitly
 * requested, its own record registered under the bare template name (torch.nn.Cloneable) -
 * clang's qualified name for a specialization decl drops the arguments. Try the canonical-
 * spelling map next, which records the full spelling against whatever identity the record took,
 * and the bare template name last: the spelling map only helps once the specialization's own
 * record has been registered, and records do NOT arrive base-before-derived for a template base.
 * Every specialization shares the one shell anyway.
 */
std::string LLVMBackend::ResolveCxxBaseIdentity(const cflat_cinterop::RawCxxBase& b) const
{
        // A base that IS a class-template specialization must consult the canonical-spelling map
        // FIRST: b.name is the argument-less placeholder every specialization of that template
        // shares, so preferring it would hand back the FIRST specialization's members. Once some
        // request has registered this specialization under its own '$' identity the spelling map
        // names it; otherwise the lookup lands back on the placeholder, as before.
        const bool specializationBase = b.canonicalType.find('<') != std::string::npos;
        if (!specializationBase && cxxClasses_.count(b.name) != 0) return b.name;
        if (!b.canonicalType.empty())
        {
            auto sp = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(b.canonicalType));
            if (sp != cxxForeignTypeSpellings_.end() && cxxClasses_.count(sp->second) != 0)
                return sp->second;
        }
        if (cxxClasses_.count(b.name) != 0) return b.name;
        const size_t sep = b.name.find('$');
        if (sep != std::string::npos && sep > 0 && cxxClasses_.count(b.name.substr(0, sep)) != 0)
            return b.name.substr(0, sep);
        return std::string();
}

std::string LLVMBackend::ResolveCxxBaseRecordName(const cflat_cinterop::RawCxxBase& b) const
{
        // Same fallbacks as ResolveCxxBaseIdentity, against the record table, so a base that is
        // still a pending projection (not yet in cxxClasses_) is found too.
        const bool specializationBase = b.canonicalType.find('<') != std::string::npos;
        if (!specializationBase && cxxRecordEntries_.count(b.name) != 0) return b.name;
        if (!b.canonicalType.empty())
        {
            auto sp = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(b.canonicalType));
            if (sp != cxxForeignTypeSpellings_.end() && cxxRecordEntries_.count(sp->second) != 0)
                return sp->second;
        }
        if (cxxRecordEntries_.count(b.name) != 0) return b.name;
        const size_t sep = b.name.find('$');
        if (sep != std::string::npos && sep > 0 && cxxRecordEntries_.count(b.name.substr(0, sep)) != 0)
            return b.name.substr(0, sep);
        return std::string();
}

std::string LLVMBackend::ResolveCxxBaseIdentity(const CxxClassInfo::BaseRef& b) const
{
        cflat_cinterop::RawCxxBase raw;
        raw.name = b.name;
        raw.canonicalType = b.canonicalType;
        return ResolveCxxBaseIdentity(raw);
}

static std::string CxxBaseMethodSignatureKey(
    const LLVMBackend::CxxClassInfo::Method& method)
{
        std::string key = method.raw.name + (method.raw.isConst ? "|const" : "|mut")
            + "|thisrq=" + std::to_string(method.raw.refQualifier);
        for (size_t i = 1; i < method.raw.paramTypes.size(); ++i)
            key += "|" + method.raw.paramTypes[i];
        return key;
}

std::vector<LLVMBackend::CxxClassInfo::Method> LLVMBackend::FindCxxBaseMethods(
    const std::string& baseType, const std::string& methodName) const
{
        std::vector<CxxClassInfo::Method> result;
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> methodsSeen;
        std::function<void(const std::string&)> visit = [&](const std::string& typeName) {
            if (!visited.insert(typeName).second) return;
            auto it = cxxClasses_.find(typeName);
            if (it == cxxClasses_.end()) return;
            for (const auto& method : it->second.directMethods)
                if (method.raw.kind == cflat_cinterop::RawCxxMember::Instance
                    && method.raw.name == methodName
                    && methodsSeen.insert(CxxBaseMethodSignatureKey(method)).second)
                    result.push_back(method);
            for (const auto& base : it->second.bases)
            {
                const std::string identity = ResolveCxxBaseIdentity(base);
                if (!identity.empty()) visit(identity);
            }
        };
        visit(baseType);
        return result;
}

std::vector<LLVMBackend::CxxClassInfo::Method> LLVMBackend::FindCxxBaseVirtualMethods(
    const std::string& baseType) const
{
        std::vector<CxxClassInfo::Method> result;
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> methodsSeen;
        std::function<void(const std::string&)> visit = [&](const std::string& typeName) {
            if (!visited.insert(typeName).second) return;
            auto it = cxxClasses_.find(typeName);
            if (it == cxxClasses_.end()) return;
            for (const auto& method : it->second.directMethods)
                if (method.raw.isVirtual
                    && methodsSeen.insert(CxxBaseMethodSignatureKey(method)).second)
                    result.push_back(method);
            for (const auto& base : it->second.bases)
            {
                const std::string identity = ResolveCxxBaseIdentity(base);
                if (!identity.empty()) visit(identity);
            }
        };
        visit(baseType);
        return result;
}

bool LLVMBackend::CxxBaseHasAccessibleMoveOrCopy(const std::string& baseType) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(baseType, "__ctor");
        const auto* info = GetCxxClassInfo(baseType);
        if (info == nullptr) return false;
        for (const auto& ctor : info->constructors)
            if ((ctor.isMoveCtor || ctor.isCopyCtor) && !ctor.isDeleted
                && ctor.access != cflat_cinterop::AccessPrivate)
                return true;
        return info->hasCopyCtor && !info->hasDeletedCopyCtor;
}

bool LLVMBackend::CxxClassOverrideParamIsBorrowed(const std::string& typeName) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(typeName, "__ctor");
        const auto* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return false;

        bool canTransfer = false;
        for (const auto& ctor : info->constructors)
        {
            if (!ctor.isCopyCtor && !ctor.isMoveCtor) continue;
            if (ctor.isDeleted || ctor.access != cflat_cinterop::AccessPublic) continue;
            // The generated wrapper can forward through T&& or const T&, but not T&.
            if (ctor.isMoveCtor
                || (ctor.isCopyCtor && ctor.params.size() == 2
                    && ctor.params[1].CxxConstRef()))
                canTransfer = true;
        }
        return !canTransfer;
}

void LLVMBackend::RecordCppStructBase(const std::string& structName,
                                      const std::string& baseName)
{
        if (!structName.empty() && !baseName.empty()) cppStructBases_[structName] = baseName;
}

void LLVMBackend::RegisterCppStructName(const std::string& structName)
{
        if (!structName.empty()) cppStructNames_.insert(structName);
}

bool LLVMBackend::IsCppStructName(const std::string& structName) const
{
        return cppStructNames_.count(structName) != 0;
}

/*
 * A C++ identity naming a [cpp] struct as a template argument spells it as clang sees it
 * (`std.weak_ptr$__cflat_user.<encoded ns.Self>`). Publish it under the CFlat spelling the program
 * writes (`std.weak_ptr$ns.Self`) so both name ONE registration. The struct's own record
 * (`__cflat_user.Self`, no '$' before it) is left alone.
 */
std::string LLVMBackend::CanonicalCxxForeignIdentity(const std::string& spelling) const
{
        return CanonicalGeneratedCxxIdentity(cflat_cinterop::CxxForeignIdentity(spelling));
}

std::string LLVMBackend::CanonicalGeneratedCxxIdentity(const std::string& identity) const
{
        static const std::string user = "__cflat_user.";
        if (cppStructNames_.empty() || identity.find(user) == std::string::npos) return identity;
        std::string out = identity;
        for (size_t pos = 0; (pos = out.find(user, pos)) != std::string::npos; )
        {
            const size_t start = pos + user.size();
            size_t end = start;
            while (end < out.size()
                   && (std::isalnum((unsigned char)out[end]) || out[end] == '_'))
                ++end;
            const std::string token = out.substr(start, end - start);
            // The encoding is injective: decode once, then require a registered [cpp] struct.
            std::string decoded;
            const bool known = cflat_cinterop::DecodeGeneratedCflatStructIdentifier(token, decoded)
                && IsCppStructName(decoded);
            if (!known || pos == 0 || out[pos - 1] != '$')
            {
                pos = end;
                continue;
            }
            out.replace(pos, end - pos, decoded);
            pos += decoded.size();
        }
        return out;
}

void LLVMBackend::RegisterCppStructProtectedBaseMembers(const std::string& structName)
{
        std::string baseName;
        if (!GetCppStructBase(structName, baseName)) return;

        std::unordered_set<std::string> visited;
        std::function<void(const std::string&)> visit = [&](const std::string& typeName) {
            if (!visited.insert(typeName).second) return;
            auto infoIt = cxxClasses_.find(typeName);
            auto recordIt = cxxRecordEntries_.find(typeName);
            if (infoIt == cxxClasses_.end() || recordIt == cxxRecordEntries_.end()) return;
            for (const auto& base : infoIt->second.bases)
            {
                const std::string identity = ResolveCxxBaseIdentity(base);
                if (!identity.empty()) visit(identity);
            }
            for (const auto& member : recordIt->second.members)
            {
                if (member.kind != cflat_cinterop::RawCxxMember::Instance
                    || member.access != cflat_cinterop::AccessProtected)
                    continue;
                RegisterCxxClassMembers(recordIt->second, member.file.empty()
                    ? std::string() : member.file, member.name);
            }
            if (auto refreshed = cxxRecordEntries_.find(typeName);
                refreshed != cxxRecordEntries_.end())
                RegisterCxxInheritedMembers(refreshed->second);
        };
        visit(baseName);

        auto derived = cxxRecordEntries_.find(structName);
        if (derived == cxxRecordEntries_.end()) return;
        for (int round = 0; round < 8; ++round)
            if (!RegisterCxxInheritedMembers(derived->second)) break;
    }

bool LLVMBackend::GetCppStructBase(const std::string& structName, std::string& baseName) const
{
        auto it = cppStructBases_.find(structName);
        if (it == cppStructBases_.end()) return false;
        baseName = it->second;
        return true;
}

bool LLVMBackend::GetCxxTypeOwnerGroup(const std::string& typeName, size_t& group) const
{
        auto it = cxxTypeOwnerGroup_.find(typeName);
        if (it == cxxTypeOwnerGroup_.end()) return false;
        group = it->second;
        return true;
}

bool LLVMBackend::IsCxxConstructingStructor(const std::string& typeName,
                                            const CxxClassInfo::Structor& st)
{
        // Every structor EmitCxxStructorCall sees constructs, except the destructor and the
        // assignment operators (the object already exists).
        const CxxClassInfo* info = GetCxxClassInfo(ResolveTypeAlias(typeName));
        if (info == nullptr) return true;
        const std::string& name = st.linkageName;
        if (name == info->destructor.linkageName || name == info->copyAssign.linkageName
            || name == info->moveAssign.linkageName)
            return false;
        for (const auto* list : { &info->copyAssignOverloads, &info->moveAssignOverloads })
            for (const CxxClassInfo::Structor& op : *list)
                if (op.linkageName == name) return false;
        for (const auto& [key, transfer] : info->xvalueTransfers)
            if (key.starts_with("assign") && transfer.linkageName == name) return false;
        return true;
}

std::string LLVMBackend::ValidateCxxVTableDeleteDemand(const std::string& typeName)
{
        // The verdict was computed at extraction and cached with the record, so a warm compile
        // refuses here exactly like a cold one, with no live group and no clang request.
        const std::string resolvedType = ResolveTypeAlias(typeName);
        if (!CxxHasVirtualDestructor(resolvedType)) return {};
        const CxxClassInfo* info = GetCxxClassInfo(resolvedType);
        if (info == nullptr || info->vtableDeleteVerdict.empty()) return {};
        return "clang: " + info->vtableDeleteVerdict;
}

static std::string CppStructOverrideNameKey(const std::string& structName,
                                            const std::string& methodName,
                                            const std::vector<LLVMBackend::TypeAndValue>& params)
{
        std::string key = structName + "#" + methodName + "#";
        for (const auto& param : params)
            key += param.TypeName + ":" + std::to_string(param.Pointer) + ":"
                + std::to_string(param.ValuePointerDepth()) + ":"
                + std::to_string(param.IsRvalueRef) + ";";
        return key;
}

void LLVMBackend::RegisterCppStructOverrideName(
    const std::string& structName, const std::string& methodName,
    const std::vector<TypeAndValue>& params, const std::string& stableName)
{
        cppStructOverrideNames_[CppStructOverrideNameKey(structName, methodName, params)] = stableName;
}

bool LLVMBackend::GetCppStructOverrideName(
    const std::string& structName, const std::string& methodName,
    const std::vector<TypeAndValue>& params, std::string& stableName) const
{
        auto it = cppStructOverrideNames_.find(
            CppStructOverrideNameKey(structName, methodName, params));
        if (it == cppStructOverrideNames_.end()) return false;
        stableName = it->second;
        return true;
}

static bool SameCppMappedType(const LLVMBackend::TypeAndValue& left,
                              const LLVMBackend::TypeAndValue& right)
{
        return left.TypeName == right.TypeName
            && left.Pointer == right.Pointer
            && left.ValuePointerDepth() == right.ValuePointerDepth()
            && left.IsArrayView == right.IsArrayView
            && left.IsRvalueRef == right.IsRvalueRef;
}

llvm::Function* LLVMBackend::EmitCppStructOverrideThunk(
    llvm::Function* original, const std::string& structName,
    const std::string& methodName, const std::vector<TypeAndValue>& params)
{
        if (original == nullptr) return nullptr;
        std::string baseName;
        std::string stableName;
        if (!GetCppStructBase(structName, baseName))
            baseName.clear();
        if (!GetCppStructOverrideName(structName, methodName, params, stableName))
            return nullptr;
        if (stableName.starts_with("__cflat_mth_"))
        {
            const CSigEntry* helperSig = nullptr;
            for (const auto& [name, signatures] : cxxFunctionSignatures_)
            {
                (void)name;
                auto found = std::find_if(signatures.begin(), signatures.end(),
                    [&](const CSigEntry& sig) {
                        return (sig.linkageName == stableName || sig.name == stableName)
                            && sig.abi.valid && sig.params.size() == params.size() + 1;
                    });
                if (found != signatures.end()) { helperSig = &*found; break; }
            }
            if (helperSig == nullptr) return nullptr;
            CxxFunctionPointerAbiPlan plan;
            plan.ret = helperSig->ret;
            plan.params = helperSig->params;
            plan.params[0].TypeName = structName;
            plan.params[0].Pointer = true;
            std::string mismatch;
            CxxAbiPlanScope abiScope(*this, &helperSig->abi, &mismatch);
            if (!BuildAbiRecipeFromClangPlan(methodName, helperSig->abi,
                                             plan.ret, plan.params, plan.recipe))
            {
                LogErrorMessage("C++ method '{}.{}' has an unsupported ABI: {}",
                                { structName, methodName,
                                  mismatch.empty() ? "unknown ABI mismatch" : mismatch });
                return nullptr;
            }
            return GetOrCreateReverseAbiFunctionThunk(original, plan, stableName, true);
        }
        if (baseName.empty()) return nullptr;
        for (const auto& candidate : FindCxxBaseMethods(baseName, methodName))
        {
            if (candidate.raw.abi.valid)
            {
                if (candidate.params.size() != params.size() + 1) continue;
                bool same = true;
                for (size_t i = 0; i < params.size(); ++i)
                    if (!SameCppMappedType(candidate.params[i + 1], params[i])) same = false;
                if (!same) continue;
                CxxFunctionPointerAbiPlan plan;
                plan.ret = candidate.ret;
                plan.params = candidate.params;
                plan.params[0].TypeName = structName;
                plan.params[0].Pointer = true;
                std::string mismatch;
                const cflat_cinterop::RawAbi* helperAbiPlan = nullptr;
                AbiRecipe fallbackHelperRecipe;
                const FunctionSymbol* helperSymbol = nullptr;
                if (auto* helperFunction = module->getFunction(stableName))
                    if ((helperSymbol = GetFunctionSymbol(helperFunction)) != nullptr
                        && helperSymbol->CxxAbi.valid)
                    {
                        helperAbiPlan = &helperSymbol->CxxAbi;
                        plan.ret = helperSymbol->ReturnType;
                        plan.params = helperSymbol->Parameters;
                        if (!plan.params.empty())
                        {
                            plan.params[0].TypeName = structName;
                            plan.params[0].Pointer = true;
                        }
                    }
                const CSigEntry* helperSig = nullptr;
                for (const auto& [name, signatures] : cxxFunctionSignatures_)
                {
                    if (helperAbiPlan != nullptr) break;
                    (void)name;
                    auto found = std::find_if(signatures.begin(), signatures.end(),
                        [&](const CSigEntry& sig) {
                            return (sig.linkageName == stableName || sig.name == stableName)
                                && sig.abi.valid
                                && sig.params.size() == plan.params.size();
                        });
                    if (found != signatures.end()) { helperSig = &*found; break; }
                }
                if (helperAbiPlan == nullptr)
                {
                    if (helperSig != nullptr)
                    {
                        helperAbiPlan = &helperSig->abi;
                        plan.ret = helperSig->ret;
                        plan.params = helperSig->params;
                        if (!plan.params.empty())
                        {
                            plan.params[0].TypeName = structName;
                            plan.params[0].Pointer = true;
                        }
                    }
                    else
                    {
                        // The thunk calls a free C helper: classify its actual signature,
                        // including Self*, instead of borrowing the base member arrangement.
                        fallbackHelperRecipe = ComputeAbiRecipe(plan.ret, plan.params);
                        AbiRecipe cxxValueRecipe = ComputeCxxReturnAbiRecipe(plan.ret, plan.params);
                        if (cxxValueRecipe.retSlot.kind == AbiSlot::SRetReturn)
                            fallbackHelperRecipe.retSlot = cxxValueRecipe.retSlot;
                        for (size_t i = 0; i < fallbackHelperRecipe.paramSlots.size()
                             && i < cxxValueRecipe.paramSlots.size(); ++i)
                            if (cxxValueRecipe.paramSlots[i].kind == AbiSlot::ByVal)
                                fallbackHelperRecipe.paramSlots[i] = cxxValueRecipe.paramSlots[i];
                    }
                }
                bool builtRecipe = false;
                if (helperAbiPlan != nullptr)
                {
                    CxxAbiPlanScope abiScope(*this, helperAbiPlan, &mismatch);
                    builtRecipe = BuildAbiRecipeFromClangPlan(methodName, *helperAbiPlan,
                                                               plan.ret, plan.params,
                                                               plan.recipe);
                }
                else
                {
                    plan.recipe = std::move(fallbackHelperRecipe);
                    builtRecipe = true;
                }
                if (!builtRecipe)
                {
                    LogErrorMessage("override method '{}.{}' has an unsupported C++ ABI: {}",
                                    { structName, methodName,
                                      mismatch.empty() ? "unknown ABI mismatch" : mismatch });
                    return nullptr;
                }
                return GetOrCreateReverseAbiFunctionThunk(original, plan, stableName, true);
            }
        }
        return nullptr;
}

/*
 * A method a class INHERITS is cloned into the derived overload set only when the base bound it.
 * A base member that was REFUSED is therefore invisible from the derived class - the refusal
 * lives on the base, so the derived class's refusedMembers says nothing and the ordinary retry
 * above never runs. Rebind on the base (recursively, for a deeper hierarchy) and re-run the
 * inheritance clone so the newly bound overload reaches the derived receiver.
 */
/*
 * A base that is a class-template SPECIALIZATION (`NormBase<1, Norm1>`, `BatchNormImplBase<1,
 * BatchNorm1dImpl>`) arrives from the header walk under the argument-less placeholder every
 * specialization of that template shares, whose members are the FIRST specialization's - and an
 * inline member of a class template has no body in the header TU at all until some request
 * ODR-uses it for concrete arguments. The derived record's base entry carries the full canonical
 * spelling, so request THAT specialization under its own '$' identity with definitions. When the
 * placeholder already stands for exactly these arguments RequestCxxForeignType aliases the new
 * identity onto it and rebinds the refused members there, which is equally correct.
 */
void LLVMBackend::RequestCxxSpecializationBase(const std::string& derivedName,
                                               const cflat_cinterop::RawCxxBase& b)
{
        if (b.canonicalType.find('<') == std::string::npos) return;
        const std::string spelling = CxxMemberValueSpelling(b.canonicalType);
        if (spelling.empty()) return;
        const std::string identity = CanonicalCxxForeignIdentity(spelling);
        if (identity.empty() || identity == derivedName) return;
        if (cxxClasses_.count(identity) != 0 || cxxForeignRequests_.count(identity) != 0) return;
        auto ownerIt = cxxTypeOwnerGroup_.find(derivedName);
        if (ownerIt == cxxTypeOwnerGroup_.end()) return;
        CxxRequestGroup group = MakeCxxRequestGroup(ownerIt->second, {});
        if (group.headers.empty()) return;
        CxxRequestGroupScope groupScope(*this, &group);
        std::string error;
        RequestCxxForeignType(identity, spelling, error, /*needDefinitions*/ true,
                              /*explicitInstantiation*/ true);
}

bool LLVMBackend::TryBindRefusedCxxBaseMember(const std::string& typeName,
                                              const std::string& memberName)
{
        auto recordIt = cxxRecordEntries_.find(typeName);
        if (recordIt == cxxRecordEntries_.end() || recordIt->second.bases.empty()) return false;
        const std::string guardKey = typeName + "." + memberName;
        if (!cxxInheritedRebindInFlight_.insert(guardKey).second) return false;
        // Bound as an instance method, or as a static reached through the class name.
        auto hasBoundMember = [&](const std::string& owner) {
            auto info = cxxClasses_.find(owner);
            if (info == cxxClasses_.end()) return false;
            if (std::find(info->second.instanceMethodNames.begin(),
                          info->second.instanceMethodNames.end(), memberName)
                != info->second.instanceMethodNames.end())
                return true;
            auto functions = functionTable.find(owner + "." + memberName);
            return functions != functionTable.end()
                && std::any_of(functions->second.begin(), functions->second.end(),
                               [](const FunctionSymbol& f) { return f.IsCxx && !f.IsMethod; });
        };
        bool bound = false;
        for (const auto& b : recordIt->second.bases)
        {
            if (b.access != cflat_cinterop::AccessPublic) continue;
            RequestCxxSpecializationBase(typeName, b);
            const std::string baseName = ResolveCxxBaseIdentity(b);
            if (baseName.empty() || baseName == typeName) continue;
            if (TryBindRefusedCxxMember(baseName, memberName)) bound = true;
            // The specialization request above may have bound the member on the base already
            // (the alias path rebinds refused members and clears the refusal), in which case the
            // retry has nothing left to do but the derived class still needs the clone.
            else if (hasBoundMember(baseName))
                bound = true;
        }
        cxxInheritedRebindInFlight_.erase(guardKey);
        if (!bound) return false;
        RegisterCxxInheritedMembers(cxxRecordEntries_.find(typeName)->second);
        return hasBoundMember(typeName);
}

bool LLVMBackend::RegisterCxxInheritedMembers(const CRecordEntry& r)
{
        if (!r.isCxx || !r.layoutRefusal.empty() || r.bases.empty()) return false;
        auto self = cxxClasses_.find(r.name);
        if (self == cxxClasses_.end()) return false;
        bool added = false;

        // Signature key of one overload, ignoring `this` - two members with the same key are the
        // same method, so the most derived declaration wins.
        auto sigKey = [](const std::string& name, const std::vector<TypeAndValue>& params,
                         int refQualifier) {
            std::string k = name;
            k += "|thisrq=" + std::to_string(refQualifier);
            for (size_t i = 1; i < params.size(); ++i)
                k += "|" + params[i].TypeName + (params[i].Pointer ? "*" : "");
            return k;
        };
        auto rawOverloadKey = [](const cflat_cinterop::RawCxxMember& method) {
            std::string k = method.name + (method.isConst ? "|const" : "|mut")
                + (method.isVolatile ? "|volatile" : "")
                + "|rq=" + std::to_string(method.refQualifier)
                + (method.variadic ? "|va" : "");
            for (size_t i = 1; i < method.paramTypes.size(); ++i)
                k += "|" + method.paramTypes[i];
            return k;
        };

        // Records the name on the derived class's member list. A base member that is ALREADY in
        // the overload set (cloned by an earlier record that shares this CFlat identity) still
        // has to be listed here: RegisterCxxClassMembers rebuilds CxxClassInfo from scratch for
        // every record, so a shell shared by several template specializations loses the list on
        // each new specialization while functionTable keeps the symbols.
        std::set<std::string> listed(self->second.instanceMethodNames.begin(),
                                     self->second.instanceMethodNames.end());
        auto noteMethodName = [&](const std::string& mn, int access = cflat_cinterop::AccessPublic) {
            if (!listed.insert(mn).second) return;
            self->second.instanceMethodNames.push_back(mn);
            auto it = self->second.memberAccess.find(mn);
            if (it == self->second.memberAccess.end() || access < it->second)
                self->second.memberAccess[mn] = access;
        };

        // [class.member.lookup]: a name this class declares hides every base member of that name;
        // a using-declaration lists the base overloads among the class's own members.
        std::set<std::string> declaredNames;
        // name -> base linkage -> access of the using-declaration naming it. A using-declaration
        // brings in only the base overloads it names; other overloads of that name stay hidden.
        std::map<std::string, std::map<std::string, int>> usingShadows;
        // Fall back to a matching base declaration when this class's virtual slot is unnameable.
        std::set<std::string> refusedVirtualSlotNames;
        for (const auto& m : r.members)
            if ((m.kind == cflat_cinterop::RawCxxMember::Instance
                 || m.kind == cflat_cinterop::RawCxxMember::StaticMethod)
                && !m.isConversion && !m.isImplicit)
            {
                if (m.isUsingShadow) usingShadows[m.name][m.linkageName] = m.access;
                else declaredNames.insert(m.name);
            }
        for (const auto& [mn, refusal] : self->second.refusedMembers)
            if (refusal == "is virtual but cflat could not determine its vtable slot")
                refusedVirtualSlotNames.insert(mn);
        // A base overload of a using-declared name is admitted only when a using-declaration of
        // this class names it at public access. A non-public one is refused by its own member.
        auto usingAdmits = [&](const std::string& mn, const std::string& linkage) {
            auto names = usingShadows.find(mn);
            if (names == usingShadows.end()) return false;
            auto shadow = names->second.find(linkage);
            return shadow != names->second.end()
                && shadow->second == cflat_cinterop::AccessPublic;
        };

        std::set<std::string> present;
        for (const std::string& mn : self->second.instanceMethodNames)
        {
            auto fit = functionTable.find(mn);
            if (fit == functionTable.end()) continue;
            for (const FunctionSymbol& sym : fit->second)
                if (sym.IsMethod && !sym.Parameters.empty() && sym.Parameters[0].TypeName == r.name)
                    present.insert(sigKey(mn, sym.Parameters, sym.CxxRefQualifier));
        }

        for (const auto& b : r.bases)
        {
            if (b.access != cflat_cinterop::AccessPublic) continue;
            const std::string baseName = ResolveCxxBaseIdentity(b);
            if (baseName.empty() || baseName == r.name) continue;
            auto bit = cxxClasses_.find(baseName);
            if (bit == cxxClasses_.end() || bit == self) continue;
            const std::vector<std::string> baseStaticNames = bit->second.staticMethodNames;
            for (const std::string& mn : baseStaticNames)
            {
                const bool usingNamed = usingShadows.count(mn) != 0;
                if (declaredNames.count(mn) != 0 && !usingNamed) continue;
                auto accessIt = bit->second.memberAccess.find(mn);
                const int access = accessIt == bit->second.memberAccess.end()
                    ? cflat_cinterop::AccessPublic : accessIt->second;
                if (access != cflat_cinterop::AccessPublic)
                {
                    auto currentAccess = self->second.memberAccess.find(mn);
                    if (currentAccess == self->second.memberAccess.end()
                        || access < currentAccess->second)
                        self->second.memberAccess[mn] = access;
                    self->second.refusedMembers[mn] = access == cflat_cinterop::AccessPrivate
                        ? "is private" : "is protected";
                    continue;
                }
                auto fit = functionTable.find(baseName + "." + mn);
                if (fit == functionTable.end()) continue;
                // No admitted overload: create no entry, so the direct member's refusal still speaks.
                if (usingNamed && std::none_of(fit->second.begin(), fit->second.end(),
                        [&](const FunctionSymbol& baseSym) {
                            return !baseSym.IsMethod && baseSym.IsCxx
                                && usingAdmits(mn, baseSym.UniqueName);
                        }))
                    continue;
                const std::string inheritedName = r.name + "." + mn;
                auto& target = functionTable[inheritedName];
                for (const FunctionSymbol& baseSym : fit->second)
                {
                    if (baseSym.IsMethod || !baseSym.IsCxx) continue;
                    if (usingNamed && !usingAdmits(mn, baseSym.UniqueName)) continue;
                    auto existing = std::find_if(target.begin(), target.end(),
                        [&](const FunctionSymbol& sym) {
                            return sym.UniqueName == baseSym.UniqueName;
                        });
                    if (existing != target.end()) continue;
                    FunctionSymbol inheritedSym = baseSym;
                    target.push_back(std::move(inheritedSym));
                    added = true;
                }
                if (!target.empty())
                {
                    if (std::find(self->second.staticMethodNames.begin(),
                                  self->second.staticMethodNames.end(), mn)
                        == self->second.staticMethodNames.end())
                        self->second.staticMethodNames.push_back(mn);
                    self->second.memberAccess[mn] = cflat_cinterop::AccessPublic;
                }
            }
            // Snapshot: the clone loop appends to self->second.instanceMethodNames, and the
            // base's list is the same vector when two specializations collapse onto one shell.
            const std::vector<std::string> baseMethodNames = bit->second.instanceMethodNames;
            for (const std::string& mn : baseMethodNames)
            {
                auto fit = functionTable.find(mn);
                if (fit == functionTable.end()) continue;
                // Snapshot: the loop below appends to this same overload vector.
                std::vector<FunctionSymbol> fromBase;
                for (const FunctionSymbol& sym : fit->second)
                {
                    if (!sym.IsMethod || sym.Parameters.empty()) continue;
                    // Re-seed the dedup set from the overload set itself: a specialization that
                    // collapsed onto an already-populated shell arrives with a FRESH CxxClassInfo,
                    // so instanceMethodNames no longer lists clones a sibling already made. Without
                    // this the same base method is cloned once per specialization.
                    if (sym.Parameters[0].TypeName == r.name)
                    {
                        present.insert(sigKey(mn, sym.Parameters, sym.CxxRefQualifier));
                        noteMethodName(mn);
                    }
                    if (sym.IsCxx && sym.Parameters[0].TypeName == baseName
                        && (usingShadows.count(mn) != 0
                                ? usingAdmits(mn, sym.UniqueName)
                                : (declaredNames.count(mn) == 0
                                   || refusedVirtualSlotNames.count(mn) != 0)))
                        fromBase.push_back(sym);
                }
                for (FunctionSymbol sym : fromBase)
                {
                    const std::string key = sigKey(mn, sym.Parameters, sym.CxxRefQualifier);
                    bool conflictingInheritedOperator = false;
                    if (mn.starts_with("operator"))
                    {
                        const auto candidates = functionTable.find(mn);
                        if (candidates != functionTable.end())
                        {
                            const bool alreadyCloned = std::any_of(
                                candidates->second.begin(), candidates->second.end(),
                                [&](const FunctionSymbol& candidate) {
                                    return candidate.IsCxx && candidate.IsMethod
                                        && !candidate.Parameters.empty()
                                        && candidate.Parameters[0].TypeName == r.name
                                        && candidate.UniqueName == sym.UniqueName;
                                });
                            if (alreadyCloned) continue;
                            conflictingInheritedOperator = present.count(key) != 0
                                && std::any_of(candidates->second.begin(), candidates->second.end(),
                                    [&](const FunctionSymbol& candidate) {
                                        return candidate.IsCxx && candidate.IsMethod
                                            && !candidate.Parameters.empty()
                                            && candidate.Parameters[0].TypeName == r.name
                                            && sigKey(mn, candidate.Parameters,
                                                      candidate.CxxRefQualifier) == key
                                            && candidate.UniqueName != sym.UniqueName;
                                    });
                            if (conflictingInheritedOperator)
                            {
                                const std::string sourceOwner = sym.CxxInheritedOwner.empty()
                                    ? baseName : sym.CxxInheritedOwner;
                                auto source = cxxClasses_.find(sourceOwner);
                                if (source != cxxClasses_.end())
                                {
                                    auto inheritedMethod = std::find_if(
                                        source->second.directMethods.begin(),
                                        source->second.directMethods.end(),
                                        [&](const CxxClassInfo::Method& method) {
                                            return method.raw.linkageName == sym.UniqueName;
                                        });
                                    if (inheritedMethod != source->second.directMethods.end())
                                    {
                                        const std::string inheritedKey =
                                            rawOverloadKey(inheritedMethod->raw);
                                        const bool redeclared = std::any_of(
                                            self->second.directMethods.begin(),
                                            self->second.directMethods.end(),
                                            [&](const CxxClassInfo::Method& method) {
                                                return !inheritedMethod->raw.isTemplateSpecialization
                                                    && !method.raw.isTemplateSpecialization
                                                    && !method.raw.isUsingShadow
                                                    && rawOverloadKey(method.raw) == inheritedKey;
                                            });
                                        if (redeclared) continue;
                                    }
                                }
                            }
                        }
                    }
                    if (!conflictingInheritedOperator && !present.insert(key).second) continue;
                    const uint64_t inherited = [&] {
                        auto a = cxxThisAdjust_.find(CxxThisAdjustKey(baseName, sym.UniqueName));
                        return a == cxxThisAdjust_.end() ? 0ull : a->second;
                    }();
                    /*
                     * A member whose owner sits inside a VIRTUAL base has no fixed offset along
                     * the inheritance path: the base is shared and placed by the most derived
                     * class. Re-resolve it from THIS class's own virtual-base table.
                     */
                    std::string ownerVirtualBase;
                    uint64_t insideVirtualBase = 0;
                    if (auto v = cxxThisVirtualBase_.find(CxxThisAdjustKey(baseName, sym.UniqueName));
                        v != cxxThisVirtualBase_.end())
                    {
                        ownerVirtualBase = v->second.first;
                        insideVirtualBase = v->second.second;
                    }
                    else if (b.isVirtual)
                    {
                        ownerVirtualBase = baseName;
                        insideVirtualBase = inherited;
                    }
                    uint64_t adjust = b.offsetBytes + inherited;
                    if (!ownerVirtualBase.empty())
                        for (const auto& vb : r.virtualBases)
                        {
                            const std::string vbIdentity = ResolveCxxBaseIdentity(vb);
                            if ((vbIdentity.empty() ? vb.name : vbIdentity) != ownerVirtualBase)
                                continue;
                            adjust = vb.offsetBytes + insideVirtualBase;
                            cxxThisVirtualBase_[CxxThisAdjustKey(r.name, sym.UniqueName)] =
                                { ownerVirtualBase, insideVirtualBase };
                            break;
                        }
                    sym.Parameters[0].TypeName = r.name;
                    if (sym.CxxInheritedOwner.empty()) sym.CxxInheritedOwner = baseName;
                    if (adjust != 0)
                        cxxThisAdjust_[CxxThisAdjustKey(r.name, sym.UniqueName)] = adjust;
                    functionTable[mn].push_back(std::move(sym));
                    // A named using-declaration admits only public overloads: access is public.
                    int access = cflat_cinterop::AccessPublic;
                    if (usingShadows.count(mn) == 0)
                        if (auto accessIt = bit->second.memberAccess.find(mn);
                            accessIt != bit->second.memberAccess.end())
                            access = accessIt->second;
                    noteMethodName(mn, access);
                    added = true;
                }
            }
            /*
             * Const twins (CxxConstTwinName) of an inherited, unhidden const/non-const pair are
             * cloned onto this class too, so a const derived receiver calls the base's const
             * member like clang. A name this class declares hides the base pair entirely.
             */
            for (const std::string& mn : baseMethodNames)
            {
                const bool usingNamed = usingShadows.count(mn) != 0;
                if (declaredNames.count(mn) != 0 && !usingNamed) continue;
                auto tit = functionTable.find(CxxConstTwinName(mn));
                if (tit == functionTable.end()) continue;
                std::vector<FunctionSymbol> fromBase;
                for (const FunctionSymbol& sym : tit->second)
                    if (sym.IsCxx && sym.IsMethod && !sym.Parameters.empty()
                        && sym.Parameters[0].TypeName == baseName
                        && (!usingNamed || usingAdmits(mn, sym.UniqueName))
                        && std::none_of(tit->second.begin(), tit->second.end(),
                               [&](const FunctionSymbol& other) {
                                   return !other.Parameters.empty()
                                       && other.Parameters[0].TypeName == r.name
                                       && other.UniqueName == sym.UniqueName;
                               }))
                        fromBase.push_back(sym);
                for (FunctionSymbol sym : fromBase)
                {
                    // Virtual-base placement is re-resolved per class; keep the plain call there.
                    if (b.isVirtual
                        || cxxThisVirtualBase_.count(CxxThisAdjustKey(baseName, sym.UniqueName)) != 0)
                        continue;
                    const uint64_t inherited = [&] {
                        auto a = cxxThisAdjust_.find(CxxThisAdjustKey(baseName, sym.UniqueName));
                        return a == cxxThisAdjust_.end() ? 0ull : a->second;
                    }();
                    const uint64_t adjust = b.offsetBytes + inherited;
                    sym.Parameters[0].TypeName = r.name;
                    if (sym.CxxInheritedOwner.empty()) sym.CxxInheritedOwner = baseName;
                    if (adjust != 0)
                        cxxThisAdjust_[CxxThisAdjustKey(r.name, sym.UniqueName)] = adjust;
                    functionTable[CxxConstTwinName(mn)].push_back(std::move(sym));
                    added = true;
                }
            }
        }
        return added;
    }

void LLVMBackend::RegisterCMacros(const std::vector<CMacroEntry>& macros)
{
        size_t registered = 0;
        for (const CMacroEntry& m : macros)
        {
            if (m.name.empty()) continue;
            if (!m.aliasTarget.empty()) continue;   // no folded value; RegisterCMacroAliases binds it
            if (globalNamedVariable.count(m.name)) continue;

            TypeAndValue tv;
            tv.VariableName = m.name;
            llvm::Constant* c = nullptr;
            std::string valSuffix;
            if (m.isString)
            {
                // Intern the string literal and register a char* global (char* matches const char* ABI).
                tv.TypeName = "char";
                tv.Pointer  = true;
                llvm::Value* strGv = CreateGlobalString(".cmacro." + m.name, m.stringValue);
                c = llvm::cast<llvm::Constant>(strGv);
                valSuffix = std::format(" = \"{}\"", m.stringValue);
            }
            else if (m.isFloat)
            {
                // Float/double macro (e.g. M_PI). Always registered as `double` - CFlat narrows at
                // use site; double matches C's default FP promotion in variadic/unprototyped calls.
                tv.TypeName = "double";
                tv.Pointer  = false;
                c = llvm::ConstantFP::get(builder->getDoubleTy(), m.floatValue);
                valSuffix = std::format(" = {}", m.floatValue);
            }
            else if (m.isFuncPtr)
            {
                // A C function-pointer macro (e.g. ((int(*)(int,int))0)) is the THIN
                // function<R(P...)>: a bare C function pointer frozen at link time. The
                // constant is just the bit pattern reinterpreted as the thin signature.
                // Force the thin marker: funcPtrTV can arrive with an empty TypeName (e.g.
                // from a cached extraction), which would make GetType pick the fat closure
                // type {ptr,ptr} for the global while the initializer below is a thin ptr -
                // a definition the verifier rejects. A C macro fn-ptr is always thin.
                tv = m.funcPtrTV;
                tv.TypeName = "__c_fn_ptr";
                tv.VariableName = m.name;
                c = llvm::ConstantExpr::getIntToPtr(
                    builder->getInt64((uint64_t)m.value), BuildThinFnPtrType(tv));
            }
            else if (m.isPointer)
            {
                // Sentinel pointer: reinterpret the bit pattern as a void* so comparisons
                // against HANDLE-returning APIs work without an explicit cast; a scalar cast
                // pointer keeps its pointee (int* for ((int*)0)).
                tv.TypeName = m.pointeeTypeName.empty() ? "void" : m.pointeeTypeName;
                tv.Pointer  = true;
                llvm::Type* i8Ptr = llvm::PointerType::get(*context, 0);
                llvm::Constant* bits = builder->getInt64((uint64_t)m.value);
                c = llvm::ConstantExpr::getIntToPtr(bits, i8Ptr);
                valSuffix = std::format(" = 0x{:x}", (uint64_t)m.value);
            }
            else if (!m.intTypeName.empty() && m.intTypeName != "__cflat_type_erased")
            {
                const std::string enumName = ResolveEnumTypeName(m.intTypeName);
                const std::string enumBacking = enumName.empty()
                    ? std::string() : GetEnumBackingType(enumName);
                tv.TypeName = enumBacking.empty() ? m.intTypeName : enumName;
                tv.Pointer = false;
                if (!enumBacking.empty())
                {
                    tv.EnumBacking = enumBacking;
                    tv.IsScopedEnum = IsScopedEnumTypeName(enumName);
                    auto* enumTy = llvm::dyn_cast_or_null<llvm::IntegerType>(GetType(tv));
                    if (enumTy != nullptr)
                        c = llvm::ConstantInt::get(enumTy, llvm::APInt(
                            enumTy->getBitWidth(), (uint64_t)m.value, m.value < 0, true));
                }
                else
                {
                    unsigned bits = BitfieldStorageBits(m.intTypeName);
                    if (bits != 0)
                        c = cflat_llvm::GetIntTruncated(llvm::IntegerType::get(*context, bits),
                                                        (uint64_t)m.value);
                }
                valSuffix = ConstIntValueSuffix(tv.TypeName, m.value);
            }
            else
            {
                // Natural type unknown: width-guess from the folded value.
                bool wide = (m.value < INT32_MIN || m.value > INT32_MAX);
                tv.TypeName = wide ? "i64" : "int";
                tv.Pointer  = false;
                c = wide
                    ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)m.value))
                    : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)m.value));
                valSuffix = ConstIntValueSuffix(tv.TypeName, m.value);
            }
            CreateGlobalVariable(tv, c);
            cxxMacroGlobalNames_.insert(m.name);
            if (m.isUnnamedEnum) cxxUnnamedEnumMacros_.insert(m.name);
            if (activeCxxRequestGroup_ != nullptr)
            {
                cxxConstantMacroNames_.insert(m.name);
                cxxConstantMacroOwnerGroup_[m.name] = activeCxxRequestGroup_->primary;
            }
            // The set also tracks imported C pointer-valued NULL macros whose C++ expansion
            // is not preserved by the C-side value import.
            if (m.isIntegerLiteralZero && c != nullptr && c->getType()->isIntegerTy()
                && c->isNullValue())
                cxxZeroIntegerMacroNames_.insert(m.name);
            else if (m.isIntegerLiteralZero && m.isPointer && c != nullptr && c->isNullValue())
                cxxZeroIntegerMacroNames_.insert(m.name);
            if (m.isIntegerLiteralZero && m.isIntegerLiteralToken && m.name != "NULL" && c != nullptr
                && c->getType()->isIntegerTy() && c->isNullValue())
                cxxLiteralZeroIntegerMacroNames_.insert(m.name);
            ++registered;

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                            tv.TypeName + (tv.Pointer ? "* " : " ") + m.name + valSuffix);
        }
        if (verbose && !macros.empty())
            std::cout << std::format("[verbose]   registered {} C macro constant(s) (of {} object-like candidates)\n",
                registered, macros.size());
    }

// Bind object-like macros whose body is a single identifier (`#define A B`). Runs after every
// C entity of the import is registered, so B is resolvable whatever kind of thing it names.
void LLVMBackend::RegisterCMacroAliases(const std::vector<CMacroEntry>& macros,
                                        const std::vector<CFunctionMacroEntry>& funcMacros,
                                        const std::string& fileForLsp)
{
        std::vector<CMacroEntry> candidates = pendingCInteropAliases_;
        candidates.insert(candidates.end(), macros.begin(), macros.end());

        std::unordered_map<std::string, std::string> aliasMap;
        for (const CMacroEntry& m : candidates)
            if (!m.name.empty() && !m.aliasTarget.empty()) aliasMap.emplace(m.name, m.aliasTarget);
        if (aliasMap.empty())
        {
            pendingCInteropAliases_.clear();
            return;
        }

        std::unordered_map<std::string, const CFunctionMacroEntry*> funcMacroByName;
        for (const CFunctionMacroEntry& f : funcMacros) funcMacroByName.emplace(f.name, &f);

        // An alias may name another alias (`#define A B` / `#define B fn`). Walk the chain to
        // the first name that resolves to something real; the hop cap breaks a self-reference.
        auto isCFunctionTarget = [](const std::vector<FunctionSymbol>& syms) {
            for (const FunctionSymbol& sym : syms)
                if (sym.IsCInteropDeclaration || sym.IsCInteropAlias)
                    return true;
            return false;
        };

        auto resolvable = [&](const std::string& n) {
            auto fit = functionTable.find(n);
            return (fit != functionTable.end() && isCFunctionTarget(fit->second))
                || globalNamedVariable.count(n) != 0
                || funcMacroByName.count(n) != 0 || dataStructures.count(n) != 0
                || ResolveTypeAlias(n) != n;
        };

        size_t bound = 0;
        std::vector<CFunctionMacroEntry> macroTemplateAliases;
        std::vector<CMacroEntry> unresolved;
        for (const CMacroEntry& m : candidates)
        {
            if (m.name.empty() || m.aliasTarget.empty()) continue;
            // A real declaration of the same name always wins over the alias spelling.
            if (functionTable.count(m.name) || globalNamedVariable.count(m.name)) continue;

            std::string target = m.aliasTarget;
            for (int hop = 0; hop < 16 && !resolvable(target); ++hop)
            {
                auto next = aliasMap.find(target);
                if (next == aliasMap.end() || next->second == target) break;
                target = next->second;
            }
            if (target == m.name) continue;   // self-reference

            if (auto fn = functionTable.find(target);
                fn != functionTable.end() && isCFunctionTarget(fn->second))
            {
                // Reuse the target's llvm::Function and its whole signature: the alias is a
                // second lookup name for one linkage symbol, never a new external symbol.
                for (const FunctionSymbol& sym : fn->second)
                {
                    if (!sym.IsCInteropDeclaration && !sym.IsCInteropAlias) continue;
                    FunctionSymbol aliasSym = sym;
                    aliasSym.IsCInteropAlias = true;
                    functionTable[m.name].push_back(std::move(aliasSym));
                }
                if (auto* sink = GetSymbolSink())
                    sink->Register(SymbolKind::Function, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                                   "#define " + m.name + " " + target);
                ++bound;
            }
            else if (auto gv = globalNamedVariable.find(target); gv != globalNamedVariable.end())
            {
                globalNamedVariable[m.name] = gv->second;
                auto gt = globalVariableTypes.find(target);
                if (gt != globalVariableTypes.end())
                {
                    TypeAndValue tv = gt->second;
                    tv.VariableName = m.name;
                    globalVariableTypes[m.name] = tv;
                }
                if (auto* sink = GetSymbolSink())
                    sink->Register(SymbolKind::Variable, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                                   "#define " + m.name + " " + target);
                ++bound;
            }
            else if (auto fm = funcMacroByName.find(target); fm != funcMacroByName.end())
            {
                // A function-like macro is a generated CFlat template, so the alias is a second
                // template with the same parameters and body.
                CFunctionMacroEntry copy = *fm->second;
                copy.name = m.name;
                copy.file = m.file; copy.line = m.line; copy.col = m.col < 0 ? 0 : m.col;
                macroTemplateAliases.push_back(std::move(copy));
                ++bound;
            }
            else if (dataStructures.count(target) || ResolveTypeAlias(target) != target)
            {
                if (ResolveTypeAlias(m.name) == m.name && !dataStructures.count(m.name))
                {
                    RegisterTypeAlias(m.name, target);
                    RegisterTypeAliasSymbol(m.name, target, m.file, m.line, m.col < 0 ? 0 : m.col);
                    ++bound;
                }
            }
            else
            {
                // Keep unresolved aliases for a later import. An unknown identifier still stays
                // unimported, silently: a header is full of names CFlat has no business binding.
                unresolved.push_back(m);
            }
        }

        pendingCInteropAliases_.swap(unresolved);

        if (!macroTemplateAliases.empty())
            RegisterCFunctionMacros(macroTemplateAliases, fileForLsp + "@aliases");

        if (verbose && bound)
            std::cout << std::format("[verbose]   bound {} alias macro(s) (of {} candidate(s)) from {}\n",
                bound, aliasMap.size(), fileForLsp);
    }

// True when `name` lexes as one CFlat Identifier token (not a keyword such as `string`).

// Two identifiers side by side (`X DIR`, a token-pasting body) are no expression.

bool LLVMBackend::TranslateMacroBody(const CFunctionMacroEntry& m, std::string& out)
{
        std::unordered_set<std::string> paramSet(m.params.begin(), m.params.end());
        out.clear();
        out.reserve(m.body.size());
        const std::string& s = m.body;
        size_t i = 0;
        bool hasContent = false;
        std::unordered_set<std::string> translatedCastTypes;

        auto macroCastType = [&](size_t open, size_t& close, std::string& flatType) {
            int depth = 1;
            close = open + 1;
            for (; close < s.size() && depth != 0; ++close)
            {
                if (s[close] == '(') ++depth;
                else if (s[close] == ')') --depth;
            }
            if (depth != 0) return false;
            --close;
            std::string spelling = s.substr(open + 1, close - open - 1);
            bool hasName = false;
            for (char ch : spelling)
            {
                if (std::isalpha((unsigned char)ch) || std::isdigit((unsigned char)ch)
                    || ch == '_' || ch == '*' || std::isspace((unsigned char)ch))
                    hasName |= std::isalpha((unsigned char)ch) || ch == '_';
                else
                    return false;
            }
            if (!hasName) return false;
            // A parenthesized macro parameter is grouping, never a cast, whatever it is named.
            std::string bare = spelling;
            std::erase_if(bare, [](char ch) { return std::isspace((unsigned char)ch); });
            if (paramSet.count(bare) != 0) return false;
            TypeAndValue mapped;
            if (!MapCTypeToTypeAndValue(spelling, mapped, /*cxxBoundary*/ false)
                && !MapCTypeToTypeAndValue(spelling, mapped, /*cxxBoundary*/ true))
                return false;
            flatType = mapped.TypeName;
            if (mapped.Pointer) flatType += "*";
            if (mapped.ElemPointer) flatType += "*";
            translatedCastTypes.insert(flatType);
            return !flatType.empty();
        };

        // 'c' = argument list of a validated call (',' allowed);
        // 'g' = grouping paren (',' would be the comma operator, rejected).
        std::vector<char> parenCtx;
        bool pendingCallParen = false;

        while (i < s.size())
        {
            char c = s[i];
            if (std::isspace((unsigned char)c)) { out += c; ++i; continue; }

            // Block comment: /* ... */ allowed (and stripped); line comments rejected.
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') return false;
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                if (i + 1 < s.size()) i += 2;
                continue;
            }

            // Identifier: a parameter, a call into a known function, or a bare reference to
            // a known global / enum constant. Anything else (unknown name) drops the macro.
            if (std::isalpha((unsigned char)c) || c == '_')
            {
                size_t start = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
                std::string ident = s.substr(start, i - start);
                if (paramSet.count(ident)) { out += ident; hasContent = true; continue; }

                // Peek past whitespace: an identifier followed by '(' is a call.
                size_t j = i;
                while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
                bool isCall = (j < s.size() && s[j] == '(');
                if (isCall)
                {
                    if (!functionTable.count(ident)) return false;   // unknown callee: drop
                    pendingCallParen = true;
                }
                else if (!globalNamedVariable.count(ident))
                {
                    return false;   // unknown bare identifier (not a constant): drop
                }
                out += ident;
                hasContent = true;
                continue;
            }

            // Integer suffixes (u/U/l/L) are stripped; float suffixes kept; hex floats dropped.
            if (std::isdigit((unsigned char)c) ||
                (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])))
            {
                size_t start = i;
                bool isHex = (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X'));
                if (isHex)
                {
                    i += 2;
                    while (i < s.size() && std::isxdigit((unsigned char)s[i])) ++i;
                    if (i < s.size() && (s[i] == '.' || s[i] == 'p' || s[i] == 'P'))
                        return false;   // hex float: drop
                    out += s.substr(start, i - start);
                    while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                    hasContent = true;
                    continue;
                }

                bool isFloat = false;
                while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                if (i < s.size() && s[i] == '.')
                {
                    isFloat = true;
                    ++i;
                    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
                {
                    size_t save = i;
                    ++i;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                    if (i < s.size() && std::isdigit((unsigned char)s[i]))
                    {
                        isFloat = true;
                        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                    }
                    else { i = save; }   // a stray 'e' that is not an exponent
                }
                out += s.substr(start, i - start);
                if (isFloat)
                    while (i < s.size() && (s[i] == 'f' || s[i] == 'F' || s[i] == 'l' || s[i] == 'L'))
                        { out += s[i]; ++i; }
                else
                    while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                hasContent = true;
                continue;
            }

            // Char literal: 'x' / '\n' etc. - cflat accepts the same C escape forms. The
            // closing quote must be found (respecting backslash escapes) or the macro drops.
            if (c == '\'')
            {
                size_t start = i;
                ++i;
                while (i < s.size() && s[i] != '\'')
                {
                    if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                    else ++i;
                }
                if (i >= s.size()) return false;   // unterminated
                ++i;                                // closing quote
                out += s.substr(start, i - start);
                hasContent = true;
                continue;
            }

            // String literals and disallowed punctuation.
            if (c == '"')                                        return false;
            if (c == '#' || c == '[' || c == ']' || c == '.' || c == ';') return false;
            if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') return false; // ->

            // Parens: track call vs grouping context so ',' is only allowed in a call's
            // argument list.
            if (c == '(')
            {
                size_t castClose = 0;
                std::string castType;
                auto castHasOperand = [&](size_t close) {
                    size_t n = close + 1;
                    while (n < s.size() && std::isspace((unsigned char)s[n])) ++n;
                    return n < s.size() && (std::isalnum((unsigned char)s[n]) || s[n] == '_'
                        || s[n] == '(' || s[n] == '-' || s[n] == '~' || s[n] == '!' || s[n] == '\'');
                };
                if (macroCastType(i, castClose, castType) && castHasOperand(castClose))
                {
                    out += "(" + castType + ")";
                    i = castClose + 1;
                    hasContent = true;
                    continue;
                }
                parenCtx.push_back(pendingCallParen ? 'c' : 'g');
                pendingCallParen = false;
                out += c; ++i; hasContent = true; continue;
            }
            if (c == ')')
            {
                if (!parenCtx.empty()) parenCtx.pop_back();
                out += c; ++i; hasContent = true; continue;
            }
            if (c == ',')
            {
                if (parenCtx.empty() || parenCtx.back() != 'c') return false; // comma operator: drop
                out += c; ++i; hasContent = true; continue;
            }

            // Two-char operators kept intact.
            if (i + 1 < s.size())
            {
                std::string two = s.substr(i, 2);
                static const std::unordered_set<std::string> twoChar = {
                    "<=", ">=", "==", "!=", "&&", "||", "<<", ">>"
                };
                if (twoChar.count(two)) { out += two; i += 2; hasContent = true; continue; }
            }

            // Single-character operators.
            static const std::string allowedSingle = "+-*/%&|^~!<>?:";
            if (allowedSingle.find(c) != std::string::npos)
            {
                out += c;
                ++i;
                hasContent = true;
                continue;
            }

            return false; // anything else: reject
        }

        if (!hasContent) return false;
        while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
        if (out.empty()) return false;

        // Strip parens around bare identifiers: '(n)' is a C-style cast in CFlat grammar,
        // but never strip call-argument parens (preceded by identifier char).
        size_t pos = 0;
        while ((pos = out.find('(', pos)) != std::string::npos)
        {
            size_t prevPos = pos;
            while (prevPos > 0 && std::isspace((unsigned char)out[prevPos - 1])) --prevPos;
            if (prevPos > 0)
            {
                char prev = out[prevPos - 1];
                if (std::isalnum((unsigned char)prev) || prev == '_') { ++pos; continue; }
            }
            size_t inner = pos + 1;
            while (inner < out.size() && std::isspace((unsigned char)out[inner])) ++inner;
            if (inner >= out.size() ||
                !(std::isalpha((unsigned char)out[inner]) || out[inner] == '_'))
            {
                ++pos; continue;
            }
            size_t identEnd = inner;
            while (identEnd < out.size() &&
                   (std::isalnum((unsigned char)out[identEnd]) || out[identEnd] == '_'))
                ++identEnd;
            size_t after = identEnd;
            while (after < out.size() && std::isspace((unsigned char)out[after])) ++after;
            if (after >= out.size() || out[after] != ')') { ++pos; continue; }
            const std::string innerName = out.substr(inner, identEnd - inner);
            if (!paramSet.count(innerName) && translatedCastTypes.count(innerName))
            {
                ++pos;
                continue;
            }
            out.replace(pos, after + 1 - pos, out.substr(inner, identEnd - inner));
            // Stay at pos: the rewrite may expose another stripping opportunity.
        }
        return true;
    }

void LLVMBackend::RegisterCFunctionMacros(const std::vector<CFunctionMacroEntry>& funcMacros,
                                 const std::string& fileForLsp,
                                 std::vector<CFunctionMacroEntry>* retryMacros)
{
        if (funcMacros.empty()) return;

        std::string generated;
        generated.reserve(funcMacros.size() * 60);
        size_t accepted = 0, rejected = 0, skipped = 0;

        for (const auto& m : funcMacros)
        {
            if (functionTable.count(m.name) ||
                globalNamedVariable.count(m.name))
            {
                ++skipped;
                if (verbose) std::cout << std::format("[verbose]   skip macro {}: name already defined\n", m.name);
                continue;
            }

            if (activeCxxRequestGroup_ != nullptr
                && activeCxxRequestGroup_->primary < cxxImportGroups_.size())
            {
                importedCxxFunctionMacros_.insert(m.name);
                cxxFunctionMacros_.insert(m.name);
                cxxFunctionOwnerGroup_.try_emplace(m.name, activeCxxRequestGroup_->primary);
            }

            // Only generic templates are bound, and `auto F<>()` is no CFlat syntax.
            if (m.params.empty())
            {
                ++rejected;
                if (verbose) std::cout << std::format("[verbose]   reject macro {}: no parameters\n", m.name);
                continue;
            }
            // A parameter spelled like a CFlat keyword (`string`) cannot name a parameter.
            if (!std::all_of(m.params.begin(), m.params.end(), IsPlainCFlatIdentifier))
            {
                ++rejected;
                if (verbose) std::cout << std::format("[verbose]   reject macro {}: parameter name is a CFlat keyword\n", m.name);
                continue;
            }

            std::string translatedBody;
            if (!TranslateMacroBody(m, translatedBody) || HasAdjacentIdentifiers(translatedBody))
            {
                ++rejected;
                if (retryMacros != nullptr)
                    retryMacros->push_back(m);
                if (verbose) std::cout << std::format("[verbose]   reject macro {}: body uses unsupported tokens\n", m.name);
                continue;
            }
            generated += "auto " + m.name + "<";
            for (size_t i = 0; i < m.params.size(); ++i)
            {
                if (i) generated += ", ";
                generated += "T" + std::to_string(i);
            }
            generated += ">(";
            for (size_t i = 0; i < m.params.size(); ++i)
            {
                if (i) generated += ", ";
                generated += "T" + std::to_string(i) + " " + m.params[i];
            }
            generated += ") { return (" + translatedBody + "); }\n";
            ++accepted;
        }

        if (verbose)
            std::cout << std::format("[verbose]   function-like C macros: {} translated, {} rejected, {} skipped (already defined) from {}\n",
                accepted, rejected, skipped, fileForLsp);

        if (!generated.empty())
            pendingMacroSources_.push_back({ fileForLsp + "@cmacros", std::move(generated) });
    }

/*
 * Text quoted from the front end is prefixed with its source, so the clause reads as relayed
 * from another compiler rather than as a cflat diagnostic. Deliberately NOT an executable name:
 * a header bind drives clang's front end in-process (CompilerInstance::ExecuteAction), so no
 * clang binary is spawned here - unlike the .c and link paths, which really do run clang-cl and
 * lld-link and name those. Clang carries no message catalog (its diagnostics are English format
 * strings baked into clang/include/clang/Basic/Diagnostic*Kinds.td), so this clause stays
 * English in every locale - naming the source says why.
 */
static constexpr const char* kClangDiagPrefix = "clang";

void LLVMBackend::ReportOrphanHeader(const std::vector<std::string>& headerPaths,
                                     const std::string& clangErr, bool cxxMode)
{
        std::string name = std::filesystem::path(headerPaths.front()).filename().string();
        const char* lang = cxxMode ? "C++" : "C";
        std::string detail = clangErr.empty()
            ? std::string("a required type is undefined")
            : std::format("{}: {}", kClangDiagPrefix, clangErr);
        if (headerPaths.size() > 1)
        {
            std::string grp;
            for (size_t i = 0; i < headerPaths.size(); ++i)
                grp += (i ? ", \"" : "\"") + std::filesystem::path(headerPaths[i]).filename().string() + "\"";
            LogRawError(std::format(
                "{} header '{}' did not compile in this group ({}). Reorder the group so the "
                "prerequisite header comes first, or add the missing one: import {{ {} }};",
                lang, name, detail, grp));
            return;
        }
        LogRawError(std::format(
            "{} header '{}' does not compile on its own ({}). It likely needs a prerequisite "
            "header included first. Import them together as one group so they share a single "
            "translation unit, e.g. import {{ \"prerequisite.h\", \"{}\" }};",
            lang, name, detail, name));
    }

void LLVMBackend::ReportUncompilableHeader(const std::vector<std::string>& headerPaths,
                                           const std::string& clangErr, bool cxxMode)
{
        std::string name = std::filesystem::path(headerPaths.front()).filename().string();
        std::string detail = clangErr.empty()
            ? std::format("{} reported an error in it", kClangDiagPrefix)
            : std::format("{}: {}", kClangDiagPrefix, clangErr);
        LogRawError(std::format(
            "{} header '{}' does not compile ({}). Nothing in it can be bound until that "
            "error is fixed.", cxxMode ? "C++" : "C", name, detail));
    }

bool LLVMBackend::CompileCHeader(const std::string& headerPath, const std::vector<std::string>& extraDefines,
                        bool cppMode)
{
        return CompileCHeaderGroup(std::vector<std::string>{ headerPath }, extraDefines, cppMode);
    }

bool LLVMBackend::CompileCHeaderGroup(const std::vector<std::string>& headerPaths,
                             const std::vector<std::string>& extraDefines,
                             bool cppMode)
{
        if (headerPaths.empty()) return true;

        std::vector<std::string> realPaths;
        realPaths.reserve(headerPaths.size());
        for (const auto& h : headerPaths)
        {
            llvm::SmallString<256> rp;
            realPaths.push_back(!llvm::sys::fs::real_path(h, rp) ? rp.str().str() : h);
            RecordDependency(realPaths.back());
        }
        const std::string& fileForLsp = realPaths.front();

        size_t cHeaderMacroGroupIndex = static_cast<size_t>(-1);
        if (!cppMode)
        {
            auto existing = std::find_if(cHeaderMacroGroups_.begin(), cHeaderMacroGroups_.end(),
                [&](const CHeaderMacroGroup& group) {
                    return group.headers == realPaths && group.defines == extraDefines;
                });
            if (existing == cHeaderMacroGroups_.end())
            {
                cHeaderMacroGroups_.push_back({ realPaths, extraDefines, realPaths });
                cHeaderMacroGroupIndex = cHeaderMacroGroups_.size() - 1;
            }
            else
            {
                cHeaderMacroGroupIndex = static_cast<size_t>(
                    std::distance(cHeaderMacroGroups_.begin(), existing));
                existing->dependencyFiles.insert(existing->dependencyFiles.end(),
                                                 realPaths.begin(), realPaths.end());
            }
        }
        auto rememberMacroDependencies = [&](const std::vector<std::string>& paths) {
            if (cHeaderMacroGroupIndex >= cHeaderMacroGroups_.size()) return;
            auto& known = cHeaderMacroGroups_[cHeaderMacroGroupIndex].dependencyFiles;
            for (const std::string& path : paths)
                if (std::find(known.begin(), known.end(), path) == known.end())
                    known.push_back(path);
        };

        // The C++ link decision must not depend on whether the parse ran: a warm in-memory or
        // disk cache hit skips ExtractCHeaderClang entirely, and the flag would stay false.
        if (cppMode) cppInteropUsed_ = true;

        /*
         * Same reason the flag above is set here: a concrete C++ type request must see this group's
         * headers and defines even when no parse ran for it in this analysis. The group is THIS
         * import statement only - every request this import triggers compiles against it, and no
         * other import can change that translation unit or its cache identity.
         */
        size_t cxxGroupIndex = static_cast<size_t>(-1);
        CxxRequestGroup cxxGroup;
        if (cppMode)
        {
            cxxGroupIndex = FindOrAddCxxImportGroup(realPaths, extraDefines);
            // A standard-library header is a template catalog the walk deliberately skips, so no
            // record of it is ever registered - seed its namespace here or `std.vector<int>` could
            // never be requested at all.
            for (const auto& h : realPaths)
            {
                if (IsSystemCxxHeaderPath(h))
                {
                    cxxForeignNamespaces_.insert("std");
                    cxxImportGroups_[cxxGroupIndex].namespaces.insert("std");
                    // The nested names ("std::chrono") are harvested separately: they must not
                    // become request leads, they only mark a dotted path as a namespace path.
                    CollectHeaderNamespaceNames(h, cxxNestedNamespaceNames_);
                }
                else
                {
                    CollectHeaderNamespaceNames(h, cxxForeignNamespaces_,
                                                &cxxImportGroups_[cxxGroupIndex].namespaces);
                }
            }
            cxxGroup = MakeCxxRequestGroup(cxxGroupIndex, {});
        }
        CxxRequestGroupScope cxxGroupGuard(*this, cppMode ? &cxxGroup : nullptr);
        // A cache hit skips extraction, which is what seeds the group's namespaces; replay the
        // stored ones so group candidate order - and so request attribution - matches cold.
        auto replayCxxGroupNamespaces = [&](const std::vector<std::string>& names) {
            if (!cppMode || cxxGroupIndex >= cxxImportGroups_.size()) return;
            for (const auto& name : names)
            {
                cxxForeignNamespaces_.insert(name);
                cxxImportGroups_[cxxGroupIndex].namespaces.insert(name);
            }
        };

        // Best-effort alias retry: a macro that still cannot be resolved is dropped, exactly as
        // the first registration pass drops it. A real header carries many such macros.
        bool aliasRetryFailed = false;
        auto macroUsesObjectAlias = [](const auto& macro, const auto& aliases) {
            std::unordered_set<std::string> params(macro.params.begin(), macro.params.end());
            std::unordered_set<std::string> aliasNames;
            for (const auto& alias : aliases)
                if (!alias.name.empty() && !alias.aliasTarget.empty()) aliasNames.insert(alias.name);
            size_t i = 0;
            while (i < macro.body.size())
            {
                if (!std::isalpha((unsigned char)macro.body[i]) && macro.body[i] != '_')
                {
                    ++i;
                    continue;
                }
                size_t start = i++;
                while (i < macro.body.size()
                       && (std::isalnum((unsigned char)macro.body[i]) || macro.body[i] == '_')) ++i;
                std::string ident = macro.body.substr(start, i - start);
                if (!params.count(ident) && aliasNames.count(ident)) return true;
            }
            return false;
        };
        auto rewriteObjectAliases = [&](CFunctionMacroEntry macro, const auto& aliases) {
            std::unordered_map<std::string, std::string> aliasMap;
            for (const auto& alias : aliases)
                if (!alias.name.empty() && !alias.aliasTarget.empty())
                    aliasMap.emplace(alias.name, alias.aliasTarget);
            std::unordered_set<std::string> params(macro.params.begin(), macro.params.end());
            auto resolve = [&](const std::string& name) {
                std::string current = name;
                std::unordered_set<std::string> seen;
                for (int depth = 0; depth < 16; ++depth)
                {
                    auto it = aliasMap.find(current);
                    if (it == aliasMap.end()) return current;
                    if (!seen.insert(current).second) { aliasRetryFailed = true; return current; }
                    current = it->second;
                }
                aliasRetryFailed = true;
                return current;
            };

            std::string rewritten;
            rewritten.reserve(macro.body.size());
            size_t i = 0;
            while (i < macro.body.size())
            {
                if (!std::isalpha((unsigned char)macro.body[i]) && macro.body[i] != '_')
                {
                    rewritten += macro.body[i++];
                    continue;
                }
                size_t start = i++;
                while (i < macro.body.size()
                       && (std::isalnum((unsigned char)macro.body[i]) || macro.body[i] == '_')) ++i;
                std::string ident = macro.body.substr(start, i - start);
                if (!params.count(ident) && aliasMap.count(ident)) rewritten += resolve(ident);
                else rewritten += ident;
            }
            macro.body = std::move(rewritten);
            return macro;
        };
        auto expandFunctionMacroAliases = [&](CFunctionMacroEntry macro,
                                               const auto& aliases, const auto& functionMacros) {
            std::unordered_map<std::string, std::string> aliasMap;
            for (const auto& alias : aliases)
                if (!alias.name.empty() && !alias.aliasTarget.empty())
                    aliasMap.emplace(alias.name, alias.aliasTarget);
            std::unordered_map<std::string, const CFunctionMacroEntry*> functionMacroMap;
            for (const auto& functionMacro : functionMacros)
                functionMacroMap.emplace(functionMacro.name, &functionMacro);
            std::unordered_set<std::string> params(macro.params.begin(), macro.params.end());

            auto resolve = [&](const std::string& name) {
                std::string current = name;
                std::unordered_set<std::string> seen;
                for (int depth = 0; depth < 16; ++depth)
                {
                    auto it = aliasMap.find(current);
                    if (it == aliasMap.end()) return current;
                    if (!seen.insert(current).second) { aliasRetryFailed = true; return current; }
                    current = it->second;
                }
                aliasRetryFailed = true;
                return current;
            };
            auto substitute = [](const CFunctionMacroEntry& functionMacro,
                                 const std::vector<std::string>& arguments) {
                std::unordered_map<std::string, std::string> replacements;
                for (size_t i = 0; i < functionMacro.params.size(); ++i)
                    replacements.emplace(functionMacro.params[i], arguments[i]);
                std::string result;
                size_t i = 0;
                while (i < functionMacro.body.size())
                {
                    if (!std::isalpha((unsigned char)functionMacro.body[i])
                        && functionMacro.body[i] != '_')
                    {
                        result += functionMacro.body[i++];
                        continue;
                    }
                    size_t start = i++;
                    while (i < functionMacro.body.size()
                           && (std::isalnum((unsigned char)functionMacro.body[i])
                               || functionMacro.body[i] == '_')) ++i;
                    std::string ident = functionMacro.body.substr(start, i - start);
                    auto replacement = replacements.find(ident);
                    result += replacement == replacements.end() ? ident : replacement->second;
                }
                return result;
            };

            for (int pass = 0; pass < 16; ++pass)
            {
                bool expanded = false;
                size_t i = 0;
                while (i < macro.body.size())
                {
                    if (!std::isalpha((unsigned char)macro.body[i]) && macro.body[i] != '_')
                    {
                        ++i;
                        continue;
                    }
                    size_t start = i++;
                    while (i < macro.body.size()
                           && (std::isalnum((unsigned char)macro.body[i]) || macro.body[i] == '_')) ++i;
                    std::string ident = macro.body.substr(start, i - start);
                    if (params.count(ident)) continue;
                    size_t open = i;
                    while (open < macro.body.size() && std::isspace((unsigned char)macro.body[open])) ++open;
                    auto functionMacro = functionMacroMap.find(resolve(ident));
                    if (aliasRetryFailed) return macro;
                    if (open >= macro.body.size() || macro.body[open] != '(' || functionMacro == functionMacroMap.end())
                        continue;

                    size_t close = open + 1;
                    int depth = 1;
                    size_t argumentStart = close;
                    std::vector<std::string> arguments;
                    while (close < macro.body.size() && depth > 0)
                    {
                        if (macro.body[close] == '(') ++depth;
                        else if (macro.body[close] == ')') --depth;
                        if (depth == 1 && macro.body[close] == ',')
                        {
                            arguments.push_back(macro.body.substr(argumentStart, close - argumentStart));
                            argumentStart = close + 1;
                        }
                        ++close;
                    }
                    if (depth != 0) { aliasRetryFailed = true; return macro; }
                    if (close - 1 > argumentStart || !functionMacro->second->params.empty())
                        arguments.push_back(macro.body.substr(argumentStart, close - 1 - argumentStart));
                    if (arguments.size() != functionMacro->second->params.size())
                        { aliasRetryFailed = true; return macro; }

                    std::string replacement = substitute(*functionMacro->second, arguments);
                    macro.body.replace(start, close - start, replacement);
                    expanded = true;
                    break;
                }
                if (!expanded) return macro;
            }
            aliasRetryFailed = true;
            return macro;
        };

        // Fold headers, include-dirs, and defines: same header under different roots/defines or
        // standalone vs. grouped must not share a stale cache entry.
        std::string cacheKey;
        cacheKey += "|T" + CInteropTargetTriple();
        for (const auto& rp : realPaths)       cacheKey += "|H" + rp;
        for (const auto& inc : cIncludeDirs_)  cacheKey += "|I" + inc;
        for (const auto& def : cDefines_)      cacheKey += "|D" + def;
        for (const auto& def : extraDefines)   cacheKey += "|d" + def;
        // C and C++ mode bind the same header differently (qualified names, linkage names),
        // so they must never share a cache entry.
        if (cppMode) cacheKey += "|CXX|STD" + cppStandard_;
        /*
         * LSP analysis binds a C++ group with assumeInlineDefinitions instead of emitDefinitions:
         * every inline member gets a linkage name and a callable surface, but no body is emitted
         * and the companion bitcode stays EMPTY. Sharing one key with a real compile would let the
         * compile adopt that empty companion and bind bodies nothing ever emits - a link-time
         * "undefined symbol" with no diagnostic. The mode is part of the identity of the result.
         */
        const bool cxxDefinitionsEmitted = cppMode && symbolSink_ == nullptr;
        if (cppMode) cacheKey += cxxDefinitionsEmitted ? "|EDEF" : "|EDECL";
        // The cached bindings were produced by THIS compiler's C type mapper, and an upgrade
        // can change it (e.g. the LP64 `long` width). Without the compiler's identity in the
        // key, a stale entry silently outlives the code that wrote it.
        cacheKey += "|C" + CompilerBuildStamp();

        const int64_t includeHashTime = (int64_t)std::filesystem::file_time_type::clock::now()
                                            .time_since_epoch().count();
        uint64_t currentHash = 0;
        bool haveHash = false;
        // Content hashes this call already has (the headers below, then the included files the
        // extraction's namespace scan hashed from clang's buffers).
        std::unordered_map<std::string, std::pair<uint64_t, int64_t>> knownHashes;
        auto hashNow = [&]() -> uint64_t {
            if (!haveHash)
            {
                currentHash = 14695981039346656037ULL;
                for (const auto& rp : realPaths)
                {
                    uint64_t h = 0;
                    const bool hashed = cppMode
                        ? HashCxxHeaderFileContents(rp, h, /*force*/ true)
                        : HashFileContents(rp, h);
                    if (hashed) knownHashes.emplace(rp,
                        std::pair<uint64_t, int64_t>{h, includeHashTime});
                    currentHash ^= h; currentHash *= 1099511628211ULL;
                }
                haveHash = true;
            }
            return currentHash;
        };

        std::error_code mtEc;
        std::filesystem::file_time_type currentMtime{};
        std::vector<int64_t> headerMtimes;
        headerMtimes.reserve(realPaths.size());
        for (const auto& rp : realPaths)
        {
            std::error_code ec;
            auto mt = std::filesystem::last_write_time(rp, ec);
            if (ec)
            {
                if (!mtEc) mtEc = ec;
                headerMtimes.push_back(0);
                continue;
            }
            headerMtimes.push_back((int64_t)mt.time_since_epoch().count());
            if (mt > currentMtime) currentMtime = mt;
        }
        const uint64_t refusalGroupKey = CHeaderDiskCacheKey(
            realPaths, cIncludeDirs_, cDefines_, extraDefines,
            targetWindows_, CInteropTargetTriple(), cppMode, cxxDefinitionsEmitted,
            cppStandard_, ProgramTargetCPUFeatures().first,
            ProgramTargetCPUFeatures().second, cDebugInfo_);
        // The demand companion keys on the entry a compile stores, which an LSP reads too.
        const uint64_t demandEntryKey = cppMode && !cxxDefinitionsEmitted
            ? CHeaderDiskCacheKey(realPaths, cIncludeDirs_, cDefines_, extraDefines,
                                  targetWindows_, CInteropTargetTriple(), cppMode,
                                  /*cxxDefinitionsEmitted*/ true, cppStandard_,
                                  ProgramTargetCPUFeatures().first,
                                  ProgramTargetCPUFeatures().second, cDebugInfo_)
            : refusalGroupKey;

        std::vector<CSigEntry> hitSigs;
        std::vector<std::string> hitDepPaths;
        std::vector<CHeaderDep> hitDeps;
        std::vector<CEnumEntry> hitEnums;
        std::vector<CRecordEntry> hitRecords;
        std::vector<CMacroEntry> hitMacros;
        std::vector<CFunctionMacroEntry> hitFuncMacros;
        std::vector<CGlobalEntry> hitGlobals;
        std::vector<cflat_cinterop::RawDynamicInitializer> hitDynamicInitializers;
        std::vector<std::pair<std::string, std::string>> hitAliases;
        std::vector<CTypeAliasEntry> hitTypeAliases;
        std::vector<std::pair<std::string, std::string>> hitUsingDirectives;
        std::vector<std::pair<std::string, std::string>> hitNamespaceAliases;
        std::vector<std::pair<std::string, std::string>> hitClassUsings;
        std::vector<cflat_cinterop::RawFunctionTemplate> hitFunctionTemplates;
        std::vector<cflat_cinterop::RawClassTemplate> hitClassTemplates;
        std::vector<std::string> hitClassTemplateNames;
        std::vector<std::string> hitVariableTemplateNames;
        std::vector<std::pair<std::string, std::string>> hitVariableTemplateDependencies;
        std::vector<std::string> hitClassTemplateSpecializations;
        std::vector<std::string> hitGroupNamespaces;
        std::vector<cflat_cinterop::RawFunctionPointerAbi> hitFunctionPointerAbis;
        std::string hitCxxBitcode;
        std::string hitDemandKey;
        uint64_t hitDemandHash = 0;
        cflat_cinterop::ExtractResult::DemandReplayChunk hitDemandReplayChunk;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto cacheIt = cFileSigCache_.find(cacheKey);
            if (!mtEc && cacheIt != cFileSigCache_.end() && CxxDemandEntryUsable(cppMode))
            {
                CFileSigCacheEntry& entry = cacheIt->second;
                hitDemandKey = entry.cxxDemandGroupKey;
                hitDemandHash = entry.hash;
                hitDemandReplayChunk = entry.cxxDemandReplayChunk;
                if (entry.mtime == currentMtime)
                {
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (mtime) for {}\n", fileForLsp);
                    SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble,
                                           entry.targetTriple);
                    TouchCFileSigEntry(cacheKey, entry);
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitDynamicInitializers = entry.cxxDynamicInitializerSymbols;
                    hitAliases = entry.recordAliases; hit = true;
                    hitTypeAliases = entry.typeAliases;
                    hitUsingDirectives = entry.usingDirectives;
                    hitNamespaceAliases = entry.namespaceAliases;
                    hitClassUsings = entry.classUsings;
                    hitFunctionTemplates = entry.functionTemplates;
                    hitClassTemplates = entry.classTemplates;
                    hitClassTemplateNames = entry.classTemplateNames;
                    hitVariableTemplateNames = entry.variableTemplateNames;
                    hitVariableTemplateDependencies = entry.variableTemplateDependencies;
                    hitClassTemplateSpecializations = entry.classTemplateSpecializations;
                    hitGroupNamespaces = entry.cxxGroupNamespaces;
                    hitFunctionPointerAbis = entry.functionPointerAbis;
                    hitCxxBitcode = entry.cxxBitcode;
                    for (const auto& dep : entry.deps) hitDepPaths.push_back(dep.path);
                    hitDeps = entry.deps;
                }
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (hash) for {}\n", fileForLsp);
                    SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble,
                                           entry.targetTriple);
                    entry.mtime = currentMtime;
                    TouchCFileSigEntry(cacheKey, entry);
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitDynamicInitializers = entry.cxxDynamicInitializerSymbols;
                    hitAliases = entry.recordAliases; hit = true;
                    hitTypeAliases = entry.typeAliases;
                    hitUsingDirectives = entry.usingDirectives;
                    hitNamespaceAliases = entry.namespaceAliases;
                    hitClassUsings = entry.classUsings;
                    hitFunctionTemplates = entry.functionTemplates;
                    hitClassTemplates = entry.classTemplates;
                    hitClassTemplateNames = entry.classTemplateNames;
                    hitVariableTemplateNames = entry.variableTemplateNames;
                    hitVariableTemplateDependencies = entry.variableTemplateDependencies;
                    hitClassTemplateSpecializations = entry.classTemplateSpecializations;
                    hitGroupNamespaces = entry.cxxGroupNamespaces;
                    hitFunctionPointerAbis = entry.functionPointerAbis;
                    hitCxxBitcode = entry.cxxBitcode;
                    for (const auto& dep : entry.deps) hitDepPaths.push_back(dep.path);
                    hitDeps = entry.deps;
                }
            }
        }
        if (hit)
        {
            rememberMacroDependencies(hitDepPaths);
            if (cppMode) NoteCxxDynamicInitializers(hitDynamicInitializers);
            for (const auto& classTemplate : hitClassTemplates)
                cxxClassTemplateParameters_[classTemplate.name] = classTemplate;
            if (!cppMode && std::ranges::any_of(hitSigs, [](const CSigEntry& sig) { return sig.isInline; }))
            {
                std::vector<std::string> hashFiles = hitDepPaths;
                hashFiles.insert(hashFiles.end(), realPaths.begin(), realPaths.end());
                std::sort(hashFiles.begin(), hashFiles.end());
                hashFiles.erase(std::unique(hashFiles.begin(), hashFiles.end()), hashFiles.end());
                auto sharedHashFiles = std::make_shared<const std::vector<std::string>>(std::move(hashFiles));
                for (CSigEntry& sig : hitSigs)
                    if (sig.isInline)
                    {
                        sig.inlineDefines = extraDefines;
                        sig.inlineHeaders = realPaths;
                        sig.inlineHashFiles = sharedHashFiles;
                    }
            }
            // A memory hit carries the storing analysis' mapped CFlat names; remap C++ signatures
            // on registration exactly like a disk hit does.
            for (CSigEntry& sig : hitSigs) sig.needsCxxRebind = sig.isCxx;
            NoteCxxDemandEntry(hitDemandKey, hitDemandHash, demandEntryKey,
                               cxxDemandLiveKeys_.count(cacheKey) == 0);
            if (!hitDemandKey.empty() && activeCxxRequestGroup_ != nullptr)
                NoteCxxDemandReplayChunk(*activeCxxRequestGroup_, hitDemandReplayChunk);
            if (cppMode) RememberCxxGroupReachableFiles(cxxGroupIndex, hitDepPaths);
            if (cppMode) RememberCxxGroupClosureContent(cxxGroupIndex, hitDeps);
            if (cppMode && cxxGroupIndex < cxxImportGroups_.size())
            {
                cxxImportGroups_[cxxGroupIndex].specializedTemplates.insert(
                    hitClassTemplateSpecializations.begin(), hitClassTemplateSpecializations.end());
                for (const auto& [owner, dependency] : hitVariableTemplateDependencies)
                    cxxImportGroups_[cxxGroupIndex].variableTemplateDependencies[owner].insert(dependency);
            }
            replayCxxGroupNamespaces(hitGroupNamespaces);
            // The C++ definitions this header needed were emitted on the cold run; relink the very
            // same bitcode instead of running CodeGen again.
            AdoptCxxCompanionBitcode(hitCxxBitcode);
            RegisterCEnums(hitEnums, fileForLsp, cppMode);
            // Records before sigs so struct-by-value signatures resolve to the same types.
            RegisterCRecords(hitRecords, fileForLsp);
            // After the enum backings and records, so an alias maps to its CFlat name.
            if (cppMode)
                CheckCxxNamespaceConflicts(hitSigs, hitEnums, hitTypeAliases, hitGlobals, hitRecords,
                                           hitClassTemplateNames, hitVariableTemplateNames,
                                           hitClassUsings, hitFunctionTemplates, fileForLsp);
            RegisterRecordAliases(hitAliases);
            RegisterTypeAliasSymbols(hitTypeAliases, cppMode);
            if (cppMode) RegisterCxxUsingDirectives(hitUsingDirectives);
            if (cppMode) RegisterCxxNamespaceAliases(hitNamespaceAliases);
            if (cppMode)
                RegisterCxxFunctionTemplates(hitFunctionTemplates, cxxGroupIndex, fileForLsp);
            RegisterCxxFunctionPointerAbis(hitFunctionPointerAbis);
            if (cppMode && activeCxxRequestGroup_ != nullptr)
            {
                if (cxxGroupIndex < cxxImportGroups_.size())
                    for (const auto& name : hitClassTemplateNames)
                        cxxImportGroups_[cxxGroupIndex].publishedNames.insert(name);
                // Cache-hit replay: the group still owns these names. Signature types are bound
                // on first lookup here too, so the warm path matches the cold one.
                PublishCxxGroupNames(activeCxxRequestGroup_->primary, hitRecords);
            }
            else
                RequestCxxSignatureTypes(hitSigs);
            RegisterCSignatures(hitSigs, fileForLsp);
            RegisterCEnums(hitEnums, fileForLsp, cppMode);
            RegisterCMacros(hitMacros);
            std::vector<CFunctionMacroEntry> retryMacros;
            RegisterCFunctionMacros(hitFuncMacros, fileForLsp, &retryMacros);
            RegisterCGlobals(hitGlobals, fileForLsp);
            RegisterCMacroAliases(hitMacros, hitFuncMacros, fileForLsp);
            std::vector<CFunctionMacroEntry> aliasRetries;
            for (const auto& m : retryMacros)
                if (macroUsesObjectAlias(m, hitMacros))
                {
                    aliasRetryFailed = false;
                    CFunctionMacroEntry rewritten = rewriteObjectAliases(m, hitMacros);
                    if (!aliasRetryFailed)
                        rewritten = expandFunctionMacroAliases(rewritten, hitMacros, hitFuncMacros);
                    std::string translated;
                    if (!aliasRetryFailed && TranslateMacroBody(rewritten, translated))
                        aliasRetries.push_back(std::move(rewritten));
                }
            RegisterCFunctionMacros(aliasRetries, fileForLsp + "@alias-retry");
            ClearCHeaderRefusal(refusalGroupKey);
            if (cppMode) ReportCxxHeaderParseSummary(cxxGroup);
            return true;
        }

        // Persistent disk cache (every import). On hit, preloads the in-memory cache and
        // registers decls, skipping the clang header parse entirely.
        std::filesystem::path cHeaderCacheDir = GetCHeaderCacheDir();
        const uint64_t diskKey = refusalGroupKey;
        /*
         * LSP analysis never writes a C++ header entry, so its own key is never on disk. It reads
         * the entry a compile stored instead - the surface the compiler binds - without the
         * companion bitcode, and never deletes it: a compile owns that file.
         */
        const bool readCompiledEntry = cppMode && !cxxDefinitionsEmitted;
        const uint64_t loadKey = readCompiledEntry
            ? CHeaderDiskCacheKey(realPaths, cIncludeDirs_, cDefines_, extraDefines,
                                  targetWindows_, CInteropTargetTriple(), cppMode,
                                  /*cxxDefinitionsEmitted*/ true, cppStandard_,
                                  ProgramTargetCPUFeatures().first,
                                  ProgramTargetCPUFeatures().second, cDebugInfo_)
            : diskKey;
        if (!mtEc && !cHeaderCacheDir.empty())
        {
            CFileSigCacheEntry diskEntry;
            std::string diskMissReason;
            bool diskHit;
            {
                llvm::TimeTraceScope loadScope("CHeaderJsonLoad", fileForLsp);
                diskHit = TryLoadCHeaderDiskCache(cHeaderCacheDir, loadKey, currentMtime, hashNow,
                                                  diskEntry, /*expectedRequestKey*/ {},
                                                  /*requireBitcode*/ false, &diskMissReason,
                                                  /*removeOnMiss*/ !readCompiledEntry);
            }
            if (!diskHit && verbose && diskMissReason != "missing entry")
                std::cout << std::format("[verbose] C header disk cache miss for {} ({})\n",
                                         fileForLsp, diskMissReason);
            if (diskHit && readCompiledEntry) diskEntry.cxxBitcode.clear();
            if (diskHit && !CxxDemandEntryUsable(cppMode)) diskHit = false;
            if (diskHit)
            {
                NoteCxxDemandEntry(diskEntry.cxxDemandGroupKey, diskEntry.hash, demandEntryKey,
                                   /*served*/ true);
                if (cppMode)
                    NoteCxxDynamicInitializers(diskEntry.cxxDynamicInitializerSymbols);
                if (!diskEntry.cxxDemandGroupKey.empty() && activeCxxRequestGroup_ != nullptr)
                    NoteCxxDemandReplayChunk(*activeCxxRequestGroup_,
                                             diskEntry.cxxDemandReplayChunk);
                // The output's up-to-date check must see the transitive includes on a hit too.
                std::vector<std::string> diskDepPaths;
                for (const auto& dep : diskEntry.deps)
                {
                    RecordDependency(dep.path);
                    diskDepPaths.push_back(dep.path);
                }
                rememberMacroDependencies(diskDepPaths);
                if (cppMode) RememberCxxGroupReachableFiles(cxxGroupIndex, diskDepPaths);
                if (cppMode) RememberCxxGroupClosureContent(cxxGroupIndex, diskEntry.deps);
                for (const auto& classTemplate : diskEntry.classTemplates)
                    cxxClassTemplateParameters_[classTemplate.name] = classTemplate;
                replayCxxGroupNamespaces(diskEntry.cxxGroupNamespaces);
                if (verbose) std::cout << std::format("[verbose] C header disk cache hit for {}\n", fileForLsp);
                {
                    std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                    InsertCFileSigEntry(cacheKey, CFileSigCacheEntry(diskEntry), verbose);
                }
                llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
                { llvm::TimeTraceScope chrStep("CHR:AdoptCxxCompanionBitcode"); AdoptCxxCompanionBitcode(diskEntry.cxxBitcode); }
                SetCInteropTargetFacts(diskEntry.longDoubleWidth,
                                       diskEntry.longDoubleIsIEEEDouble,
                                       diskEntry.targetTriple);
                { llvm::TimeTraceScope chrStep("CHR:RegisterCEnums"); RegisterCEnums(diskEntry.enums, fileForLsp, cppMode); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCRecords"); RegisterCRecords(diskEntry.records, fileForLsp); }
                if (cppMode)
                    { llvm::TimeTraceScope chrStep("CHR:CheckCxxNamespaceConflicts"); CheckCxxNamespaceConflicts(diskEntry.sigs, diskEntry.enums,
                                               diskEntry.typeAliases, diskEntry.globals,
                                               diskEntry.records, diskEntry.classTemplateNames,
                                               diskEntry.variableTemplateNames,
                                               diskEntry.classUsings, diskEntry.functionTemplates, fileForLsp); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterRecordAliases"); RegisterRecordAliases(diskEntry.recordAliases); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterTypeAliasSymbols"); RegisterTypeAliasSymbols(diskEntry.typeAliases, cppMode); }
                if (cppMode) { llvm::TimeTraceScope chrStep("CHR:RegisterCxxUsingDirectives"); RegisterCxxUsingDirectives(diskEntry.usingDirectives); }
                if (cppMode) { llvm::TimeTraceScope chrStep("CHR:RegisterCxxNamespaceAliases"); RegisterCxxNamespaceAliases(diskEntry.namespaceAliases); }
                if (cppMode)
                    { llvm::TimeTraceScope chrStep("CHR:RegisterCxxFunctionTemplates"); RegisterCxxFunctionTemplates(diskEntry.functionTemplates, cxxGroupIndex, fileForLsp); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCxxFunctionPointerAbis"); RegisterCxxFunctionPointerAbis(diskEntry.functionPointerAbis); }
                if (cppMode && activeCxxRequestGroup_ != nullptr)
                {
                    if (cxxGroupIndex < cxxImportGroups_.size())
                    {
                        for (const auto& name : diskEntry.classTemplateNames)
                            cxxImportGroups_[cxxGroupIndex].publishedNames.insert(name);
                        cxxImportGroups_[cxxGroupIndex].specializedTemplates.insert(
                            diskEntry.classTemplateSpecializations.begin(),
                            diskEntry.classTemplateSpecializations.end());
                        for (const auto& [owner, dependency] : diskEntry.variableTemplateDependencies)
                            cxxImportGroups_[cxxGroupIndex].variableTemplateDependencies[owner].insert(
                                dependency);
                    }
                    { llvm::TimeTraceScope chrStep("CHR:PublishCxxGroupNames"); PublishCxxGroupNames(activeCxxRequestGroup_->primary, diskEntry.records); }
                }
                else
                    { llvm::TimeTraceScope chrStep("CHR:RequestCxxSignatureTypes"); RequestCxxSignatureTypes(diskEntry.sigs); }
                if (!cppMode && std::ranges::any_of(diskEntry.sigs,
                                                    [](const CSigEntry& sig) { return sig.isInline; }))
                {
                    std::vector<std::string> hashFiles;
                    hashFiles.reserve(hashFiles.size() + diskEntry.deps.size());
                    for (const auto& dep : diskEntry.deps) hashFiles.push_back(dep.path);
                    hashFiles.insert(hashFiles.end(), realPaths.begin(), realPaths.end());
                    std::sort(hashFiles.begin(), hashFiles.end());
                    hashFiles.erase(std::unique(hashFiles.begin(), hashFiles.end()), hashFiles.end());
                    auto sharedHashFiles = std::make_shared<const std::vector<std::string>>(std::move(hashFiles));
                    for (CSigEntry& sig : diskEntry.sigs)
                        if (sig.isInline)
                        {
                            sig.inlineDefines = extraDefines;
                            sig.inlineHeaders = realPaths;
                            sig.inlineHashFiles = sharedHashFiles;
                        }
                }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCSignatures"); RegisterCSignatures(diskEntry.sigs, fileForLsp); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCEnums"); RegisterCEnums(diskEntry.enums, fileForLsp, cppMode); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCMacros"); RegisterCMacros(diskEntry.macros); }
                std::vector<CFunctionMacroEntry> retryMacros;
                { llvm::TimeTraceScope chrStep("CHR:RegisterCFunctionMacros"); RegisterCFunctionMacros(diskEntry.funcMacros, fileForLsp, &retryMacros); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCGlobals"); RegisterCGlobals(diskEntry.globals, fileForLsp); }
                { llvm::TimeTraceScope chrStep("CHR:RegisterCMacroAliases"); RegisterCMacroAliases(diskEntry.macros, diskEntry.funcMacros, fileForLsp); }
                std::vector<CFunctionMacroEntry> aliasRetries;
                for (const auto& m : retryMacros)
                    if (macroUsesObjectAlias(m, diskEntry.macros))
                    {
                        aliasRetryFailed = false;
                        CFunctionMacroEntry rewritten = rewriteObjectAliases(m, diskEntry.macros);
                        if (!aliasRetryFailed)
                            rewritten = expandFunctionMacroAliases(rewritten, diskEntry.macros, diskEntry.funcMacros);
                        std::string translated;
                        if (!aliasRetryFailed && TranslateMacroBody(rewritten, translated))
                            aliasRetries.push_back(std::move(rewritten));
                    }
                RegisterCFunctionMacros(aliasRetries, fileForLsp + "@alias-retry");
                ClearCHeaderRefusal(refusalGroupKey);
                if (cppMode) ReportCxxHeaderParseSummary(cxxGroup);
                return true;
            }
        }

        const std::string refusalKey = CHeaderRefusalValidityKey(
            refusalGroupKey, realPaths, headerMtimes, hashNow());
        std::string refusalText;
        std::string refusalStalePath;
        if (LookupCHeaderRefusal(refusalGroupKey, refusalKey, refusalText, refusalStalePath))
        {
            if (verbose)
                std::cout << std::format(
                    "[verbose] C header negative MRU hit for {}\n", fileForLsp);
            ReportUncompilableHeader(headerPaths, refusalText, cppMode);
            return false;
        }
        if (verbose && !refusalStalePath.empty())
            std::cout << std::format(
                "[verbose] C header negative MRU stale: {} changed\n", refusalStalePath);

        std::vector<CSigEntry> sigs;
        std::vector<CEnumEntry> enums;
        std::vector<CRecordEntry> records;
        std::vector<CMacroEntry> macros;
        std::vector<CFunctionMacroEntry> funcMacros;
        std::vector<CGlobalEntry> globals;
        std::vector<std::pair<std::string, std::string>> aliases;
        std::vector<CTypeAliasEntry> typeAliases;
        std::vector<cflat_cinterop::RawFunctionTemplate> functionTemplates;
        std::vector<cflat_cinterop::RawClassTemplate> classTemplates;
        std::vector<std::string> classTemplateNames;
        std::vector<std::string> variableTemplateNames;
        std::vector<std::pair<std::string, std::string>> variableTemplateDependencies;
        std::vector<std::string> classTemplateSpecializations;
        std::vector<std::pair<std::string, std::string>> usingDirectives;
        std::vector<std::pair<std::string, std::string>> namespaceAliases;
        std::vector<std::pair<std::string, std::string>> classUsings;
        std::vector<cflat_cinterop::RawFunctionPointerAbi> functionPointerAbis;
        std::vector<cflat_cinterop::RawDynamicInitializer> dynamicInitializers;
        uint64_t longDoubleWidth = 0;
        bool longDoubleIsIEEEDouble = false;
        std::string targetTriple;
        // Collect the transitive include set so the disk entry can validate it: every import is
        // cached, so an edit to an indirectly included header must invalidate the entry.
        bool wantDeps = !cHeaderCacheDir.empty();
        std::vector<std::string> includes;
        std::string cxxBitcode;   // M5 companion module for this group, empty for C imports
        // Request entries are pruned after publication only when their ownership marker is old.
        // A C++ group counts its own parses (AttachCxxHeaderParseGuard); a C header counts here.
        if (!cppMode && !CountTuParse("C header", std::format("{:016x}", refusalGroupKey), fileForLsp,
                                    "header extraction"))
            return false;
        std::unordered_set<std::string> namespacesBeforeExtract;
        {
            // All C entities are extracted in one full parse (plus a cheap preprocess-only prepass
            // for macro names). Uses clang C++ API, not clang-cl or libclang.
            llvm::TimeTraceScope extractScope("CHeaderExtract", fileForLsp);
            bool prereqFailure = false;
            bool headerFailure = false;
            std::string prereqMsg;
            if (cppMode && cxxGroupIndex < cxxImportGroups_.size())
                namespacesBeforeExtract = cxxImportGroups_[cxxGroupIndex].namespaces;
            pendingCxxDemandGroupKey_.clear();
            pendingCxxDemandReplayChunk_ = {};
            cxxEarlyClosureDeps_.clear();
            hashNow();
            includeHashSink_ = wantDeps ? &knownHashes : nullptr;
            struct SinkReset { std::unordered_map<std::string, std::pair<uint64_t, int64_t>>*& sink;
                               ~SinkReset() { sink = nullptr; } } sinkReset{includeHashSink_};
            if (!ExtractCHeaderClang(realPaths, includeHashTime, sigs, enums, records, macros,
                                     funcMacros, globals,
                                     aliases, typeAliases, extraDefines, &includes,
                                     &prereqFailure, &prereqMsg, &headerFailure, cppMode, &cxxBitcode,
                                     &functionPointerAbis, &longDoubleWidth,
                                     &longDoubleIsIEEEDouble, &targetTriple, &functionTemplates,
                                     &usingDirectives, &namespaceAliases, &classTemplateNames,
                                     &variableTemplateNames,
                                     &classUsings, &classTemplateSpecializations, &classTemplates,
                                     &dynamicInitializers, &variableTemplateDependencies))
            {
                if (prereqFailure)
                    ReportOrphanHeader(headerPaths, prereqMsg, cppMode);
                else if (headerFailure)
                {
                    std::vector<CHeaderRefusalDep> refusalDeps;
                    if (CaptureCHeaderRefusalDeps(includes, refusalDeps))
                        RememberCHeaderRefusal(refusalGroupKey, refusalKey, prereqMsg,
                                               std::move(refusalDeps));
                    ReportUncompilableHeader(headerPaths, prereqMsg, cppMode);
                }
                return false;
            }
            rememberMacroDependencies(includes);
            if (cppMode) NoteCxxDynamicInitializers(dynamicInitializers);
            // C inline demand inputs, computed once per group (never per signature, never C++:
            // C++ inline bodies come from the companion module).
            if (!cppMode && std::ranges::any_of(sigs, [](const CSigEntry& sig) { return sig.isInline; }))
            {
                std::vector<std::string> hashFiles = includes;
                hashFiles.insert(hashFiles.end(), realPaths.begin(), realPaths.end());
                std::erase_if(hashFiles, [](const std::string& path) {
                    std::error_code ec;
                    return !std::filesystem::is_regular_file(path, ec);
                });
                std::sort(hashFiles.begin(), hashFiles.end());
                hashFiles.erase(std::unique(hashFiles.begin(), hashFiles.end()), hashFiles.end());
                auto sharedHashFiles = std::make_shared<const std::vector<std::string>>(std::move(hashFiles));
                for (auto& sig : sigs)
                    if (sig.isInline)
                    {
                        sig.inlineHeaders = realPaths;
                        sig.inlineHashFiles = sharedHashFiles;
                    }
            }
        }
        for (const auto& classTemplate : classTemplates)
            cxxClassTemplateParameters_[classTemplate.name] = classTemplate;
        if (cppMode) RememberCxxGroupReachableFiles(cxxGroupIndex, includes);
        if (cppMode && cxxGroupIndex < cxxImportGroups_.size())
        {
            cxxImportGroups_[cxxGroupIndex].specializedTemplates.insert(
                classTemplateSpecializations.begin(), classTemplateSpecializations.end());
            for (const auto& [owner, dependency] : variableTemplateDependencies)
                cxxImportGroups_[cxxGroupIndex].variableTemplateDependencies[owner].insert(dependency);
        }
        std::vector<std::string> seededGroupNamespaces;
        if (cppMode && cxxGroupIndex < cxxImportGroups_.size())
        {
            for (const auto& name : cxxImportGroups_[cxxGroupIndex].namespaces)
                if (namespacesBeforeExtract.count(name) == 0) seededGroupNamespaces.push_back(name);
            std::sort(seededGroupNamespaces.begin(), seededGroupNamespaces.end());
        }

        if (!mtEc)
        {
            llvm::TimeTraceScope cacheScope("CHeaderCacheAssemble");
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.longDoubleWidth = longDoubleWidth;
            entry.longDoubleIsIEEEDouble = longDoubleIsIEEEDouble;
            entry.targetTriple = targetTriple;
            entry.sigs  = sigs;
            entry.functionTemplates = functionTemplates;
            entry.classTemplates = classTemplates;
            entry.classTemplateNames = classTemplateNames;
            entry.variableTemplateNames = variableTemplateNames;
            entry.variableTemplateDependencies = variableTemplateDependencies;
            entry.classTemplateSpecializations = classTemplateSpecializations;
            entry.cxxGroupNamespaces = seededGroupNamespaces;
            entry.enums = enums;
            entry.records = records;
            entry.macros = macros;
            entry.funcMacros = funcMacros;
            entry.globals = globals;
            entry.recordAliases = aliases;
            entry.typeAliases = typeAliases;
            entry.usingDirectives = usingDirectives;
            entry.namespaceAliases = namespaceAliases;
            entry.classUsings = classUsings;
            entry.functionPointerAbis = functionPointerAbis;
            entry.cxxDynamicInitializerSymbols = dynamicInitializers;
            entry.cxxBitcode = cxxBitcode;
            entry.cxxDemandGroupKey = std::move(pendingCxxDemandGroupKey_);
            pendingCxxDemandGroupKey_.clear();
            entry.cxxDemandReplayChunk = std::move(pendingCxxDemandReplayChunk_);
            pendingCxxDemandReplayChunk_ = {};
            if (!entry.cxxDemandGroupKey.empty())
            {
                NoteCxxDemandEntry(entry.cxxDemandGroupKey, entry.hash, demandEntryKey,
                                   /*served*/ false);
                cxxDemandLiveKeys_.insert(cacheKey);
            }
            // Keep only real on-disk paths in the transitive dependency list.
            // Non-existent deps would otherwise poison every later cache validation.
            if (wantDeps && !cxxEarlyClosureDeps_.empty())
            {
                // Already hashed before registration (see ExtractCHeaderClang).
                for (const auto& inc : includes) RecordDependency(inc);
                entry.deps = std::move(cxxEarlyClosureDeps_);
                cxxEarlyClosureDeps_.clear();
            }
            else if (wantDeps)
            {
                CollectCHeaderDependencies(includes, knownHashes, entry.deps);
                for (const auto& inc : includes) RecordDependency(inc);
                if (cppMode) RememberCxxGroupClosureContent(cxxGroupIndex, entry.deps);
            }
            // --run is read-only: never persist the header cache to disk.
            // The in-memory entry still serves this compile.
            // Nor does LSP analysis write: its entry carries a bound surface with no bodies
            // (see the cache-key comment above), which is not a result a compile may reuse.
            if (!runMode_ && symbolSink_ == nullptr && !cHeaderCacheDir.empty())
                ScheduleCHeaderDiskCacheWrite(cHeaderCacheDir, diskKey, currentMtime, hashNow(),
                                              CFileSigCacheEntry(entry));
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            InsertCFileSigEntry(cacheKey, std::move(entry), verbose);
        }

        // Records were already registered inside ExtractCHeaderClang.
        {
            llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
            if (cppMode)
                { llvm::TimeTraceScope chrStep("CHR:CheckCxxNamespaceConflicts"); CheckCxxNamespaceConflicts(sigs, enums, typeAliases, globals, records,
                                           classTemplateNames, variableTemplateNames, classUsings,
                                           functionTemplates, fileForLsp); }
            if (cppMode)
                { llvm::TimeTraceScope chrStep("CHR:RegisterCxxFunctionTemplates"); RegisterCxxFunctionTemplates(functionTemplates, cxxGroupIndex, fileForLsp); }
            if (cppMode && cxxGroupIndex < cxxImportGroups_.size())
            {
                for (const auto& name : classTemplateNames)
                    cxxImportGroups_[cxxGroupIndex].publishedNames.insert(name);
                cxxImportGroups_[cxxGroupIndex].specializedTemplates.insert(
                    classTemplateSpecializations.begin(), classTemplateSpecializations.end());
            }
            { llvm::TimeTraceScope chrStep("CHR:RegisterCSignatures"); RegisterCSignatures(sigs, fileForLsp); }
            if (!cppMode) { llvm::TimeTraceScope chrStep("CHR:RegisterTypeAliasSymbols"); RegisterTypeAliasSymbols(typeAliases, false); }
            { llvm::TimeTraceScope chrStep("CHR:RegisterCEnums"); RegisterCEnums(enums, fileForLsp, cppMode); }
            { llvm::TimeTraceScope chrStep("CHR:RegisterCMacros"); RegisterCMacros(macros); }
            std::vector<CFunctionMacroEntry> retryMacros;
            { llvm::TimeTraceScope chrStep("CHR:RegisterCFunctionMacros"); RegisterCFunctionMacros(funcMacros, fileForLsp, &retryMacros); }
            { llvm::TimeTraceScope chrStep("CHR:RegisterCGlobals"); RegisterCGlobals(globals, fileForLsp); }
            { llvm::TimeTraceScope chrStep("CHR:RegisterCMacroAliases"); RegisterCMacroAliases(macros, funcMacros, fileForLsp); }
            std::vector<CFunctionMacroEntry> aliasRetries;
            for (const auto& m : retryMacros)
                if (macroUsesObjectAlias(m, macros))
                {
                    aliasRetryFailed = false;
                    CFunctionMacroEntry rewritten = rewriteObjectAliases(m, macros);
                    if (!aliasRetryFailed)
                        rewritten = expandFunctionMacroAliases(rewritten, macros, funcMacros);
                    std::string translated;
                    if (!aliasRetryFailed && TranslateMacroBody(rewritten, translated))
                        aliasRetries.push_back(std::move(rewritten));
                }
            { llvm::TimeTraceScope chrStep("CHR:RegisterCFunctionMacros"); RegisterCFunctionMacros(aliasRetries, fileForLsp + "@alias-retry"); }
        }
        ClearCHeaderRefusal(refusalGroupKey);
        return true;
    }

/*
 * ============================================================================================
 * M4b - foreign NONTRIVIAL C++ class lifetime
 *
 * Everything below serves ONE invariant: a nontrivial imported C++ object is created, copied,
 * moved and destroyed by its own C++ special members, on storage CFlat allocated, and never by
 * a CFlat bitwise store. The declaration site allocates the slot and calls a constructor INTO
 * it (see MainListener::TryDeclareForeignCxxLocal); the destructor is registered as the class's
 * CFlat destructor so the existing scope-exit machinery is the only cleanup path.
 * ============================================================================================
 */

llvm::Function* LLVMBackend::GetOrCreateCxxStructor(const std::string& typeName,
                                                    const CxxClassInfo::Structor& st,
                                                    AbiRecipe& recipeOut)
{
        if (st.linkageName.empty() || st.params.empty()) return nullptr;
        if (!BuildAbiRecipeFromClangPlan(typeName, st.abi, st.ret, st.params, recipeOut))
            return nullptr;
        llvm::FunctionType* fnTy = BuildExternFunctionType(st.ret, st.params, /*varargs*/ false,
                                                           recipeOut);
        if (fnTy == nullptr) return nullptr;
        if (llvm::Function* existing = module->getFunction(st.linkageName))
        {
            if (existing->getFunctionType() != fnTy)
            {
                // Every user-facing name is demangled; the Itanium symbol is not a spelling
                // the user wrote. llvm::demangle returns the input unchanged if it is not one.
                LogError(std::format(
                    "internal: C++ special member '{}' of '{}' was already declared with a "
                    "different signature", llvm::demangle(st.linkageName),
                    DisplayCxxClassName(typeName)));
                return nullptr;
            }
            return existing;
        }
        auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage,
                                          st.linkageName, module.get());
        ApplyAbiAttributes(fn, recipeOut);
        return fn;
    }

bool LLVMBackend::CxxConstantDefaultsFrom(const CxxClassInfo::Structor& st, size_t first)
{
        if (st.defaultArgs.size() != st.params.size()) return false;
        for (size_t i = first; i < st.defaultArgs.size(); ++i)
        {
            const std::string& k = st.defaultArgs[i].kind;
            if (k != "int" && k != "bool" && k != "enum" && k != "float" && k != "double"
                && k != "nullptr")
                return false;
        }
        return first < st.params.size();
}

bool LLVMBackend::LinkCInlineDemandBodies()
{
        /*
         * A plain C99 inline the program calls follows clang for the target and opt level:
         * MSVC C inline semantics emit the body (linkonce_odr); elsewhere clang -O0 emits nothing
         * (an external definition must exist), clang -O1+ an available_externally copy. Either
         * body becomes a private copy, so it links exactly as the inlined clang call would.
         */
        const bool msvcInline = module->getTargetTriple().isWindowsMSVCEnvironment();
        std::set<std::string> processed;
        std::set<std::string> directlyUsedPlainInline;
        for (const auto& [name, request] : cInlineBodyRequests_)
            if (!request.isStatic)
                if (llvm::Function* function = module->getFunction(name);
                    function != nullptr && !function->use_empty())
                    directlyUsedPlainInline.insert(name);
        const bool skipDirectPlainInline = !msvcInline && cOptLevel_ == 0;
        while (true)
        {
            std::map<std::string, std::vector<CInlineBodyRequest>> byHeader;
            for (const auto& [name, request] : cInlineBodyRequests_)
            {
                llvm::Function* function = module->getFunction(name);
                if (processed.contains(name)
                    || (skipDirectPlainInline && directlyUsedPlainInline.contains(name))
                    || function == nullptr
                    || !function->isDeclaration() || function->use_empty()) continue;
                // Off MSVC a plain inline used only by address resolves to its external definition
                // (C99, as clang), so it needs no body - and requesting one late would reparse.
                if (!msvcInline && !request.isStatic
                    && std::none_of(function->users().begin(), function->users().end(),
                        [function](const llvm::User* user) {
                            auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                            return call != nullptr && call->getCalledOperand() == function;
                        }))
                    continue;
                std::string groupKey = request.header;
                for (const auto& path : request.headers) groupKey += "\nH" + path;
                for (const auto& define : request.defines) groupKey += "\nD" + define;
                byHeader[groupKey].push_back(request);
            }
            if (byHeader.empty()) return true;

            for (auto& [groupKey, functions] : byHeader)
            {
            (void)groupKey;
            std::sort(functions.begin(), functions.end(), [](const auto& a, const auto& b) {
                return a.name < b.name;
            });
            const std::string& header = functions.front().header;
            const std::vector<std::string>& headers = functions.front().headers;
            const std::string dir = std::filesystem::path(header).parent_path().string();
            const std::vector<std::string>& extraDefines = functions.front().defines;
            std::vector<std::string> args = BuildClangDriverArgs(dir, extraDefines, true, false);
            // -g reaches the bodies too (CodeView on Windows, like CFlat's own); the args fold into
            // the key below, so a -g body and a non-g body never share a c-inline entry.
            if (cDebugInfo_) args.push_back("-g");
            if (cDebugInfo_ && targetWindows_) args.push_back("-gcodeview");
            uint64_t keyHash = 14695981039346656037ULL;
            auto fold = [&keyHash](std::string_view value) {
                for (unsigned char c : value) { keyHash ^= c; keyHash *= 1099511628211ULL; }
                keyHash ^= '|'; keyHash *= 1099511628211ULL;
            };
            fold("c-inline-v9");
            std::vector<std::string> hashFiles;
            if (functions.front().hashFiles) hashFiles = *functions.front().hashFiles;
            if (hashFiles.empty()) hashFiles.push_back(header);
            for (const auto& path : hashFiles)
            {
                uint64_t contentHash = 0;
                if (!HashFileFnv1a(path, contentHash))
                {
                    LogError("clang: cannot hash inline header dependency '" + path + "'");
                    return false;
                }
                fold(path);
                fold(std::to_string(contentHash));
            }
            for (const auto& arg : args) fold(arg);
            std::vector<std::pair<std::string, bool>> wanted;
            for (const auto& request : functions)
            {
                fold(request.name);
                fold(request.isStatic ? "static" : "inline");
                wanted.emplace_back(request.name, request.isStatic);
            }

            const std::string cacheDir = noCache_ ? std::string{} : GetCHeaderCacheDir();
            std::string bitcode;
            std::filesystem::path cachePath;
            if (!cacheDir.empty())
            {
                cachePath = std::filesystem::path(cacheDir) / "c-inline"
                    / (std::format("{:016x}.bc", keyHash));
                std::ifstream in(cachePath, std::ios::binary);
                if (in) bitcode.assign(std::istreambuf_iterator<char>(in), {});
            }
            if (bitcode.empty())
            {
                if (!CountTuParse("C inline bodies", groupKey, header, "demand body emission"))
                    return false;
                std::string error;
                if (!cflat_cinterop::EmitCInlineBodies(args, headers, wanted, bitcode, error))
                {
                    if (!error.starts_with("clang: ")) error = "clang: " + error;
                    LogError(error);
                    return false;
                }
                if (!cachePath.empty())
                {
                    std::error_code ec;
                    std::filesystem::create_directories(cachePath.parent_path(), ec);
                    if (!ec)
                    {
                        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
                        if (out) out.write(bitcode.data(), static_cast<std::streamsize>(bitcode.size()));
                    }
                }
            }
            if (bitcode.empty())
            {
                LogError("clang: inline body compilation produced empty bitcode for '" + header + "'");
                return false;
            }
            auto buffer = llvm::MemoryBuffer::getMemBufferCopy(bitcode, "cflat-c-inline-demand");
            auto parsed = llvm::parseBitcodeFile(buffer->getMemBufferRef(), module->getContext());
            if (!parsed)
            {
                LogError("clang: cannot read cached inline body bitcode for '" + header + "'");
                return false;
            }
            (*parsed)->setDataLayout(module->getDataLayout());
            std::map<std::string, std::string> localNames;
            for (const auto& request : functions)
            {
                llvm::Function* body = (*parsed)->getFunction(request.name);
                // No body (gnu_inline, not emitted): the program's declaration stays external.
                if (body == nullptr || body->isDeclaration()) continue;
                // An MSVC C inline (linkonce_odr, comdat) becomes a private copy like the rest:
                // under its own name it collides on COFF with a strong or imported definition.
                if (!request.isStatic && body->hasAvailableExternallyLinkage() && cOptLevel_ == 0
                    && directlyUsedPlainInline.contains(request.name))
                {
                    body->deleteBody();
                    continue;
                }
                uint64_t localHash = 14695981039346656037ULL;
                for (unsigned char c : request.header)
                    { localHash ^= c; localHash *= 1099511628211ULL; }
                const std::string localName = std::format("__cflat_c_inline_{:016x}_{}",
                    localHash, request.name);
                // A plain inline is external: only direct calls take the private copy, its address
                // stays the real symbol so it compares equal across TUs (C11 6.5.9). A static
                // inline is this program's own copy, so every use moves.
                auto isDirectCall = [](const llvm::Use& use) {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(use.getUser());
                    return call != nullptr && call->isCallee(&use);
                };
                auto redirect = [&](llvm::Function* from, llvm::Function* to) {
                    if (request.isStatic) from->replaceAllUsesWith(to);
                    else from->replaceUsesWithIf(to, isDirectCall);
                };
                bool programTakesAddress = false;
                if (llvm::Function* declaration = module->getFunction(request.name))
                {
                    auto* localDeclaration = llvm::Function::Create(
                        declaration->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                        llvm::Twine(localName), module.get());
                    localDeclaration->setCallingConv(declaration->getCallingConv());
                    redirect(declaration, localDeclaration);
                    if (declaration->use_empty() && declaration->isDeclaration())
                        declaration->eraseFromParent();
                    else programTakesAddress = !declaration->use_empty();
                }
                if (!request.isStatic)
                {
                    // Address uses inside the bodies bind the real symbol too.
                    const bool msvcComdatBody = body->hasLinkOnceLinkage();
                    auto* external = llvm::Function::Create(body->getFunctionType(),
                        llvm::GlobalValue::ExternalLinkage, "", parsed->get());
                    external->setCallingConv(body->getCallingConv());
                    body->replaceUsesWithIf(external, [&](const llvm::Use& use) {
                        return !isDirectCall(use);
                    });
                    body->setName(localName);
                    if (external->use_empty() && !programTakesAddress)
                        external->eraseFromParent();
                    else if (!msvcComdatBody)
                        external->setName(request.name);
                    else
                    {
                        // MSVC C inline semantics: no TU holds an external definition, every TU
                        // emits a linkonce_odr comdat copy and the linker keeps one, so the real
                        // name stays that comdat body here too (clang-cl's shape) and the
                        // address links and compares equal across TUs.
                        llvm::ValueToValueMapTy valueMap;
                        llvm::Function* comdatBody = llvm::CloneFunction(body, valueMap);
                        external->replaceAllUsesWith(comdatBody);
                        external->eraseFromParent();
                        comdatBody->setName(request.name);
                        comdatBody->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
                        comdatBody->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
                        llvm::Comdat* comdat = (*parsed)->getOrInsertComdat(request.name);
                        comdat->setSelectionKind(llvm::Comdat::Any);
                        comdatBody->setComdat(comdat);
                    }
                }
                body->setName(localName);
                body->setLinkage(llvm::GlobalValue::ExternalLinkage);
                body->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
                body->setComdat(nullptr);
                localNames.emplace(request.name, localName);
            }
            llvm::Linker linker(*module);
            if (linker.linkInModule(std::move(*parsed)))
            {
                LogError("clang: cannot link inline bodies from '" + header + "'");
                return false;
            }
            for (const auto& [name, localName] : localNames)
            {
                llvm::Function* body = module->getFunction(localName);
                (void)name;
                if (body != nullptr) body->setLinkage(llvm::GlobalValue::InternalLinkage);
            }
            for (const auto& request : functions) processed.insert(request.name);
            }
        }
}

