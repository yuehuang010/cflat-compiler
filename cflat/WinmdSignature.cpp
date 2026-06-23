// See WinmdSignature.h. RFC 4122 v5 PIID derivation + the WinRT signature encoder.
#include "WinmdSignature.h"

#include <windows.h>
#include <bcrypt.h>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

namespace cflat_winmd
{
    // --- GUID image <-> text (matches WinmdExtract::FormatGuid / WinmdEmit::ParseGuid) ----------
    // The in-memory image is Data1 (4 bytes LE), Data2 (2 LE), Data3 (2 LE), Data4 (8 raw).
    static bool ParseGuidText(const std::string& text, uint8_t out[16])
    {
        uint8_t raw[16];
        int n = 0;
        for (char c : text)
        {
            if (c == '-' || c == '{' || c == '}') continue;
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return false;
            if (n >= 32) return false;
            if ((n & 1) == 0) raw[n / 2] = (uint8_t)(v << 4); else raw[n / 2] |= (uint8_t)v;
            n++;
        }
        if (n != 32) return false;
        out[0] = raw[3]; out[1] = raw[2]; out[2] = raw[1]; out[3] = raw[0];   // Data1 LE
        out[4] = raw[5]; out[5] = raw[4];                                     // Data2 LE
        out[6] = raw[7]; out[7] = raw[6];                                     // Data3 LE
        for (int i = 8; i < 16; i++) out[i] = raw[i];                         // Data4 raw
        return true;
    }

    std::string FormatGuidImage(const uint8_t g[16])
    {
        unsigned d1 = g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24);
        unsigned d2 = g[4] | (g[5] << 8);
        unsigned d3 = g[6] | (g[7] << 8);
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
        return buf;
    }

    // The in-memory image stores Data1/2/3 little-endian; RFC 4122 hashes them big-endian
    // (network order). These two helpers convert between the image and the network-order bytes.
    static void ImageToNetwork(const uint8_t img[16], uint8_t be[16])
    {
        be[0] = img[3]; be[1] = img[2]; be[2] = img[1]; be[3] = img[0];   // Data1
        be[4] = img[5]; be[5] = img[4];                                   // Data2
        be[6] = img[7]; be[7] = img[6];                                   // Data3
        for (int i = 8; i < 16; i++) be[i] = img[i];                      // Data4
    }

    static void NetworkToImage(const uint8_t be[16], uint8_t img[16])
    {
        img[0] = be[3]; img[1] = be[2]; img[2] = be[1]; img[3] = be[0];
        img[4] = be[5]; img[5] = be[4];
        img[6] = be[7]; img[7] = be[6];
        for (int i = 8; i < 16; i++) img[i] = be[i];
    }

    static bool Sha1(const uint8_t* data, size_t len, uint8_t digest[20])
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
            return false;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0
            && BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0) == 0
            && BCryptFinishHash(hash, digest, 20, 0) == 0;
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    bool DeriveV5Guid(const uint8_t nsImage[16], const std::string& name, uint8_t outImage[16])
    {
        // hash input = network-order namespace bytes ++ utf8(name)
        std::string buf;
        uint8_t nsBe[16];
        ImageToNetwork(nsImage, nsBe);
        buf.append((const char*)nsBe, 16);
        buf.append(name);

        uint8_t digest[20];
        if (!Sha1((const uint8_t*)buf.data(), buf.size(), digest))
            return false;

        // First 16 bytes are the UUID in network order; stamp version 5 and the RFC variant.
        uint8_t be[16];
        for (int i = 0; i < 16; i++) be[i] = digest[i];
        be[6] = (uint8_t)((be[6] & 0x0F) | 0x50);   // version 5
        be[8] = (uint8_t)((be[8] & 0x3F) | 0x80);   // RFC 4122 variant
        NetworkToImage(be, outImage);
        return true;
    }

    // --- Signature encoder ----------------------------------------------------------------------
    static std::string FundamentalSig(const std::string& winrtName)
    {
        if (winrtName == "Boolean") return "b1";
        if (winrtName == "UInt8")   return "u1";
        if (winrtName == "Int16")   return "i2";
        if (winrtName == "UInt16")  return "u2";
        if (winrtName == "Int32")   return "i4";
        if (winrtName == "UInt32")  return "u4";
        if (winrtName == "Int64")   return "i8";
        if (winrtName == "UInt64")  return "u8";
        if (winrtName == "Single")  return "f4";
        if (winrtName == "Double")  return "f8";
        if (winrtName == "Char16")  return "c2";
        if (winrtName == "String")  return "string";
        if (winrtName == "Guid")    return "g16";
        if (winrtName == "Object")  return "cinterface(IInspectable)";
        return "";
    }

    static const Delegate* FindDelegate(const Model& model, const std::string& fullName)
    {
        for (const auto& d : model.delegates) if (d.fullName == fullName) return &d;
        return nullptr;
    }

    static const Enum* FindEnum(const Model& model, const std::string& fullName)
    {
        for (const auto& e : model.enums) if (e.fullName == fullName) return &e;
        return nullptr;
    }

    static const Struct* FindStruct(const Model& model, const std::string& fullName)
    {
        for (const auto& s : model.structs) if (s.fullName == fullName) return &s;
        return nullptr;
    }

    static const RuntimeClass* FindRuntimeClass(const Model& model, const std::string& fullName)
    {
        for (const auto& rc : model.runtimeClasses) if (rc.fullName == fullName) return &rc;
        return nullptr;
    }

    // A guid in a signature is the lowercase dashed form wrapped in braces.
    static std::string Braced(const std::string& iid)
    {
        return "{" + iid + "}";
    }

    std::string EncodeSig(const TypeRef& t, const Model& model, std::string& err)
    {
        if (!err.empty()) return "";

        // Arrays may not appear in a parameterized argument list (WinRT type-system rule).
        if (t.isArray)
        {
            err = "array types cannot appear in a parameterized type argument list";
            return "";
        }

        // Parameterized instance: pinterface({seed};arg1;arg2;...). Both p-interfaces and
        // p-delegates use the "pinterface(" prefix (per the WinRT grammar).
        if (!t.genericArgs.empty())
        {
            std::string seed;
            if (const Interface* gi = model.FindInterface(t.fullName)) seed = gi->iid;
            else if (const Delegate* gd = FindDelegate(model, t.fullName)) seed = gd->iid;
            if (seed.empty())
            {
                err = "no seed GUID for parameterized type '" + t.fullName + "'";
                return "";
            }
            std::string s = "pinterface(" + Braced(seed);
            for (const auto& arg : t.genericArgs)
            {
                s += ";";
                s += EncodeSig(arg, model, err);
                if (!err.empty()) return "";
            }
            s += ")";
            return s;
        }

        // Fundamentals.
        std::string fund = FundamentalSig(t.fullName);
        if (!fund.empty()) return fund;

        // Named types.
        if (const Interface* iface = model.FindInterface(t.fullName))
        {
            if (iface->iid.empty()) { err = "interface '" + t.fullName + "' has no IID"; return ""; }
            return Braced(iface->iid);
        }
        if (const Delegate* del = FindDelegate(model, t.fullName))
        {
            if (del->iid.empty()) { err = "delegate '" + t.fullName + "' has no IID"; return ""; }
            return "delegate(" + Braced(del->iid) + ")";
        }
        if (const Enum* en = FindEnum(model, t.fullName))
        {
            TypeRef underlying;
            underlying.fullName = en->underlying;   // "Int32" | "UInt32"
            return "enum(" + en->fullName + ";" + EncodeSig(underlying, model, err) + ")";
        }
        if (const Struct* st = FindStruct(model, t.fullName))
        {
            std::string s = "struct(" + st->fullName;
            for (const auto& f : st->fields)
            {
                s += ";";
                s += EncodeSig(f.type, model, err);
                if (!err.empty()) return "";
            }
            s += ")";
            return s;
        }
        if (const RuntimeClass* rc = FindRuntimeClass(model, t.fullName))
        {
            if (rc->defaultInterface.empty())
            {
                err = "runtime class '" + t.fullName + "' has no default interface";
                return "";
            }
            TypeRef def;
            def.fullName = rc->defaultInterface;
            return "rc(" + rc->fullName + ";" + EncodeSig(def, model, err) + ")";
        }

        err = "cannot encode signature for type '" + t.fullName + "'";
        return "";
    }

    bool DerivePiid(const TypeRef& instance, const Model& model, uint8_t outImage[16], std::string& err)
    {
        std::string sig = EncodeSig(instance, model, err);
        if (!err.empty()) return false;
        uint8_t ns[16];
        if (!ParseGuidText(kWrtPinterfaceNamespace, ns))
        {
            err = "bad WinRT namespace GUID constant";
            return false;
        }
        if (!DeriveV5Guid(ns, sig, outImage))
        {
            err = "SHA-1 failed";
            return false;
        }
        return true;
    }

    // --- Self-test ------------------------------------------------------------------------------
    // Seed GUIDs (the GuidAttribute on each generic interface in Windows.Foundation*.winmd).
    namespace
    {
        struct Seed { const char* name; const char* guid; };
        constexpr Seed kIVector       = { "Windows.Foundation.Collections.IVector",       "913337e9-11a1-4345-a3a2-4e7f956e222d" };
        constexpr Seed kIReference    = { "Windows.Foundation.IReference",                "61c17706-2d65-11e0-9ae8-d48564015472" };
        constexpr Seed kIIterable     = { "Windows.Foundation.Collections.IIterable",     "faa585ea-6214-4217-afda-7f46de5869b3" };
        constexpr Seed kIKeyValuePair = { "Windows.Foundation.Collections.IKeyValuePair", "02b51929-c1c4-4a7e-8940-0312b5c18500" };
        constexpr Seed kIMap          = { "Windows.Foundation.Collections.IMap",          "3c2925fe-8519-45c1-aa79-197b6718c1c1" };

        // Register a generic interface template carrying only its full name + seed GUID (all the
        // encoder needs; arity/methods are irrelevant to the signature string).
        void AddGeneric(Model& m, const Seed& s, int arity)
        {
            Interface iface;
            iface.fullName = s.name;
            iface.iid = s.guid;
            for (int i = 0; i < arity; i++) iface.genericParams.push_back("T" + std::to_string(i));
            m.interfaces.push_back(iface);
        }

        TypeRef Fund(const char* winrtName)
        {
            TypeRef t; t.fullName = winrtName; return t;
        }

        TypeRef Inst(const char* fullName, std::vector<TypeRef> args)
        {
            TypeRef t; t.fullName = fullName; t.genericArgs = std::move(args); return t;
        }
    }

    bool WinmdSignatureSelfTest(std::string& report)
    {
        bool allOk = true;
        report.clear();

        auto line = [&](const std::string& s) { report += s; report += "\n"; };

        // 1. RFC 4122 v5 machinery: the canonical DNS test vector validates SHA-1, the big-endian
        //    namespace serialization, the version/variant nibbles, and the output byte order.
        {
            uint8_t dnsNs[16], out[16];
            ParseGuidText("6ba7b810-9dad-11d1-80b4-00c04fd430c8", dnsNs);   // RFC 4122 DNS namespace
            DeriveV5Guid(dnsNs, "www.example.com", out);
            std::string got = FormatGuidImage(out);
            const char* want = "2ed6657d-e927-568b-95e1-2665a8aea6a2";
            bool ok = got == want;
            allOk = allOk && ok;
            line(std::string(ok ? "[ ok ] " : "[FAIL] ") + "RFC4122 v5 DNS vector: got " + got +
                 (ok ? "" : std::string(" want ") + want));
        }

        // 2/3. The signature strings and derived PIIDs against published reference IIDs.
        Model m;
        AddGeneric(m, kIVector, 1);
        AddGeneric(m, kIReference, 1);
        AddGeneric(m, kIIterable, 1);
        AddGeneric(m, kIKeyValuePair, 2);
        AddGeneric(m, kIMap, 2);

        struct Case { const char* label; TypeRef type; const char* sig; const char* iid; };
        std::vector<Case> cases = {
            { "IVector<Int32>",
              Inst(kIVector.name, { Fund("Int32") }),
              "pinterface({913337e9-11a1-4345-a3a2-4e7f956e222d};i4)",
              "b939af5b-b45d-5489-9149-61442c1905fe" },
            { "IReference<Int32>",
              Inst(kIReference.name, { Fund("Int32") }),
              "pinterface({61c17706-2d65-11e0-9ae8-d48564015472};i4)",
              "548cefbd-bc8a-5fa0-8df2-957440fc8bf4" },
            { "IIterable<Int32>",
              Inst(kIIterable.name, { Fund("Int32") }),
              "pinterface({faa585ea-6214-4217-afda-7f46de5869b3};i4)",
              "81a643fb-f51c-5565-83c4-f96425777b66" },
            { "IMap<String,Object>",
              Inst(kIMap.name, { Fund("String"), Fund("Object") }),
              "pinterface({3c2925fe-8519-45c1-aa79-197b6718c1c1};string;cinterface(IInspectable))",
              "1b0d3570-0877-5ec2-8a2c-3b9539506aca" },
            { "IIterable<IKeyValuePair<String,Object>>",
              Inst(kIIterable.name, { Inst(kIKeyValuePair.name, { Fund("String"), Fund("Object") }) }),
              "pinterface({faa585ea-6214-4217-afda-7f46de5869b3};pinterface({02b51929-c1c4-4a7e-8940-0312b5c18500};string;cinterface(IInspectable)))",
              "fe2f3d47-5d47-5499-8374-430c7cda0204" },
        };

        for (const auto& c : cases)
        {
            std::string err;
            std::string sig = EncodeSig(c.type, m, err);
            bool sigOk = err.empty() && sig == c.sig;
            allOk = allOk && sigOk;
            line(std::string(sigOk ? "[ ok ] " : "[FAIL] ") + c.label + " sig: " +
                 (err.empty() ? sig : "ERROR: " + err) + (sigOk ? "" : std::string("\n        want ") + c.sig));

            uint8_t image[16];
            err.clear();
            bool derived = DerivePiid(c.type, m, image, err);
            std::string got = derived ? FormatGuidImage(image) : ("ERROR: " + err);
            bool iidOk = derived && got == c.iid;
            allOk = allOk && iidOk;
            line(std::string(iidOk ? "[ ok ] " : "[FAIL] ") + c.label + " IID: " + got +
                 (iidOk ? "" : std::string("  want ") + c.iid));
        }

        line(allOk ? "\nALL PASS" : "\nFAILURES PRESENT");
        return allOk;
    }
}
