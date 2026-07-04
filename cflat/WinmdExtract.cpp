// WinRT metadata reader - raises a .winmd file into cflat_winmd::Model via the OS dispenser.
//
// Uses the Windows Runtime metadata COM API (MetaDataGetDispenser -> IMetaDataImport2). The
// Windows SDK ships only the dispenser entry point (rometadata.h) and the interfaces
// (RoMetadataApi.h); the ECMA-335 constants (CorElementType, token masks, type/field flags)
// and the metadata GUIDs are NOT in this SDK, so the stable subset is defined here. Signature
// blobs are decoded per ECMA-335 II.23.2.
#include "WinmdExtract.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <rometadata.h>
#include <RoMetadataApi.h>

#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "rometadata.lib")

namespace
{
    using namespace cflat_winmd;

    // --- ECMA-335 constants (corhdr.h subset, not shipped in this SDK) ----------------------
    enum CorElementType : unsigned
    {
        ET_END = 0x00, ET_VOID = 0x01, ET_BOOLEAN = 0x02, ET_CHAR = 0x03,
        ET_I1 = 0x04, ET_U1 = 0x05, ET_I2 = 0x06, ET_U2 = 0x07,
        ET_I4 = 0x08, ET_U4 = 0x09, ET_I8 = 0x0a, ET_U8 = 0x0b,
        ET_R4 = 0x0c, ET_R8 = 0x0d, ET_STRING = 0x0e, ET_PTR = 0x0f,
        ET_BYREF = 0x10, ET_VALUETYPE = 0x11, ET_CLASS = 0x12, ET_VAR = 0x13,
        ET_ARRAY = 0x14, ET_GENERICINST = 0x15, ET_TYPEDBYREF = 0x16,
        ET_I = 0x18, ET_U = 0x19, ET_FNPTR = 0x1b, ET_OBJECT = 0x1c,
        ET_SZARRAY = 0x1d, ET_MVAR = 0x1e, ET_CMOD_REQD = 0x1f, ET_CMOD_OPT = 0x20,
    };

    constexpr DWORD kTokenMask     = 0xff000000;
    constexpr DWORD kRidMask       = 0x00ffffff;
    constexpr DWORD kmdtTypeDef    = 0x02000000;
    constexpr DWORD kmdtTypeRef    = 0x01000000;
    constexpr DWORD kmdtTypeSpec   = 0x1b000000;

    constexpr DWORD ktdInterface   = 0x00000020;   // CorTypeAttr: class-semantics = interface
    constexpr DWORD kfdStatic      = 0x00000010;   // CorFieldAttr
    constexpr DWORD kfdLiteral     = 0x00000040;
    constexpr DWORD kmdStatic      = 0x00000010;   // CorMethodAttr
    constexpr DWORD kpdOut         = 0x00000002;   // CorParamAttr
    constexpr DWORD kCallConvGeneric = 0x10;       // IMAGE_CEE_CS_CALLCONV_GENERIC
    constexpr DWORD kofReadOnly    = 0x00000010;   // CorOpenFlags

    // Metadata GUIDs (defined locally so we depend only on rometadata.lib's MetaDataGetDispenser,
    // never on whichever lib happens to export the IID/CLSID symbols).
    const GUID kCLSID_CorMetaDataDispenser =
        { 0xE5CB7A31, 0x7512, 0x11d2, { 0x89, 0xCE, 0x00, 0x80, 0xC7, 0x92, 0xE5, 0xD8 } };
    const GUID kIID_IMetaDataDispenser =
        { 0x809C652E, 0x7396, 0x11D2, { 0x97, 0x71, 0x00, 0xA0, 0xC9, 0xB4, 0xD5, 0x0C } };
    const GUID kIID_IMetaDataImport2 =
        { 0xFCE5EFA0, 0x8BBA, 0x4f8e, { 0xA0, 0x36, 0x8F, 0x20, 0x22, 0xB0, 0x84, 0x66 } };

    // TypeFromToken / RidFromToken / IsNilToken are function-like macros from the metadata
    // headers; use them directly rather than redefining (kTokenMask/kRidMask kept for clarity).

    std::string ToUtf8(const wchar_t* w, size_t len)
    {
        if (!w || len == 0) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return "";
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, (int)len, s.data(), n, nullptr, nullptr);
        return s;
    }

    // Strip the ECMA generic-arity suffix ("IVector`1" -> "IVector").
    std::string StripArity(const std::string& name)
    {
        auto tick = name.find('`');
        return tick == std::string::npos ? name : name.substr(0, tick);
    }

    // Generic arity encoded in an ECMA type name ("IVector`1" -> 1, non-generic -> 0).
    int ArityOf(const std::string& name)
    {
        auto tick = name.find('`');
        if (tick == std::string::npos) return 0;
        int n = std::atoi(name.c_str() + tick + 1);
        return n > 0 ? n : 0;
    }

    std::string FormatGuid(const BYTE* g)
    {
        // g is the 16-byte GUID image: Data1 (4 LE), Data2 (2 LE), Data3 (2 LE), Data4 (8 raw).
        unsigned d1 = g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24);
        unsigned d2 = g[4] | (g[5] << 8);
        unsigned d3 = g[6] | (g[7] << 8);
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
        return buf;
    }

    // A decode cursor over a signature/typespec blob.
    struct Cursor
    {
        const BYTE* p;
        const BYTE* end;
        bool ok = true;

        unsigned Data()
        {
            if (p >= end) { ok = false; return 0; }
            BYTE b = *p;
            if ((b & 0x80) == 0) { p += 1; return b; }
            if ((b & 0xC0) == 0x80)
            {
                if (p + 2 > end) { ok = false; return 0; }
                unsigned v = ((b & 0x3f) << 8) | p[1]; p += 2; return v;
            }
            if (p + 4 > end) { ok = false; return 0; }
            unsigned v = ((b & 0x1f) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            p += 4; return v;
        }

        // Decode a TypeDefOrRef-or-Spec coded token (2 low bits select the table).
        mdToken Token()
        {
            unsigned data = Data();
            static const DWORD table[4] = { kmdtTypeDef, kmdtTypeRef, kmdtTypeSpec, 0 };
            return (mdToken)((data >> 2) | table[data & 3]);
        }
    };

    class Reader
    {
    public:
        IMetaDataImport2* md = nullptr;

        // Resolve a TypeDef/TypeRef token to its full dotted name (arity suffix stripped).
        std::string ResolveTypeName(mdToken tk)
        {
            if (IsNilToken(tk)) return "";
            wchar_t buf[1024];
            ULONG len = 0;
            if (TypeFromToken(tk) == kmdtTypeDef)
            {
                if (FAILED(md->GetTypeDefProps(tk, buf, 1024, &len, nullptr, nullptr))) return "";
            }
            else if (TypeFromToken(tk) == kmdtTypeRef)
            {
                if (FAILED(md->GetTypeRefProps(tk, nullptr, buf, 1024, &len))) return "";
            }
            else
            {
                return "";   // TypeSpec base - rare in a base-class slot; not needed for Phase 0
            }
            return StripArity(ToUtf8(buf, len > 0 ? len - 1 : 0));
        }

        // Decode one type from a signature cursor (ECMA-335 II.23.2.12).
        TypeRef DecodeType(Cursor& c)
        {
            TypeRef t;
            unsigned et = c.Data();
            switch (et)
            {
            case ET_VOID:    t.fullName = "Void"; break;
            case ET_BOOLEAN: t.fullName = "Boolean"; break;
            case ET_CHAR:    t.fullName = "Char16"; break;
            case ET_I1:      t.fullName = "Int8"; break;
            case ET_U1:      t.fullName = "UInt8"; break;
            case ET_I2:      t.fullName = "Int16"; break;
            case ET_U2:      t.fullName = "UInt16"; break;
            case ET_I4:      t.fullName = "Int32"; break;
            case ET_U4:      t.fullName = "UInt32"; break;
            case ET_I8:      t.fullName = "Int64"; break;
            case ET_U8:      t.fullName = "UInt64"; break;
            case ET_R4:      t.fullName = "Single"; break;
            case ET_R8:      t.fullName = "Double"; break;
            case ET_I:       t.fullName = "IntPtr"; break;
            case ET_U:       t.fullName = "UIntPtr"; break;
            case ET_STRING:  t.fullName = "String"; break;
            case ET_OBJECT:  t.fullName = "Object"; break;
            case ET_VALUETYPE:
            case ET_CLASS:
            {
                std::string n = ResolveTypeName(c.Token());
                t.fullName = (n == "System.Guid") ? "Guid" : n;
                break;
            }
            case ET_VAR:
            case ET_MVAR:
                t.isGenericVar = true;
                t.genericVarIndex = (int)c.Data();
                break;
            case ET_GENERICINST:
            {
                c.Data();   // inner CLASS/VALUETYPE marker
                std::string n = ResolveTypeName(c.Token());
                t.fullName = (n == "System.Guid") ? "Guid" : n;
                unsigned argc = c.Data();
                for (unsigned i = 0; i < argc && c.ok; i++) t.genericArgs.push_back(DecodeType(c));
                break;
            }
            case ET_SZARRAY:
            {
                t = DecodeType(c);
                t.isArray = true;
                break;
            }
            case ET_BYREF:
            case ET_PTR:
            {
                t = DecodeType(c);
                t.pointerDepth++;
                break;
            }
            case ET_CMOD_REQD:
            case ET_CMOD_OPT:
                c.Token();              // skip the modifier type
                return DecodeType(c);   // and decode the real type
            default:
                t.fullName = "Void";    // unknown element type - degrade rather than fail the read
                break;
            }
            return t;
        }

        // Decode a method signature: callconv, optional generic count, param count, ret, params.
        void DecodeMethodSig(const BYTE* sig, ULONG cb, Method& m)
        {
            Cursor c{ sig, sig + cb };
            unsigned callconv = c.Data();
            if (callconv & kCallConvGeneric) c.Data();   // generic param count
            unsigned paramCount = c.Data();
            m.returnType = DecodeType(c);
            // WinRT metadata stores the logical return and carries HRESULT implicitly (projected
            // back as a trailing [out,retval] param). Win32 (raw-COM) metadata spells the HRESULT
            // return explicitly and lists every out-param, so there is no implicit retval.
            if (m.returnType.fullName == "Windows.Win32.Foundation.HRESULT" && m.returnType.pointerDepth == 0)
                m.hresultImplicit = false;
            for (unsigned i = 0; i < paramCount && c.ok; i++)
            {
                Param p;
                p.type = DecodeType(c);
                m.params.push_back(std::move(p));
            }
        }

        // Pull a Windows.Foundation.Metadata.GuidAttribute off a token as canonical GUID text.
        std::string ReadIid(mdToken tk)
        {
            const BYTE* data = nullptr;
            ULONG cb = 0;
            HRESULT hr = md->GetCustomAttributeByName(
                tk, L"Windows.Foundation.Metadata.GuidAttribute", (const void**)&data, &cb);
            if (FAILED(hr) || !data || cb < 2 + 16) return "";
            return FormatGuid(data + 2);   // skip the 2-byte attribute prolog
        }

        bool HasAttribute(mdToken tk, const wchar_t* name)
        {
            const BYTE* data = nullptr;
            ULONG cb = 0;
            return md->GetCustomAttributeByName(tk, name, (const void**)&data, &cb) == S_OK;
        }

        // Fill method names and out-param flags from the ParamDef rows (signature has no names).
        void FillParamNames(mdMethodDef method, Method& m)
        {
            HCORENUM e = nullptr;
            mdParamDef params[64];
            ULONG n = 0;
            if (FAILED(md->EnumParams(&e, method, params, 64, &n))) return;
            for (ULONG i = 0; i < n; i++)
            {
                ULONG seq = 0;
                DWORD attr = 0;
                wchar_t nameBuf[256];
                ULONG nameLen = 0;
                if (FAILED(md->GetParamProps(params[i], nullptr, &seq, nameBuf, 256, &nameLen,
                                             &attr, nullptr, nullptr, nullptr)))
                    continue;
                if (seq == 0 || seq > m.params.size()) continue;   // seq 0 is the return
                Param& p = m.params[seq - 1];
                p.name = ToUtf8(nameBuf, nameLen > 0 ? nameLen - 1 : 0);
                if (attr & kpdOut)
                    p.dir = (i + 1 == n) ? ParamDir::RetVal : ParamDir::Out;
            }
            md->CloseEnum(e);
        }

        void ReadMethods(mdTypeDef td, std::vector<Method>& out)
        {
            HCORENUM e = nullptr;
            mdMethodDef methods[256];
            ULONG n = 0;
            if (FAILED(md->EnumMethods(&e, td, methods, 256, &n))) return;
            for (ULONG i = 0; i < n; i++)
            {
                wchar_t nameBuf[512];
                ULONG nameLen = 0, cbSig = 0;
                DWORD attr = 0;
                PCCOR_SIGNATURE sig = nullptr;
                if (FAILED(md->GetMethodProps(methods[i], nullptr, nameBuf, 512, &nameLen,
                                              &attr, &sig, &cbSig, nullptr, nullptr)))
                    continue;
                std::string name = ToUtf8(nameBuf, nameLen > 0 ? nameLen - 1 : 0);
                if (name == ".ctor" || name == ".cctor") continue;
                Method m;
                m.name = name;
                m.isStatic = (attr & kmdStatic) != 0;
                DecodeMethodSig(sig, cbSig, m);
                FillParamNames(methods[i], m);
                out.push_back(std::move(m));
            }
            md->CloseEnum(e);
        }

        void ReadInterfaceRequires(mdTypeDef td, std::vector<std::string>& out, mdToken* defaultIface = nullptr)
        {
            HCORENUM e = nullptr;
            mdInterfaceImpl impls[128];
            ULONG n = 0;
            if (FAILED(md->EnumInterfaceImpls(&e, td, impls, 128, &n))) return;
            for (ULONG i = 0; i < n; i++)
            {
                mdToken iface = 0;
                if (FAILED(md->GetInterfaceImplProps(impls[i], nullptr, &iface))) continue;
                std::string name = ResolveTypeName(iface);
                if (!name.empty()) out.push_back(name);
                if (defaultIface && HasAttribute(impls[i], L"Windows.Foundation.Metadata.DefaultAttribute"))
                    *defaultIface = iface;
            }
            md->CloseEnum(e);
        }

        // Read a literal int64 enum-member value out of the field constant blob.
        int64_t ReadFieldConstant(mdFieldDef fd, unsigned& elemTypeOut)
        {
            DWORD cplus = 0;
            UVCP_CONSTANT val = nullptr;
            ULONG vlen = 0;
            if (FAILED(md->GetFieldProps(fd, nullptr, nullptr, 0, nullptr, nullptr,
                                         nullptr, nullptr, &cplus, &val, &vlen)))
                return 0;
            elemTypeOut = cplus;
            if (!val) return 0;
            const unsigned char* b = (const unsigned char*)val;
            switch (cplus)
            {
            case ET_I4: case ET_U4: return (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24));
            case ET_I8: case ET_U8:
            {
                int64_t v = 0;
                for (int i = 0; i < 8; i++) v |= (int64_t)b[i] << (8 * i);
                return v;
            }
            case ET_I2: case ET_U2: return (int16_t)(b[0] | (b[1] << 8));
            case ET_I1: case ET_U1: return (int8_t)b[0];
            default: return 0;
            }
        }
    };

    void ReadEnum(Reader& r, mdTypeDef td, const std::string& fullName, Model& out)
    {
        Enum e;
        e.fullName = fullName;
        e.isFlags = r.HasAttribute(td, L"System.FlagsAttribute") ||
                    r.HasAttribute(td, L"Windows.Foundation.Metadata.FlagsAttribute");
        HCORENUM en = nullptr;
        mdFieldDef fields[512];
        ULONG n = 0;
        if (SUCCEEDED(r.md->EnumFields(&en, td, fields, 512, &n)))
        {
            for (ULONG i = 0; i < n; i++)
            {
                wchar_t nameBuf[256];
                ULONG nameLen = 0;
                DWORD attr = 0;
                if (FAILED(r.md->GetFieldProps(fields[i], nullptr, nameBuf, 256, &nameLen, &attr,
                                               nullptr, nullptr, nullptr, nullptr, nullptr)))
                    continue;
                std::string name = ToUtf8(nameBuf, nameLen > 0 ? nameLen - 1 : 0);
                if (!(attr & kfdStatic))     // the instance "value__" field carries the underlying type
                {
                    unsigned et = 0;
                    Cursor c{ nullptr, nullptr };
                    (void)c;
                    PCCOR_SIGNATURE sig = nullptr;
                    ULONG cbSig = 0;
                    if (SUCCEEDED(r.md->GetFieldProps(fields[i], nullptr, nullptr, 0, nullptr, nullptr,
                                                      &sig, &cbSig, nullptr, nullptr, nullptr)) && sig && cbSig >= 2)
                    {
                        Cursor fc{ sig + 1, sig + cbSig };   // skip FIELD callconv byte
                        TypeRef ut = r.DecodeType(fc);
                        if (!ut.fullName.empty()) e.underlying = ut.fullName;
                    }
                    (void)et;
                    continue;
                }
                unsigned elemType = 0;
                int64_t v = r.ReadFieldConstant(fields[i], elemType);
                e.members.push_back({ name, v });
            }
            r.md->CloseEnum(en);
        }
        out.enums.push_back(std::move(e));
    }

    void ReadStruct(Reader& r, mdTypeDef td, const std::string& fullName, Model& out)
    {
        Struct s;
        s.fullName = fullName;
        HCORENUM en = nullptr;
        mdFieldDef fields[512];
        ULONG n = 0;
        if (SUCCEEDED(r.md->EnumFields(&en, td, fields, 512, &n)))
        {
            for (ULONG i = 0; i < n; i++)
            {
                wchar_t nameBuf[256];
                ULONG nameLen = 0, cbSig = 0;
                DWORD attr = 0;
                PCCOR_SIGNATURE sig = nullptr;
                if (FAILED(r.md->GetFieldProps(fields[i], nullptr, nameBuf, 256, &nameLen, &attr,
                                               &sig, &cbSig, nullptr, nullptr, nullptr)))
                    continue;
                if (attr & kfdStatic) continue;
                Field f;
                f.name = ToUtf8(nameBuf, nameLen > 0 ? nameLen - 1 : 0);
                if (sig && cbSig >= 2)
                {
                    Cursor fc{ sig + 1, sig + cbSig };
                    f.type = r.DecodeType(fc);
                }
                s.fields.push_back(std::move(f));
            }
            r.md->CloseEnum(en);
        }
        out.structs.push_back(std::move(s));
    }
}

namespace cflat_winmd
{
    bool ReadWinmd(const std::string& path, Model& out, std::string& err)
    {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool didInit = SUCCEEDED(hrInit);

        IMetaDataDispenser* disp = nullptr;
        HRESULT hr = MetaDataGetDispenser(kCLSID_CorMetaDataDispenser, kIID_IMetaDataDispenser, (void**)&disp);
        if (FAILED(hr) || !disp)
        {
            err = "MetaDataGetDispenser failed (hr=" + std::to_string((unsigned)hr) + ")";
            if (didInit) CoUninitialize();
            return false;
        }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath((size_t)(wlen > 0 ? wlen : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        IUnknown* unk = nullptr;
        hr = disp->OpenScope(wpath.c_str(), kofReadOnly, kIID_IMetaDataImport2, &unk);
        if (FAILED(hr) || !unk)
        {
            err = "OpenScope failed for '" + path + "' (hr=" + std::to_string((unsigned)hr) + ")";
            disp->Release();
            if (didInit) CoUninitialize();
            return false;
        }

        Reader r;
        unk->QueryInterface(kIID_IMetaDataImport2, (void**)&r.md);

        HCORENUM e = nullptr;
        mdTypeDef typeDefs[512];
        ULONG n = 0;
        while (SUCCEEDED(r.md->EnumTypeDefs(&e, typeDefs, 512, &n)) && n > 0)
        {
            for (ULONG i = 0; i < n; i++)
            {
                mdTypeDef td = typeDefs[i];
                wchar_t nameBuf[1024];
                ULONG nameLen = 0;
                DWORD flags = 0;
                mdToken extends = 0;
                if (FAILED(r.md->GetTypeDefProps(td, nameBuf, 1024, &nameLen, &flags, &extends)))
                    continue;
                std::string fullName = ToUtf8(nameBuf, nameLen > 0 ? nameLen - 1 : 0);
                if (fullName.empty() || fullName[0] == '<') continue;   // <Module>

                if (flags & ktdInterface)
                {
                    Interface iface;
                    iface.fullName = StripArity(fullName);
                    iface.iid = r.ReadIid(td);
                    for (int gp = 0, arity = ArityOf(fullName); gp < arity; gp++)
                        iface.genericParams.push_back("T" + std::to_string(gp));
                    r.ReadInterfaceRequires(td, iface.requires_);
                    r.ReadMethods(td, iface.methods);
                    out.interfaces.push_back(std::move(iface));
                    continue;
                }

                std::string base = r.ResolveTypeName(extends);
                if (base == "System.Enum")
                {
                    ReadEnum(r, td, fullName, out);
                }
                else if (base == "System.MulticastDelegate" || base == "System.Delegate")
                {
                    Delegate d;
                    d.fullName = StripArity(fullName);
                    d.iid = r.ReadIid(td);
                    for (int gp = 0, arity = ArityOf(fullName); gp < arity; gp++)
                        d.genericParams.push_back("T" + std::to_string(gp));
                    std::vector<Method> ms;
                    r.ReadMethods(td, ms);
                    for (auto& m : ms) if (m.name == "Invoke") { d.invoke = std::move(m); break; }
                    out.delegates.push_back(std::move(d));
                }
                else if (base == "System.ValueType")
                {
                    ReadStruct(r, td, fullName, out);
                }
                else
                {
                    RuntimeClass rc;
                    rc.fullName = fullName;
                    rc.activatable = r.HasAttribute(td, L"Windows.Foundation.Metadata.ActivatableAttribute");
                    mdToken def = 0;
                    r.ReadInterfaceRequires(td, rc.interfaces, &def);
                    if (!IsNilToken(def)) rc.defaultInterface = r.ResolveTypeName(def);
                    out.runtimeClasses.push_back(std::move(rc));
                }
            }
            n = 0;
        }
        r.md->CloseEnum(e);

        if (r.md) r.md->Release();
        unk->Release();
        disp->Release();
        if (didInit) CoUninitialize();
        return true;
    }
}
