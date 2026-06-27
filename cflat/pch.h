#pragma once
// Precompiled header: the heavy, stable third-party headers, parsed once and
// reused across translation units (LLVM dominates compile time here).
//
// Applied to the target via target_precompile_headers() in CMakeLists.txt.
// IMPORTANT: the generated antlr parser sources (CFlatParser.cpp etc.) declare
// an EOF() accessor and must compile with the EOF macro UNDEFINED; this PCH
// leaves EOF defined (nlohmann_json and MSVC's <fstream> need it). Those
// generated sources - and the Windows-only WinMD sources - are excluded from
// the PCH via SKIP_PRECOMPILE_HEADERS in CMakeLists.txt.
//
// clang's frontend headers are intentionally NOT here: only CClangExtract.cpp
// uses them, so they stay local to that one TU rather than bloating the shared
// PCH.

// --- C++ standard library ---
#include <cstdio>     // pull EOF in before <fstream> so basic_filebuf parses on MSVC
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <variant>
#include <functional>
#include <algorithm>
#include <ranges>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <format>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>

// --- LLVM ---
#include <llvm/ADT/StringSet.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/JITLink/x86_64.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/CrashRecoveryContext.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

// --- antlr4 runtime (EOF intact: it pulls <fstream> which needs the macro) ---
#include <antlr4-runtime.h>

// --- simdjson ---
#include <simdjson.h>

// --- nlohmann json (EOF-macro-safe wrapper; leaves EOF defined) ---
#include "platform/JsonCompat.h"
