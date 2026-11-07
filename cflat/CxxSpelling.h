#pragma once

#include "LLVMBackend.h"

enum class CxxBraceContainerKind
{
    Unsupported,
    InitializerList,
    ArrayRef,
    Vector,
    PointerPairClass
};

/*
 * M5b - a C++ REFERENCE at a bound boundary is CFlat's `alias T`: the same machine representation
 * (a pointer) and the same rules (an lvalue passes its address, an rvalue materializes a temporary).
 * That makes `v.push_back(3)`, `s.append(other)` and `v[0] = 5` work without a spelling for
 * references in CFlat. Returns the referred-to spelling, or the input unchanged.
 */
enum class CxxReferenceKind { None, Lvalue, Rvalue, RefToPointer };

std::string ClangClCppStandard(const std::string& standard);
bool IsSystemCxxHeaderPath(const std::string& path);
uint64_t Fnv1aBytes(std::string_view bytes);
std::string StripCxxRefAndCv(std::string_view spelling);
uint64_t HashWrapperKey(std::string_view key);
void EraseDeclaratorToken(std::string& s, const char* word);
bool CxxSpellingIsBareNullptrT(std::string spelled);
bool CxxSpellingHasTopLevelParen(const std::string& spelling);
bool IsCxxClosureReferenceResult(std::string spelling);
std::string CxxClosureReferenceResultRefusal(const std::string& callee);
bool CxxParamIsConstLvalueReference(const std::string& spelling);
bool CxxRvalueReferenceIsConst(const std::string& spelling);
bool CxxSpellingHasVolatileTarget(const std::string& spelling);
bool CxxParamIsConstRefToPointer(const std::string& spelling);
uint32_t CSpellingPointeeConstMask(const std::string& spelling, int depth);
bool CxxSpellingHasConstPointee(const std::string& spelling);
uint32_t CxxSpellingPointeeConstMask(const std::string& spelling, int depth);
bool CxxSpellingHasTopLevelConst(const std::string& spelling);
bool CxxSpellingHasTopLevelObjectConst(const std::string& spelling);
std::string CxxMemberValueSpelling(const std::string& spelling);
std::string CxxFieldIdentitySpelling(const std::string& spelling);
std::string CxxDefaultWrapperName(const std::string& linkageName, size_t omittedArity);
bool HasNonConstDefaultSuffix(const std::vector<cflat_cinterop::RawDefaultArg>& defaults,
                                     size_t first);
std::string CxxNameFromCflat(const std::string& name);

template <class RawRecord>
std::string CxxRecordOwnerSpelling(const RawRecord& record)
{
        return record.canonicalCtype.empty() ? CxxNameFromCflat(record.name)
                                             : "::" + record.canonicalCtype;
}
void PrependCv(std::string& spelling, std::string_view cv);
bool ParseCxxArrayParameter(const std::string& spelling, std::string& element,
                                   uint64_t& extent);
std::vector<std::string> CxxRequestIncludeRoots(const std::vector<std::string>& headers);
std::string FirstCxxErrorLine(const std::string& text);
std::string TrimCxxBraceType(std::string text);
std::string CxxBraceTargetElementSpelling(const std::string& parameter);
bool CxxBraceParameterIsPointer(const std::string& parameter);
CxxBraceContainerKind CxxBraceContainerKindOf(const std::string& parameter);
bool CxxArrayReferenceParameter(const std::string& parameter, std::string& element,
                                       std::string& extent);
std::string CxxInitializerListOverStorage(const std::string& listType, const std::string& il,
                                                 const std::string& p, size_t count);
bool CxxBraceElementsAllScalar(const LLVMBackend::CxxBraceArgument& brace);
std::string CxxZeroLiteralSpelling(const std::string& identity);
std::string CxxFunctionPointerReferentParameter(const std::string& parameter);
std::string CxxTypeofInner(const std::string& spelling);
bool IsCxxConstRefToPointerSpelling(std::string type);
bool IsCxxArrayReferenceSpelling(const std::string& type);
bool CxxBraceFieldIsReference(const std::string& spelling);
bool CxxBraceFieldIsFunctionReference(const std::string& spelling);
bool CxxBraceFieldIsRvalueReference(const std::string& spelling);
std::string CxxNamedParameter(const std::string& spelling,
                                     const std::string& parameterName);
std::string NormalizeCxxOwnershipPath(const std::string& value);
std::string CxxSpellingWithoutRef(const std::string& spelling,
                                         CxxReferenceKind* kind = nullptr);
bool IsPlainCFlatIdentifier(const std::string& name);
bool HasAdjacentIdentifiers(const std::string& text);
bool CxxTernaryArmConst(const LLVMBackend::TypeAndValue& arm);
const char* CxxArmCategoryTag(LLVMBackend::CxxArmCategory category);
void MarkCxxDeclaredConstField(LLVMBackend::TypeAndValue& tv, bool isConst);
bool CxxIncrementalSpellingSafe(const std::string& spelling);
std::string DirectBraceConstantText(const LLVMBackend::NamedVariable& element,
                                           const std::string& spelling, bool nullConstant);
std::string CxxRequestOdrUsePreamble();
std::string BuildStdFunctionCtorUse(const std::string& cxxSpelling,
                                           const std::string& marker);
