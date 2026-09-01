#pragma once
// LlvmHelpers.h - small wrappers over LLVM spellings that are easy to get wrong.
//
// These are NOT version shims - cflat targets LLVM 23 only. They live in their own header
// because LLVMBackend.h and MoveDataflow.h both need them, and MoveDataflow.h is itself
// included by LLVMBackend.h: defining them inside LLVMBackend.h forced the namespace to sit
// wedged between two groups of #includes, above whichever consumer came next.

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>

namespace cflat_llvm
{

// BasicBlock::getTerminator() asserts on a block that has none; this is the nullable
// query, and it also tolerates a null block (IRBuilder::GetInsertBlock() can return one).
inline llvm::Instruction* GetTerminatorOrNull(llvm::BasicBlock* block)
{
    return block == nullptr ? nullptr : block->getTerminatorOrNull();
}

inline const llvm::Instruction* GetTerminatorOrNull(const llvm::BasicBlock* block)
{
    return block == nullptr ? nullptr : block->getTerminatorOrNull();
}

// APInt rejects a value that does not fit the target bit width instead of truncating it
// silently, so ConstantInt::get(iN, uint64) asserts. Bitfield masks and C macro constants
// are built at 64 bits and narrowed ON PURPOSE, so truncate explicitly.
inline llvm::ConstantInt* GetIntTruncated(llvm::Type* type, uint64_t value)
{
    const unsigned bits = type->getIntegerBitWidth();
    return llvm::ConstantInt::get(type->getContext(), llvm::APInt(64, value).zextOrTrunc(bits));
}

// Opaque pointers make a pointer type independent of its pointee: every 'T*' lowers to the
// same 'ptr' in a given address space, which is why LLVM 23 deprecated Type::getPointerTo().
// PointerType::get(Type*, unsigned) is deprecated too - only the context overload survives.
// This keeps the readable "pointer to this type" spelling at 150-odd call sites while
// routing to the supported API; the argument is used solely to reach its LLVMContext.
inline llvm::PointerType* PointerTo(llvm::Type* type, unsigned addressSpace = 0)
{
    return llvm::PointerType::get(type->getContext(), addressSpace);
}

} // namespace cflat_llvm
