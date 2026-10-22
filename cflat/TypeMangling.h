#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "LLVMBackend.h"

struct TypeSpelling
{
    std::string base;
    std::string canonicalPrimitive;
    std::vector<TypeSpelling> args;
    int pointerDepth = 0;
    bool view = false;
    bool alias = false;
    bool unique = false;
    bool value = false;
    bool closure = false;
    bool thinClosure = false;
    bool bareFunction = false;
    bool move = false;
    // Pointee const of a generic type argument: const levels counted from the innermost
    // pointee (`(const int)*` = 1, `(const int*)*` = 2). Never the argument's own level.
    int constDepth = 0;
    std::string encodedName;
};

struct FunctionSymbolSpelling
{
    std::string name;
    TypeSpelling returnType;
    std::vector<TypeSpelling> parameters;
    std::vector<bool> move;
    bool varargs = false;
};

std::string MangleType(const LLVMBackend& compiler,
                       const LLVMBackend::TypeAndValue& type);
std::string MangleType(const LLVMBackend& compiler, const TypeSpelling& spelling);
std::string MangleGenericInstance(const LLVMBackend& compiler, std::string_view base,
                                  const std::vector<std::string>& args);
bool DemangleType(const LLVMBackend& compiler, std::string_view mangled, TypeSpelling& out);
std::string MangledGenericArgument(const LLVMBackend& compiler, std::string_view mangled,
                                   size_t index = 0);
// Argument count for a mangled generic instance the mangler itself never produced (a C++
// foreign identity built from a clang spelling), so DemangleType can split it.
void RememberMangledArity(const LLVMBackend& compiler, std::string_view mangled, size_t arity);

std::string SpellType(const LLVMBackend& compiler,
                      const LLVMBackend::TypeAndValue& type);
std::string SpellTypeForSymbolDump(const LLVMBackend& compiler,
                                   const LLVMBackend::TypeAndValue& type);
std::string SpellDiagnosticType(const LLVMBackend& compiler,
                                const LLVMBackend::TypeAndValue& type);
std::string_view MangledBase(std::string_view mangled);
// A generic type argument carrying pointee const, spelled "(const int)*" (mangled ".c1$.p$int")
// or "(const int)[]" (".c1$.v$int"): the argument without it and its const depth. False if none.
bool SplitConstTypeArgument(std::string_view arg, std::string& plain, int& constDepth);
// Inverse of SplitConstTypeArgument for a spelled argument; the depth is capped below the
// argument's own level (a generic argument never carries top-level const).
std::string JoinConstTypeArgument(const std::string& plain, int constDepth);
// Declared defaults of a template's generic parameters (parallel to its parameter list; empty
// string = no default). Null when the template name is unknown.
const std::vector<std::string>* GenericValueDefaultsFor(const LLVMBackend& compiler,
                                                        std::string_view base);
bool IsThinMangledClosure(std::string_view mangled);

std::string MangleFunctionSymbol(const LLVMBackend& compiler, std::string_view name,
                                 const LLVMBackend::TypeAndValue& returnType,
                                 const std::vector<LLVMBackend::TypeAndValue>& parameters,
                                 bool varargs = false);
std::string MangleFunctionSymbol(const LLVMBackend& compiler,
                                 const FunctionSymbolSpelling& spelling);
bool DemangleFunctionSymbol(const LLVMBackend& compiler, std::string_view symbol,
                            FunctionSymbolSpelling& out);
std::string SpellFunctionSymbol(const LLVMBackend& compiler, std::string_view symbol);

std::string MangleClosureType(const LLVMBackend& compiler, bool isThin,
                              const std::string& ret, int retDepth,
                              const std::vector<std::pair<std::string, int>>& params,
                              int retConstMask = -1, const std::vector<int>* paramConstMasks = nullptr,
                              const std::vector<bool>* paramArrayViews = nullptr);
