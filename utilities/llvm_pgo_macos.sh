#!/bin/bash
# PGO rebuild of the pinned LLVM 23.1.0 (macOS arm64, host = Apple clang, same recipe as
# internal/llvm-from-source-build.md). Stages: instrumented clang -> training on cflat's own clang
# invocations (torch/simdjson/fmt/eigen/json headers + interop fixtures) -> merge -> final build
# -> install to ~/.cflat-compiler-deps/llvm-23.1.0-pgo (the macOS Release preset links it; Debug stays on
# the -assert tree). Needs the LLVM source at $SRC and Homebrew pytorch/simdjson/fmt/nlohmann-json/eigen
# for the training headers. Usage: utilities/llvm_pgo_macos.sh [all|instr|train|final]
set -u
SRC=$HOME/llvm-src/llvm-project-23.1.0.src/llvm
INSTR=$HOME/llvm-src/build-23-instr
FINAL=$HOME/llvm-src/build-23-pgo
PROF=$HOME/llvm-src/clang23.profdata
INSTALL=$HOME/.cflat-compiler-deps/llvm-23.1.0-pgo
REPO="$(cd "$(dirname "$0")/.." && pwd)"
COMMON="-DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_PARALLEL_LINK_JOBS=4"
stamp() { echo "[$(date +%H:%M:%S)] $*"; }
stage=${1:-all}

if [ "$stage" = all ] || [ "$stage" = instr ]; then
  stamp "stage 1: instrumented build (clang only, AArch64 only)"
  cmake -G Ninja -S "$SRC" -B "$INSTR" $COMMON -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_BUILD_INSTRUMENTED=IR -DLLVM_BUILD_RUNTIME=OFF -DCMAKE_INSTALL_PREFIX=$HOME/llvm-src/instr-install >"$INSTR.cfg.log" 2>&1 || { stamp "instr configure FAILED"; tail -20 "$INSTR.cfg.log"; exit 1; }
  cmake --build "$INSTR" --target clang >"$INSTR.build.log" 2>&1 || { stamp "instr build FAILED"; tail -30 "$INSTR.build.log"; exit 1; }
  stamp "instrumented clang built"
fi

if [ "$stage" = all ] || [ "$stage" = train ]; then
  stamp "stage 2: training"
  CXX=$INSTR/bin/clang++
  rm -rf "$INSTR/profiles"; mkdir -p "$INSTR/profiles" "$REPO/scratch/pgo_train"
  SDK=$(xcrun --show-sdk-path); RES=$($CXX -print-resource-dir)
  T=$(brew --prefix pytorch)
  # cflat's exact clang invocation shape (from the request-cache key), torch include dirs
  CF="--target=arm64-apple-macosx11.0.0 -isysroot $SDK -resource-dir $RES -x c++ -std=c++20 -ferror-limit=0 -Wno-everything -fno-spell-checking -mcpu=apple-a14 -O1 -Xclang -disable-llvm-passes"
  INC="-I$T/include -I$T/include/torch/csrc/api/include -I$(brew --prefix simdjson)/include -I$(brew --prefix fmt)/include -I$(brew --prefix nlohmann-json)/include -I$(brew --prefix eigen)/include/eigen3"
  cd "$REPO/scratch/pgo_train"
  printf '#include <torch/torch.h>\n' > torch.cpp
  printf '#include <simdjson.h>\n' > simdjson.cpp
  printf '#include <fmt/format.h>\n#include <fmt/ranges.h>\n' > fmt.cpp
  printf '#include <nlohmann/json.hpp>\n' > json.cpp
  printf '#include <Eigen/Dense>\n' > eigen.cpp
  printf '#include <vector>\n#include <string>\n#include <map>\n#include <memory>\n#include <algorithm>\n#include <functional>\n' > std.cpp
  for h in $(find "$REPO/Test" -name '*.h' | head -40); do printf '#include "%s"\n' "$h" > "fx_$(basename "$h" .h).cpp"; done
  n=0
  for f in *.cpp "$REPO/test_libs/torch/torch_04_training.cpp"; do
    for mode in "-fsyntax-only" "-c -o /dev/null"; do
      LLVM_PROFILE_FILE="$INSTR/profiles/%m-%p.profraw" $CXX $CF $INC $mode "$f" >/dev/null 2>&1 && n=$((n+1)) || stamp "  training compile failed (ignored): $f $mode"
    done
  done
  # also the plain driver shape clang++ users see (Apple-like defaults)
  LLVM_PROFILE_FILE="$INSTR/profiles/%m-%p.profraw" $CXX -std=c++20 -O0 -isysroot $SDK $INC -c "$REPO/test_libs/torch/torch_04_training.cpp" -o /dev/null >/dev/null 2>&1 && n=$((n+1))
  LLVM_PROFILE_FILE="$INSTR/profiles/%m-%p.profraw" $CXX -std=c++20 -O2 -isysroot $SDK $INC -c "$REPO/test_libs/torch/torch_04_training.cpp" -o /dev/null >/dev/null 2>&1 && n=$((n+1))
  stamp "training compiles ok: $n; raw profiles: $(ls "$INSTR/profiles" | wc -l)"
  [ "$(ls "$INSTR/profiles" | wc -l)" -gt 0 ] || { stamp "no profiles - abort"; exit 1; }
  xcrun llvm-profdata merge -output="$PROF" "$INSTR"/profiles/*.profraw || { stamp "merge FAILED"; exit 1; }
  stamp "merged: $(ls -la "$PROF" | awk '{print $5}') bytes"
  cd "$REPO"
fi

if [ "$stage" = all ] || [ "$stage" = final ]; then
  stamp "stage 3: final PGO build (clang;lld, X86;AArch64)"
  cmake -G Ninja -S "$SRC" -B "$FINAL" $COMMON -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
    -DLLVM_PROFDATA_FILE="$PROF" -DCMAKE_INSTALL_PREFIX="$INSTALL" >"$FINAL.cfg.log" 2>&1 || { stamp "final configure FAILED"; tail -20 "$FINAL.cfg.log"; exit 1; }
  cmake --build "$FINAL" >"$FINAL.build.log" 2>&1 || { stamp "final build FAILED"; grep -m5 -E 'error:|FAILED' "$FINAL.build.log"; exit 1; }
  cmake --install "$FINAL" >"$FINAL.install.log" 2>&1 || { stamp "install FAILED"; exit 1; }
  stamp "installed to $INSTALL"
fi
stamp "done"
