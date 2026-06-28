# cflat compiler - cross-platform CMake build.
#
# This replaces the MSBuild .vcxproj/.slnx. Configure via CMakePresets.json:
#   cmake --preset win-x64-release   (or win-x64-debug)
#   cmake --build --preset win-x64-release
#
# The Windows preset reuses the existing repo-root vcpkg_installed tree
# (VCPKG_MANIFEST_INSTALL=OFF) so it does NOT trigger a 26 GB / 50 min rebuild.
#
# Output mirrors the old MSBuild layout exactly so test.bat works unchanged:
#   x64/<Config>/cflat.exe + x64/<Config>/core/** + lld-link/clang-cl/*.dll.
#
# Cross-platform structure: Windows is implemented; APPLE/UNIX branches are
# stubbed where they will be filled in by later stages (see
# internal/plan/cross-platform-macos.md).

cmake_minimum_required(VERSION 3.21)

# Static CRT (/MT, /MTd) to match the x64-windows-static deps. Must be set
# before project(). Requires policy CMP0091 (CMake >= 3.15, default NEW here).
if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(cflat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CFLAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cflat)

# ---------------------------------------------------------------------------
# Dependencies (provided by vcpkg via the toolchain file in the preset).
# ---------------------------------------------------------------------------
find_package(LLVM CONFIG REQUIRED)
# CClangExtract.cpp drives the clang frontend (CompilerInstance, FrontendAction,
# PPCallbacks, Lexer) for C-interop auto-extern. The old MSBuild build relied on
# vcpkg auto-linking every installed lib; CMake names the clang libs explicitly.
find_package(Clang CONFIG REQUIRED)
find_package(antlr4-runtime CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)

message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} (targets: ${LLVM_TARGETS_TO_BUILD})")
message(STATUS "LLVM tools dir: ${LLVM_TOOLS_BINARY_DIR}")

# vcpkg ships antlr4-runtime as antlr4_static (static triplet) or antlr4_shared.
if(TARGET antlr4_static)
  set(ANTLR4_RUNTIME_TARGET antlr4_static)
  set(ANTLR4_IS_STATIC TRUE)
elseif(TARGET antlr4_shared)
  set(ANTLR4_RUNTIME_TARGET antlr4_shared)
  set(ANTLR4_IS_STATIC FALSE)
else()
  message(FATAL_ERROR "antlr4-runtime found but neither antlr4_static nor antlr4_shared target exists")
endif()

# LLVM lib selection. On Windows (vcpkg static build) link the individual
# static component archives. On Debian/Ubuntu, clang's libs are built against
# the shared libLLVM dylib; also linking the static LLVM archives would
# register LLVM's CommandLine options twice ("registered more than once").
# Prefer the single shared LLVM dylib target there. 'native'/'nativecodegen'
# resolve to the host target (X86/AArch64) so the component list stays portable.
if(NOT WIN32 AND TARGET LLVM)
  set(LLVM_LIBS LLVM)
  message(STATUS "Linking shared LLVM dylib target")
else()
  llvm_map_components_to_libnames(LLVM_LIBS
    core support irreader bitreader bitwriter asmparser
    object linker mc mcparser mcdisassembler
    target codegen analysis passes
    instrumentation scalaropts ipo instcombine transformutils
    orcjit jitlink executionengine
    native nativecodegen
    # Cross-target macOS arm64 emission: the code calls InitializeAllTargets(),
    # which references every target in the installed LLVM (now X86 + AArch64), so
    # the AArch64 target libs must be linked too - 'native' only covers the host.
    # Use the exact (case-sensitive) lib suffixes: llvm_map_components_to_libnames
    # prepends 'LLVM' verbatim, so 'aarch64' would wrongly yield 'LLVMaarch64'.
    AArch64CodeGen AArch64AsmParser AArch64Disassembler
    AArch64Desc AArch64Info AArch64Utils)
endif()

# ---------------------------------------------------------------------------
# ANTLR4 parser generation from CFlat.g4.
# Tool resolution order: -DANTLR4_JAR=<complete.jar> (java -jar, reproducible)
# else the antlr4 wrapper on PATH (pip install antlr4-tools). Generates a
# listener by default (no visitor) - matches the previous antlr4.bat command.
# ---------------------------------------------------------------------------
set(CFLAT_GRAMMAR ${CFLAT_DIR}/CFlat.g4)
set(ANTLR_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/antlr_generated)
set(ANTLR_GEN_SOURCES
  ${ANTLR_GEN_DIR}/CFlatLexer.cpp
  ${ANTLR_GEN_DIR}/CFlatParser.cpp
  ${ANTLR_GEN_DIR}/CFlatBaseListener.cpp
  ${ANTLR_GEN_DIR}/CFlatListener.cpp)
set(ANTLR_GEN_HEADERS
  ${ANTLR_GEN_DIR}/CFlatLexer.h
  ${ANTLR_GEN_DIR}/CFlatParser.h
  ${ANTLR_GEN_DIR}/CFlatBaseListener.h
  ${ANTLR_GEN_DIR}/CFlatListener.h)

set(ANTLR4_JAR "" CACHE FILEPATH "Path to an antlr-complete .jar (optional; overrides the antlr4 wrapper)")
if(ANTLR4_JAR)
  find_package(Java COMPONENTS Runtime REQUIRED)
  set(ANTLR_GEN_COMMAND "${Java_JAVA_EXECUTABLE}" -jar "${ANTLR4_JAR}"
      -Dlanguage=Cpp -o "${ANTLR_GEN_DIR}" "${CFLAT_GRAMMAR}")
else()
  find_program(ANTLR4_TOOL NAMES antlr4 antlr4.bat
    DOC "antlr4 wrapper (pip install antlr4-tools) - or set -DANTLR4_JAR")
  if(NOT ANTLR4_TOOL)
    message(FATAL_ERROR
      "ANTLR4 tool not found. Either 'pip install antlr4-tools' so 'antlr4' is on PATH, "
      "or pass -DANTLR4_JAR=/path/to/antlr-complete.jar")
  endif()
  set(ANTLR_GEN_COMMAND "${ANTLR4_TOOL}" -Dlanguage=Cpp -o "${ANTLR_GEN_DIR}" "${CFLAT_GRAMMAR}")
endif()

add_custom_command(
  OUTPUT ${ANTLR_GEN_SOURCES} ${ANTLR_GEN_HEADERS}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ANTLR_GEN_DIR}"
  COMMAND ${ANTLR_GEN_COMMAND}
  DEPENDS ${CFLAT_GRAMMAR}
  COMMENT "Generating ANTLR4 parser from CFlat.g4"
  VERBATIM)

# ---------------------------------------------------------------------------
# Compiler target.
# ---------------------------------------------------------------------------
set(CFLAT_SOURCES
  ${CFLAT_DIR}/main.cpp
  ${CFLAT_DIR}/LLVMBackend.cpp
  ${CFLAT_DIR}/CClangExtract.cpp
  ${CFLAT_DIR}/JsonRpcLoop.cpp
  ${CFLAT_DIR}/LspServer.cpp
  ${CFLAT_DIR}/LspSymbolIndex.cpp)

# WinMD import is a Windows-only feature (reads .winmd via windows.h). It will
# be compiled out / stubbed on other platforms in Stage 2.
if(WIN32)
  set(WINMD_SOURCES
    ${CFLAT_DIR}/WinmdExtract.cpp
    ${CFLAT_DIR}/WinmdEmit.cpp
    ${CFLAT_DIR}/WinmdSignature.cpp)
else()
  # Stubs so the host links on non-Windows; WinMD paths fail cleanly at runtime.
  set(WINMD_SOURCES ${CFLAT_DIR}/WinmdStubs.cpp)
endif()
list(APPEND CFLAT_SOURCES ${WINMD_SOURCES})

add_executable(cflat ${CFLAT_SOURCES} ${ANTLR_GEN_SOURCES})

# The generated parser includes <antlr4-runtime.h>. vcpkg's antlr4 target
# propagates its include dir; Debian's does not - locate it explicitly so both
# work. (Header lives in <prefix>/include/antlr4-runtime.)
find_path(ANTLR4_RUNTIME_INCLUDE_DIR antlr4-runtime.h PATH_SUFFIXES antlr4-runtime REQUIRED)

target_include_directories(cflat PRIVATE
  ${CFLAT_DIR}
  ${ANTLR_GEN_DIR}
  ${ANTLR4_RUNTIME_INCLUDE_DIR}
  ${LLVM_INCLUDE_DIRS})

target_compile_definitions(cflat PRIVATE ${LLVM_DEFINITIONS})
if(ANTLR4_IS_STATIC)
  target_compile_definitions(cflat PRIVATE ANTLR4CPP_STATIC)
endif()
if(WIN32)
  target_compile_definitions(cflat PRIVATE _CONSOLE)
endif()

# Precompiled header of the heavy third-party headers (LLVM/antlr/json/simdjson).
# The generated antlr sources declare an EOF() accessor that must see the EOF
# macro UNDEFINED, and the WinMD sources pull in <windows.h>; both are excluded
# from the PCH (which leaves EOF defined).
target_precompile_headers(cflat PRIVATE ${CFLAT_DIR}/pch.h)
set_source_files_properties(${ANTLR_GEN_SOURCES} ${WINMD_SOURCES}
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Clang frontend libs (imported targets carry their own transitive deps).
set(CLANG_LIBS
  clangFrontend clangFrontendTool clangTooling clangToolingCore
  clangParse clangSema clangAnalysis clangEdit clangAST clangASTMatchers
  clangLex clangDriver clangSerialization clangCodeGen clangRewrite
  clangIndex clangBasic clangSupport)

target_link_libraries(cflat PRIVATE
  ${CLANG_LIBS}
  ${LLVM_LIBS}
  ${ANTLR4_RUNTIME_TARGET}
  nlohmann_json::nlohmann_json
  simdjson::simdjson)

# bigobj: these TUs (and the generated parser) exceed the COFF section limit.
if(MSVC)
  set_source_files_properties(
    ${CFLAT_DIR}/LLVMBackend.cpp
    ${CFLAT_DIR}/CClangExtract.cpp
    ${ANTLR_GEN_DIR}/CFlatParser.cpp
    PROPERTIES COMPILE_OPTIONS "/bigobj")
  # /permissive- matched ConformanceMode=true; /wd4251 quiets antlr's dll-iface
  # warnings on the generated sources.
  target_compile_options(cflat PRIVATE /permissive- /MP)
  set_source_files_properties(${ANTLR_GEN_SOURCES}
    PROPERTIES COMPILE_OPTIONS "/wd4251")
endif()

# ---------------------------------------------------------------------------
# Platform link libraries.
# ---------------------------------------------------------------------------
if(WIN32)
  # ws2_32/version per the old .vcxproj; advapi32 (registry SDK detection),
  # ole32 (WinMD/COM) are default MSVC libs, named here for clarity.
  target_link_libraries(cflat PRIVATE ws2_32 version advapi32 ole32)
  # 64 MB stack: the recursive-descent codegen walk has very large frames.
  target_link_options(cflat PRIVATE /STACK:67108864 /ignore:4099)
elseif(UNIX)
  # Linux/macOS host: LLVM needs the dynamic linker, threads, zlib, etc.
  # find_package(Threads) + LLVM's interface libs cover most; filled in Stage 2.
  find_package(Threads REQUIRED)
  target_link_libraries(cflat PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Output location: x64/<Config>/cflat(.exe) to match test.bat expectations.
# ---------------------------------------------------------------------------
set_target_properties(cflat PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>)

# ---------------------------------------------------------------------------
# Post-build deployment: core library tree + linker tools next to the exe.
# ---------------------------------------------------------------------------
# Deploy core/ via a stamp that depends on every core file, NOT a POST_BUILD on
# cflat. A POST_BUILD only fires when the exe relinks, so editing only a core
# *.cb (no C++ change) would leave the deployed copy - which the exe actually
# reads - stale. The stamp re-runs the copy whenever any core file is newer.
# CONFIGURE_DEPENDS re-globs at build time so new core files are picked up
# without a manual reconfigure.
file(GLOB_RECURSE CORE_FILES CONFIGURE_DEPENDS "${CFLAT_DIR}/core/*")
set(_core_stamp "${CMAKE_CURRENT_BINARY_DIR}/deploy_core.stamp")
add_custom_command(
  OUTPUT ${_core_stamp}
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CFLAT_DIR}/core" "$<TARGET_FILE_DIR:cflat>/core"
  COMMAND ${CMAKE_COMMAND} -E touch ${_core_stamp}
  DEPENDS ${CORE_FILES} cflat
  COMMENT "Deploying core/ library tree next to cflat")
add_custom_target(deploy_core ALL DEPENDS ${_core_stamp})

if(WIN32)
  # lld-link + clang-cl (the C-interop / native link drivers) and the LLVM
  # plugin DLLs that the existing build copies next to the exe.
  get_filename_component(_llvm_triplet_root "${LLVM_TOOLS_BINARY_DIR}/../.." ABSOLUTE)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_llvm_bin_dir "${_llvm_triplet_root}/debug/bin")
  else()
    set(_llvm_bin_dir "${_llvm_triplet_root}/bin")
  endif()

  set(_deploy_tools
    "${LLVM_TOOLS_BINARY_DIR}/lld-link.exe"
    "${LLVM_TOOLS_BINARY_DIR}/clang-cl.exe")
  foreach(_dll LTO.dll Remarks.dll)
    if(EXISTS "${_llvm_bin_dir}/${_dll}")
      list(APPEND _deploy_tools "${_llvm_bin_dir}/${_dll}")
    endif()
  endforeach()

  add_custom_command(TARGET cflat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_deploy_tools} "$<TARGET_FILE_DIR:cflat>"
    COMMENT "Deploying lld-link/clang-cl + LLVM plugin DLLs")
endif()
