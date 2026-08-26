#!/usr/bin/env bash
# Deploy ld64.lld next to cflat. A shared-libLLVM provider (Homebrew) builds
# ld64.lld against @rpath/libLLVM.dylib with only @loader_path/../lib on its
# rpath, so the copy cannot find it; add the provider's lib dir. No re-sign
# needed: the adhoc linker signature asserts no identity, so install_name_tool
# regenerates it. A static provider skips this branch entirely.
# Usage: deploy_macho_linker.sh <src-ld64.lld> <dest-dir> <llvm-lib-dir>
set -eu
src="$1"; dest_dir="$2"; llvm_lib="$3"
dest="$dest_dir/ld64.lld"
cp -f "$src" "$dest"
# Homebrew's lld is split from llvm and its driver loads the lld component dylibs
# through @rpath. Keep those alongside the deployed driver so test and example
# invocations do not depend on the user's Homebrew environment.
lld_lib_dir="$(cd "$(dirname "$src")/../lib" && pwd)"
runtime_lib_dir="$dest_dir/../lib"
if [ -d "$lld_lib_dir" ]; then
  mkdir -p "$runtime_lib_dir"
  for lib in "$lld_lib_dir"/liblld*.dylib; do
    [ -e "$lib" ] || continue
    cp -f "$lib" "$runtime_lib_dir/"
  done
fi
if otool -L "$dest" | grep -q '@rpath/libLLVM'; then
  otool -l "$dest" | grep -A2 LC_RPATH | grep -q "path $llvm_lib\$" \
    || install_name_tool -add_rpath "$llvm_lib" "$dest"
fi
