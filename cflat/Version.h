#pragma once

// Single source of truth for the cflat compiler version.
// The VS Code extension version is stamped from these by
// vscode-extension/sync-version.js (run by build.bat / install.bat).
//
// Keep these as plain decimal integers - do NOT write a leading zero
// (e.g. 01), which C++ parses as an octal literal.
#define CFLAT_VERSION_MAJOR 0
#define CFLAT_VERSION_MINOR 7

// Monotonic integer form for ordered comparisons (CFLAT_VERSION_NUMBER < X, ==, etc.).
// Do NOT compare CFLAT_VERSION_STRING - it sorts lexicographically ("0.10" < "0.5").
// Minor is capped at 99; bump the multiplier if a release ever needs more.
#define CFLAT_VERSION_NUMBER (CFLAT_VERSION_MAJOR * 100 + CFLAT_VERSION_MINOR)

#define CFLAT_VERSION_STRINGIZE_(x) #x
#define CFLAT_VERSION_STRINGIZE(x) CFLAT_VERSION_STRINGIZE_(x)
#define CFLAT_VERSION_STRING \
    CFLAT_VERSION_STRINGIZE(CFLAT_VERSION_MAJOR) "." CFLAT_VERSION_STRINGIZE(CFLAT_VERSION_MINOR)

const int MAJOR_VERSION = CFLAT_VERSION_MAJOR;
const int MINOR_VERSION = CFLAT_VERSION_MINOR;
