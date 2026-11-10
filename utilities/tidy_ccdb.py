#!/usr/bin/env python3
"""Write a clang-tidy copy of a CMake compile database with precompiled-header flags removed.

clang-tidy refuses a .pch built by another compiler (MSVC /Yu /Fp, or a clang of another version),
so the database CMake exports cannot be used as is. The forced include of cmake_pch.hxx (/FI, or
-include) stays: it is a plain header that pulls in cflat/pch.h, so each TU still sees its includes.

usage: tidy_ccdb.py BUILD_DIR      reads BUILD_DIR/compile_commands.json,
                                   writes BUILD_DIR/tidy/compile_commands.json, prints that dir
Then: clang-tidy -p BUILD_DIR/tidy <file.cpp>  (or run-clang-tidy.py / clang-tidy-diff.py -p ...)
"""
import json
import os
import shlex
import sys


def strip_pch(args):
    out = []
    i = 0
    while i < len(args):
        arg = args[i]
        # MSVC: /Yu<hdr> /Yc<hdr> /Fp<file>.pch (also the -Yu / -Fp spellings)
        if arg[:3] in ("/Yu", "-Yu", "/Yc", "-Yc", "/Fp", "-Fp"):
            i += 1
            continue
        # clang: -Xclang -include-pch -Xclang <file>.pch, and -include-pch <file>
        if arg == "-Xclang" and i + 3 < len(args) and args[i + 1] == "-include-pch":
            i += 4
            continue
        if arg == "-include-pch":
            i += 2
            continue
        out.append(arg)
        i += 1
    return out


def split_command(command):
    # Ninja on Windows writes an unquoted-backslash cl.exe command line; posix shlex would eat the
    # backslashes, so split it the Windows way there.
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        argc = ctypes.c_int()
        fn = ctypes.windll.shell32.CommandLineToArgvW
        fn.restype = ctypes.POINTER(wintypes.LPWSTR)
        ptr = fn(command, ctypes.byref(argc))
        try:
            return [ptr[k] for k in range(argc.value)]
        finally:
            ctypes.windll.kernel32.LocalFree(ptr)
    return shlex.split(command)


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    build_dir = sys.argv[1]
    src = os.path.join(build_dir, "compile_commands.json")
    with open(src, encoding="utf-8") as fh:
        entries = json.load(fh)
    for entry in entries:
        if "arguments" in entry:
            entry["arguments"] = strip_pch(entry["arguments"])
        else:
            entry["arguments"] = strip_pch(split_command(entry.pop("command")))
    out_dir = os.path.join(build_dir, "tidy")
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "compile_commands.json"), "w", encoding="utf-8") as fh:
        json.dump(entries, fh, indent=1)
    print(out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
