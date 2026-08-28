#pragma once

// Small LLVM API shims shared by the LLVM 18, 22 and 23 builds.

#include <string>
#include <cstdint>
#include <optional>
#include <memory>
#include <utility>
#include <type_traits>

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/AArch64TargetParser.h>

#if defined(CFLAT_LLVM_COMPAT_CLANG)
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#if LLVM_VERSION_MAJOR >= 20
#include <clang/Driver/CreateInvocationFromArgs.h>
#else
#include <clang/Frontend/Utils.h>
#endif
#endif

namespace cflat_llvm_compat
{

// LLVM 23 turned BasicBlock::getTerminator() into an asserting accessor that
// returns the list sentinel when the block has no terminator, and added
// getTerminatorOrNull() for the nullable query. cflat uses the nullable form
// throughout, and because LLVM is built with assertions off the 23 accessor
// would silently hand back garbage instead of null.
inline llvm::Instruction* GetTerminatorOrNull(llvm::BasicBlock* block)
{
    if (block == nullptr) return nullptr;
#if LLVM_VERSION_MAJOR >= 23
    return block->getTerminatorOrNull();
#else
    return block->getTerminator();
#endif
}

inline const llvm::Instruction* GetTerminatorOrNull(const llvm::BasicBlock* block)
{
    if (block == nullptr) return nullptr;
#if LLVM_VERSION_MAJOR >= 23
    return block->getTerminatorOrNull();
#else
    return block->getTerminator();
#endif
}

// LLVM 23 split the Br opcode into UncondBr/CondBr and deprecated BranchInst.
// isa_and_nonnull because the terminator query above is nullable.
inline bool IsBranch(const llvm::Instruction* terminator)
{
#if LLVM_VERSION_MAJOR >= 23
    return llvm::isa_and_nonnull<llvm::UncondBrInst, llvm::CondBrInst>(terminator);
#else
    return llvm::isa_and_nonnull<llvm::BranchInst>(terminator);
#endif
}

// AArch64 keeps CPU aliases (apple-m1, cyclone, ...) out of the subtarget's
// processor table, so enumerating CPUs has to merge them in separately. The
// field holding the alias spelling was renamed AltName in LLVM 21.
inline llvm::StringRef AArch64AliasName(
    const std::remove_reference_t<decltype(llvm::AArch64::CpuAliases[0])>& alias)
{
#if LLVM_VERSION_MAJOR >= 23
    // 23 turned the alias fields into StringTable offsets into AArch64::StrTab.
    return llvm::AArch64::StrTab[alias.AltName];
#elif LLVM_VERSION_MAJOR >= 21
    return alias.AltName;
#else
    return alias.Alias;
#endif
}

// LLVM 23 made SubtargetSubTypeKV use a relative string offset, replacing the
// Key member with a key() accessor.
inline const char* SubtargetKey(const llvm::SubtargetSubTypeKV& kv)
{
#if LLVM_VERSION_MAJOR >= 23
    return kv.key();
#else
    return kv.Key;
#endif
}

// LLVM 23 dropped the string-triple overload of lookupTarget; only the
// Triple-typed form and the (ArchName, Triple&) form remain.
inline const llvm::Target* LookupTarget(llvm::StringRef triple, std::string& err)
{
#if LLVM_VERSION_MAJOR >= 23
    return llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
#else
    return llvm::TargetRegistry::lookupTarget(triple, err);
#endif
}

inline void SetModuleTriple(llvm::Module& module, llvm::StringRef triple)
{
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(llvm::Triple(triple));
#else
    module.setTargetTriple(triple);
#endif
}

inline std::string GetModuleTripleStr(const llvm::Module& module)
{
#if LLVM_VERSION_MAJOR >= 21
    return module.getTargetTriple().str();
#else
    return module.getTargetTriple();
#endif
}

inline llvm::Function* GetIntrinsicDecl(llvm::Module* module, llvm::Intrinsic::ID id,
                                        llvm::ArrayRef<llvm::Type*> types = {})
{
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(module, id, types);
#else
    return llvm::Intrinsic::getDeclaration(module, id, types);
#endif
}

// llvm.va_start / va_end became overloaded on the va_list pointer type in LLVM 20
// ("llvm.va_start.p0"). Passing the type on 18 mangles a name that release does not
// recognize as an intrinsic, so the call links as an undefined external symbol.
inline llvm::Function* GetVaIntrinsicDecl(llvm::Module* module, llvm::Intrinsic::ID id,
                                          llvm::Type* vaListPointerType)
{
#if LLVM_VERSION_MAJOR >= 20
    return GetIntrinsicDecl(module, id, { vaListPointerType });
#else
    (void)vaListPointerType;
    return GetIntrinsicDecl(module, id);
#endif
}

inline llvm::CallInst* CreateMaskedGather(llvm::IRBuilder<>& builder, llvm::Module* module,
                                          llvm::Type* vectorType, llvm::Type* pointerVectorType,
                                          llvm::Value* pointers, llvm::Value* mask,
                                          llvm::Value* passthru, uint32_t alignment,
                                          llvm::Twine name = {})
{
    auto* function = GetIntrinsicDecl(module, llvm::Intrinsic::masked_gather,
                                      { vectorType, pointerVectorType });
#if LLVM_VERSION_MAJOR >= 22
    return builder.CreateCall(function, { pointers, mask, passthru }, name);
#else
    return builder.CreateCall(function,
        { pointers, builder.getInt32(alignment), mask, passthru }, name);
#endif
}

inline llvm::CallInst* CreateMaskedLoad(llvm::IRBuilder<>& builder, llvm::Module* module,
                                        llvm::Type* vectorType, llvm::Type* pointerType,
                                        llvm::Value* pointer, llvm::Value* mask,
                                        llvm::Value* passthru, uint32_t alignment,
                                        llvm::Twine name = {})
{
    auto* function = GetIntrinsicDecl(module, llvm::Intrinsic::masked_load,
                                      { vectorType, pointerType });
#if LLVM_VERSION_MAJOR >= 22
    return builder.CreateCall(function, { pointer, mask, passthru }, name);
#else
    return builder.CreateCall(function,
        { pointer, builder.getInt32(alignment), mask, passthru }, name);
#endif
}

inline llvm::CallInst* CreateMaskedStore(llvm::IRBuilder<>& builder, llvm::Module* module,
                                         llvm::Type* vectorType, llvm::Type* pointerType,
                                         llvm::Value* value, llvm::Value* pointer,
                                         llvm::Value* mask, uint32_t alignment)
{
    auto* function = GetIntrinsicDecl(module, llvm::Intrinsic::masked_store,
                                      { vectorType, pointerType });
#if LLVM_VERSION_MAJOR >= 22
    return builder.CreateCall(function, { value, pointer, mask });
#else
    return builder.CreateCall(function,
        { value, pointer, builder.getInt32(alignment), mask });
#endif
}

inline llvm::CallInst* CreateMaskedScatter(llvm::IRBuilder<>& builder, llvm::Module* module,
                                           llvm::Type* vectorType, llvm::Type* pointerVectorType,
                                           llvm::Value* value, llvm::Value* pointers,
                                           llvm::Value* mask, uint32_t alignment)
{
    auto* function = GetIntrinsicDecl(module, llvm::Intrinsic::masked_scatter,
                                      { vectorType, pointerVectorType });
#if LLVM_VERSION_MAJOR >= 22
    return builder.CreateCall(function, { value, pointers, mask });
#else
    return builder.CreateCall(function,
        { value, pointers, builder.getInt32(alignment), mask });
#endif
}

inline llvm::StringRef JitLinkSymbolName(const llvm::jitlink::Symbol& symbol)
{
#if LLVM_VERSION_MAJOR >= 21
    return *symbol.getName();
#else
    return symbol.getName();
#endif
}

inline llvm::TargetMachine* CreateTargetMachine(
    const llvm::Target& target, llvm::StringRef triple, llvm::StringRef cpu,
    llvm::StringRef features, const llvm::TargetOptions& options,
    std::optional<llvm::Reloc::Model> reloc)
{
#if LLVM_VERSION_MAJOR >= 21
    return target.createTargetMachine(llvm::Triple(triple), cpu, features, options, reloc);
#else
    return target.createTargetMachine(triple, cpu, features, options, reloc);
#endif
}

template <typename FieldT, typename ContextT>
inline unsigned GetBitWidthValue(const FieldT& field, const ContextT& context)
{
#if LLVM_VERSION_MAJOR >= 21
    return field.getBitWidthValue();
#else
    return field.getBitWidthValue(context);
#endif
}

#if defined(CFLAT_LLVM_COMPAT_CLANG)

template <typename DiagnosticsEngineT, typename DiagnosticIDsPtrT,
          typename DiagnosticOptionsT>
class ClangDiagnosticsState
{
public:
#if LLVM_VERSION_MAJOR >= 21
    DiagnosticOptionsT options;
#else
    llvm::IntrusiveRefCntPtr<DiagnosticOptionsT> options;
#endif
    llvm::IntrusiveRefCntPtr<DiagnosticsEngineT> engine;

    template <typename ConsumerT>
    ClangDiagnosticsState(DiagnosticIDsPtrT ids, ConsumerT* consumer)
#if LLVM_VERSION_MAJOR >= 21
        : options(), engine(new DiagnosticsEngineT(ids, options, consumer, true))
#else
        : options(new DiagnosticOptionsT()), engine(new DiagnosticsEngineT(ids, options, consumer, true))
#endif
    {}
};

inline std::unique_ptr<clang::CompilerInvocation> CreateClangInvocation(
    llvm::ArrayRef<const char*> args,
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics)
{
    clang::CreateInvocationOptions options;
    options.Diags = std::move(diagnostics);
    options.RecoverOnError = true;
    return clang::createInvocation(args, options);
}

inline std::unique_ptr<clang::CompilerInstance> CreateClangCompilerInstance(
    std::shared_ptr<clang::CompilerInvocation> invocation)
{
#if LLVM_VERSION_MAJOR >= 21
    return std::make_unique<clang::CompilerInstance>(std::move(invocation));
#else
    auto instance = std::make_unique<clang::CompilerInstance>();
    instance->setInvocation(std::move(invocation));
    return instance;
#endif
}

#endif

/*
 * LLVM 23 made APInt reject a value that does not fit the target bit width instead of
 * truncating it silently, so ConstantInt::get(iN, uint64) now asserts. Bitfield masks and
 * C macro constants are built at 64 bits and narrowed ON PURPOSE, so truncate explicitly.
 */
inline llvm::ConstantInt* GetIntTruncated(llvm::Type* type, uint64_t value)
{
    const unsigned bits = type->getIntegerBitWidth();
    return llvm::ConstantInt::get(type->getContext(), llvm::APInt(64, value).zextOrTrunc(bits));
}

} // namespace cflat_llvm_compat
