// WinRT parameterized-type signatures and derived IIDs (PIID).
//
// A parameterized WinRT interface (IVector<T>, IMap<K,V>, IReference<T>, ...) has no IID stored
// in metadata for any concrete instantiation. Every projection (cppwinrt, windows-rs, C#/WinRT,
// and now CFlat) must SYNTHESIZE the instance IID identically, or consumers reject the object.
// The IID is an RFC 4122 version-5 (SHA-1) UUID computed over a canonical signature string built
// recursively from the generic interface's seed GUID and each type argument's own signature.
//
// This header is deliberately free of Windows/LLVM headers (like WinmdModel.h) so it can be
// included by LLVMBackend.h. All Win32 (BCrypt SHA-1) usage lives in WinmdSignature.cpp.
//
// Reference: "The Windows Runtime (WinRT) type system" (learn.microsoft.com), section
// "Guid generation for parameterized types".
#pragma once

#include "WinmdModel.h"
#include <string>
#include <cstdint>

namespace cflat_winmd
{
    // The fixed WinRT namespace GUID used as the RFC 4122 namespace for every PIID derivation.
    // (Spelled out for documentation; the .cpp parses it.)
    constexpr const char* kWrtPinterfaceNamespace = "11f47ad5-7b73-42c0-abae-878b1e16adee";

    // Encode a type as its WinRT signature octet string (the "name" half of the v5 hash input).
    // Fundamentals map to fixed codes ("i4", "string", "g16", ...); named types resolve through
    // `model` (interface -> "{iid}", enum -> "enum(...)", struct -> "struct(...)", runtimeclass
    // -> "rc(...)"); a parameterized instance (genericArgs non-empty) recurses into
    // "pinterface({seed};arg1;arg2;...)". Returns "" and sets `err` if a type cannot be encoded.
    std::string EncodeSig(const TypeRef& t, const Model& model, std::string& err);

    // Generic RFC 4122 version-5 (SHA-1) UUID. `nsImage` and `outImage` are 16-byte GUID images
    // in the in-memory little-endian-Data1/2/3 convention used throughout the projection (matching
    // WinmdExtract's FormatGuid / WinmdEmit's ParseGuid). Returns false only if SHA-1 fails.
    bool DeriveV5Guid(const uint8_t nsImage[16], const std::string& name, uint8_t outImage[16]);

    // Derive the IID of a concrete parameterized instance (e.g. IVector<Int32>). `instance` is a
    // TypeRef whose fullName names the generic interface and whose genericArgs hold the concrete
    // args. Returns false and sets `err` if the signature cannot be built.
    bool DerivePiid(const TypeRef& instance, const Model& model, uint8_t outImage[16], std::string& err);

    // Format a 16-byte GUID image as canonical lowercase dashed text (no braces).
    std::string FormatGuidImage(const uint8_t image[16]);

    // Self-check: validates the v5 machinery against the canonical RFC 4122 DNS test vector and
    // the PIID derivation against published reference IIDs (IVector<Int32>, IReference<Int32>,
    // IMap<String,Object>, and a recursive IIterable<IKeyValuePair<String,Object>>). Returns true
    // on full agreement; otherwise returns false and fills `report` with the mismatches. `report`
    // always receives a human-readable summary (used by the --winmd-sig-selftest CLI flag).
    bool WinmdSignatureSelfTest(std::string& report);
}
