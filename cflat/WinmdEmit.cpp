// WinRT metadata writer - serializes a cflat_winmd::Model into a .winmd image by hand.
//
// rometadata.dll is read-only (DefineScope -> E_NOTIMPL), so we build the ECMA-335 metadata
// ourselves: the #~ table stream + #Strings/#Blob/#GUID/#US heaps (II.24.2), wrapped in a
// minimal PE32 CLI image (II.25) so the OS reader's OpenScope accepts the file. The first
// validation bar is a clean round-trip through WinmdExtract (the Phase 0 reader).
#include "WinmdEmit.h"

#include <cstdint>
#include <cstdio>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace
{
    using namespace cflat_winmd;

    // ECMA-335 table ids we emit.
    enum Tid { T_Module = 0x00, T_TypeRef = 0x01, T_TypeDef = 0x02, T_Field = 0x04,
               T_MethodDef = 0x06, T_Param = 0x08, T_InterfaceImpl = 0x09, T_MemberRef = 0x0A,
               T_Constant = 0x0B, T_CustomAttribute = 0x0C, T_Assembly = 0x20, T_AssemblyRef = 0x23 };

    // CorElementType (encode side).
    enum { ET_VOID = 0x01, ET_BOOLEAN = 0x02, ET_CHAR = 0x03, ET_I1 = 0x04, ET_U1 = 0x05,
           ET_I2 = 0x06, ET_U2 = 0x07, ET_I4 = 0x08, ET_U4 = 0x09, ET_I8 = 0x0a, ET_U8 = 0x0b,
           ET_R4 = 0x0c, ET_R8 = 0x0d, ET_STRING = 0x0e, ET_VALUETYPE = 0x11, ET_CLASS = 0x12,
           ET_OBJECT = 0x1c };

    // TypeAttributes / FieldAttributes / MethodAttributes subset.
    constexpr uint32_t tdPublic = 0x1, tdAbstract = 0x80, tdInterface = 0x20, tdSealed = 0x100,
                       tdWindowsRuntime = 0x4000, tdImport = 0x1000, tdAutoLayout = 0x0,
                       tdSequentialLayout = 0x8, tdClassSemanticsMask = 0x20;
    constexpr uint16_t fdPublic = 0x6, fdStatic = 0x10, fdLiteral = 0x40, fdHasDefault = 0x8000,
                       fdRTSpecialName = 0x400, fdSpecialName = 0x200, fdPrivate = 0x1;
    constexpr uint16_t mdPublic = 0x6, mdVirtual = 0x40, mdAbstract = 0x400, mdHideBySig = 0x80,
                       mdNewSlot = 0x100, mdInstance = 0x0, mdStatic = 0x10, mdSpecialName = 0x800,
                       mdRTSpecialName = 0x1000;

    void CompressU(std::string& out, uint32_t v)
    {
        if (v < 0x80) out.push_back((char)v);
        else if (v < 0x4000) { out.push_back((char)(0x80 | (v >> 8))); out.push_back((char)(v & 0xff)); }
        else { out.push_back((char)(0xC0 | (v >> 24))); out.push_back((char)((v >> 16) & 0xff));
               out.push_back((char)((v >> 8) & 0xff)); out.push_back((char)(v & 0xff)); }
    }

    void put16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xff)); s.push_back((char)(v >> 8)); }
    void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xff)); }

    // Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into the 16-byte GUID image (Data1/2/3 little
    // endian, Data4 raw) - the same layout WinmdExtract::FormatGuid consumes.
    bool ParseGuid(const std::string& text, uint8_t out[16])
    {
        uint8_t raw[16]; int n = 0;
        for (char c : text)
        {
            if (c == '-' || c == '{' || c == '}') continue;
            int hi; if (c >= '0' && c <= '9') hi = c - '0';
            else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
            else return false;
            if (n >= 32) return false;
            if ((n & 1) == 0) raw[n / 2] = (uint8_t)(hi << 4); else raw[n / 2] |= (uint8_t)hi;
            n++;
        }
        if (n != 32) return false;
        out[0] = raw[3]; out[1] = raw[2]; out[2] = raw[1]; out[3] = raw[0];   // Data1 LE
        out[4] = raw[5]; out[5] = raw[4];                                     // Data2 LE
        out[6] = raw[7]; out[7] = raw[6];                                     // Data3 LE
        for (int i = 8; i < 16; i++) out[i] = raw[i];                         // Data4 raw
        return true;
    }

    // --- Heaps -------------------------------------------------------------------------------
    struct Heaps
    {
        std::string strings = std::string(1, '\0');
        std::string blob    = std::string(1, '\0');
        std::string us      = std::string(1, '\0');
        std::vector<uint8_t> guids;
        std::unordered_map<std::string, uint32_t> strMap, blobMap;

        uint32_t String(const std::string& s)
        {
            if (s.empty()) return 0;
            auto it = strMap.find(s); if (it != strMap.end()) return it->second;
            uint32_t off = (uint32_t)strings.size();
            strings.append(s); strings.push_back('\0');
            strMap[s] = off; return off;
        }
        uint32_t Blob(const std::string& b)
        {
            if (b.empty()) return 0;
            auto it = blobMap.find(b); if (it != blobMap.end()) return it->second;
            uint32_t off = (uint32_t)blob.size();
            std::string hdr; CompressU(hdr, (uint32_t)b.size());
            blob.append(hdr); blob.append(b);
            blobMap[b] = off; return off;
        }
        uint32_t Guid(const uint8_t g[16]) { guids.insert(guids.end(), g, g + 16); return (uint32_t)(guids.size() / 16); }
    };

    // --- Coded index descriptors (tag bits + member tables) ----------------------------------
    struct Coded { int tagBits; };
    constexpr Coded TypeDefOrRef        { 2 };  // TypeDef=0, TypeRef=1, TypeSpec=2
    constexpr Coded ResolutionScope     { 2 };  // Module=0, ModuleRef=1, AssemblyRef=2, TypeRef=3
    constexpr Coded MemberRefParent     { 3 };  // TypeDef=0, TypeRef=1, ModuleRef=2, MethodDef=3, TypeSpec=4
    constexpr Coded HasConstant         { 2 };  // Field=0, Param=1, Property=2
    constexpr Coded HasCustomAttribute  { 5 };  // MethodDef=0, Field=1, TypeRef=2, TypeDef=3, ...
    constexpr Coded CustomAttributeType { 3 };  // MethodDef=2, MemberRef=3

    static uint32_t CodeIndex(int tag, uint32_t row, Coded c) { return (row << c.tagBits) | (uint32_t)tag; }

    // A single column value plus how to size it at serialize time.
    struct Col
    {
        enum Kind { U16, U32, U8, Str, Blob, Guid, TableIdx, CodedIdx } kind;
        uint32_t value = 0;
        uint32_t aux = 0;   // TableIdx: table id; CodedIdx: tagBits
    };
    static Col CU8(uint32_t v)  { return { Col::U8,  v }; }
    static Col CU16(uint32_t v) { return { Col::U16, v }; }
    static Col CU32(uint32_t v) { return { Col::U32, v }; }
    static Col CStr(uint32_t v) { return { Col::Str, v }; }
    static Col CBlob(uint32_t v){ return { Col::Blob, v }; }
    static Col CGuid(uint32_t v){ return { Col::Guid, v }; }
    static Col CTab(uint32_t v, int tid) { return { Col::TableIdx, v, (uint32_t)tid }; }
    static Col CCod(uint32_t v, Coded c) { return { Col::CodedIdx, v, (uint32_t)c.tagBits }; }

    struct Table { std::vector<std::vector<Col>> rows; };

    class Writer
    {
    public:
        Heaps heaps;
        Table tables[0x40];
        // Sorted-table bitmask: set the bit for any table whose rows we keep sorted.
        uint64_t sortedMask = 0;

        uint32_t Add(int tid, std::vector<Col> row) { tables[tid].rows.push_back(std::move(row)); return (uint32_t)tables[tid].rows.size(); }
        uint32_t Rows(int tid) const { return (uint32_t)tables[tid].rows.size(); }

        // Build the #~ table stream into `out`.
        void EmitTableStream(std::string& out)
        {
            bool strBig = heaps.strings.size() >= 0x10000;
            bool guidBig = (heaps.guids.size() / 16) >= 0x10000;
            bool blobBig = heaps.blob.size() >= 0x10000;

            uint64_t valid = 0;
            uint32_t rowCount[0x40] = { 0 };
            for (int t = 0; t < 0x40; t++)
                if (!tables[t].rows.empty()) { valid |= (1ull << t); rowCount[t] = (uint32_t)tables[t].rows.size(); }

            auto tableIdxBig = [&](uint32_t tid) { return rowCount[tid] >= 0x10000; };
            auto codedBig = [&](int tagBits, std::initializer_list<int> tabs) {
                uint32_t mx = 0; for (int t : tabs) mx = std::max(mx, rowCount[t]);
                return mx >= (1u << (16 - tagBits));
            };
            // Which member tables each coded index spans (only what we use).
            auto codedWidth = [&](uint32_t tagBits) -> int {
                switch (tagBits) {
                case 2: // shared by TypeDefOrRef / ResolutionScope / HasConstant - size by the widest set
                    return codedBig(2, { T_TypeDef, T_TypeRef, T_Module, T_AssemblyRef, T_Field, T_Param }) ? 4 : 2;
                case 3: // MemberRefParent / CustomAttributeType
                    return codedBig(3, { T_TypeDef, T_TypeRef, T_MethodDef, T_MemberRef }) ? 4 : 2;
                case 5: // HasCustomAttribute
                    return codedBig(5, { T_MethodDef, T_Field, T_TypeRef, T_TypeDef, T_Param, T_InterfaceImpl,
                                         T_MemberRef, T_Module, T_Assembly, T_AssemblyRef }) ? 4 : 2;
                default: return 2;
                }
            };

            auto writeIdx = [&](std::string& s, uint32_t v, int width) {
                if (width == 2) put16(s, (uint16_t)v); else put32(s, v);
            };

            // Header.
            put32(out, 0);                                  // Reserved
            out.push_back(2); out.push_back(0);             // Major/Minor version
            uint8_t heapSizes = (strBig ? 1 : 0) | (guidBig ? 2 : 0) | (blobBig ? 4 : 0);
            out.push_back((char)heapSizes);
            out.push_back(1);                               // Reserved
            for (int i = 0; i < 8; i++) out.push_back((char)((valid >> (8 * i)) & 0xff));
            for (int i = 0; i < 8; i++) out.push_back((char)((sortedMask >> (8 * i)) & 0xff));
            for (int t = 0; t < 0x40; t++) if (valid & (1ull << t)) put32(out, rowCount[t]);

            // Rows.
            for (int t = 0; t < 0x40; t++)
            {
                if (!(valid & (1ull << t))) continue;
                for (auto& row : tables[t].rows)
                    for (auto& c : row)
                    {
                        switch (c.kind)
                        {
                        case Col::U8:  out.push_back((char)(c.value & 0xff)); break;
                        case Col::U16: put16(out, (uint16_t)c.value); break;
                        case Col::U32: put32(out, c.value); break;
                        case Col::Str: writeIdx(out, c.value, strBig ? 4 : 2); break;
                        case Col::Blob: writeIdx(out, c.value, blobBig ? 4 : 2); break;
                        case Col::Guid: writeIdx(out, c.value, guidBig ? 4 : 2); break;
                        case Col::TableIdx: writeIdx(out, c.value, tableIdxBig(c.aux) ? 4 : 2); break;
                        case Col::CodedIdx: writeIdx(out, c.value, codedWidth(c.aux)); break;
                        }
                    }
            }
        }
    };

    void Pad4(std::string& s) { while (s.size() & 3) s.push_back('\0'); }

    // Assemble the metadata root (BSJB) + the 5 streams into a single image.
    std::string BuildMetadataImage(Writer& w)
    {
        std::string tablesStream; w.EmitTableStream(tablesStream); Pad4(tablesStream);
        std::string& strings = w.heaps.strings; Pad4(strings);
        std::string& us = w.heaps.us; Pad4(us);
        std::string guidStream((const char*)w.heaps.guids.data(), w.heaps.guids.size());
        std::string& blob = w.heaps.blob; Pad4(blob);

        const char* ver = "WindowsRuntime 1.4";
        std::string verStr = ver; verStr.push_back('\0'); while (verStr.size() & 3) verStr.push_back('\0');

        struct Stream { const char* name; const std::string* data; };
        std::vector<Stream> streams = {
            { "#~", &tablesStream }, { "#Strings", &strings }, { "#US", &us },
            { "#GUID", &guidStream }, { "#Blob", &blob } };

        // Root header size (offsets are from the start of the metadata root).
        uint32_t header = 4 + 2 + 2 + 4 + 4 + (uint32_t)verStr.size() + 2 + 2;
        for (auto& s : streams)
        {
            uint32_t nameLen = (uint32_t)strlen(s.name) + 1; nameLen = (nameLen + 3) & ~3u;
            header += 8 + nameLen;
        }

        std::string img;
        put32(img, 0x424A5342);             // "BSJB"
        put16(img, 1); put16(img, 1);       // version 1.1
        put32(img, 0);                      // reserved
        put32(img, (uint32_t)verStr.size());
        img.append(verStr);
        put16(img, 0);                      // flags
        put16(img, (uint16_t)streams.size());

        uint32_t off = header;
        for (auto& s : streams)
        {
            put32(img, off); put32(img, (uint32_t)s.data->size());
            uint32_t rawLen = (uint32_t)strlen(s.name);
            uint32_t padded = (rawLen + 1 + 3) & ~3u;   // name + null, rounded to 4
            img.append(s.name); for (uint32_t i = rawLen; i < padded; i++) img.push_back('\0');
            off += (uint32_t)s.data->size();
        }
        for (auto& s : streams) img.append(*s.data);
        return img;
    }

    // Wrap the metadata image in a minimal PE32 CLI image so OpenScope accepts the file.
    std::string WrapPe(const std::string& metadata)
    {
        const uint32_t fileAlign = 0x200, sectAlign = 0x2000;
        const uint32_t imageBase = 0x400000;
        const uint32_t headersSize = 0x200;        // DOS+PE+optional+1 section header, padded to fileAlign
        const uint32_t textRva = sectAlign;        // 0x2000
        const uint32_t cliHeaderSize = 0x48;

        // Section content: [COR20 header][metadata].
        std::string text;
        // COR20 (CLI) header.
        put32(text, cliHeaderSize);                // cb
        put16(text, 2); put16(text, 5);            // runtime version 2.5
        put32(text, textRva + cliHeaderSize);      // MetaData RVA
        put32(text, (uint32_t)metadata.size());    // MetaData size
        put32(text, 0x00000001);                   // Flags = COMIMAGE_FLAGS_ILONLY
        put32(text, 0);                            // EntryPointToken
        for (int i = 0; i < 6; i++) put32(text, 0), put32(text, 0);  // 6 directory pairs (Resources..ManagedNative)
        text.append(metadata);

        uint32_t textRaw = (uint32_t)text.size();
        uint32_t textRawAligned = (textRaw + fileAlign - 1) & ~(fileAlign - 1);
        std::string textPadded = text; textPadded.resize(textRawAligned, '\0');
        uint32_t textVirt = (textRaw + sectAlign - 1) & ~(sectAlign - 1);
        uint32_t imageSize = textRva + textVirt;

        std::string pe;
        // DOS header: "MZ" + e_lfanew at 0x3C pointing to the PE signature at 0x80.
        pe.append("MZ"); pe.resize(0x3C, '\0'); put32(pe, 0x80); pe.resize(0x80, '\0');
        // PE signature + COFF header.
        pe.append("PE\0\0", 4);
        put16(pe, 0x014C);                  // Machine I386 (matches the PE32 optional header; ILONLY)
        put16(pe, 1);                       // NumberOfSections
        put32(pe, 0);                       // TimeDateStamp
        put32(pe, 0);                       // PointerToSymbolTable
        put32(pe, 0);                       // NumberOfSymbols
        put16(pe, 0xE0);                    // SizeOfOptionalHeader (PE32)
        put16(pe, 0x2102);                  // Characteristics: EXECUTABLE_IMAGE|32BIT_MACHINE|DLL
        // Optional header (PE32).
        put16(pe, 0x10B);                   // Magic PE32
        pe.push_back(8); pe.push_back(0);   // Linker version
        put32(pe, textRawAligned);          // SizeOfCode
        put32(pe, 0); put32(pe, 0);         // SizeOfInitializedData / Uninitialized
        put32(pe, 0);                       // AddressOfEntryPoint
        put32(pe, textRva);                 // BaseOfCode
        put32(pe, textRva + textVirt);      // BaseOfData (PE32 only)
        put32(pe, imageBase);
        put32(pe, sectAlign); put32(pe, fileAlign);
        put16(pe, 4); put16(pe, 0);         // OS version
        put16(pe, 0); put16(pe, 0);         // Image version
        put16(pe, 4); put16(pe, 0);         // Subsystem version
        put32(pe, 0);                       // Win32VersionValue
        put32(pe, imageSize);
        put32(pe, headersSize);
        put32(pe, 0);                       // CheckSum
        put16(pe, 3);                       // Subsystem = CUI
        put16(pe, 0x400);                   // DllCharacteristics = NX_COMPAT
        put32(pe, 0x100000); put32(pe, 0x1000);   // Stack reserve/commit
        put32(pe, 0x100000); put32(pe, 0x1000);   // Heap reserve/commit
        put32(pe, 0);                       // LoaderFlags
        put32(pe, 16);                      // NumberOfRvaAndSizes
        for (int i = 0; i < 16; i++)
        {
            if (i == 14) { put32(pe, textRva); put32(pe, cliHeaderSize); }  // CLI header directory
            else { put32(pe, 0); put32(pe, 0); }
        }
        // Section header ".text".
        std::string name = ".text"; name.resize(8, '\0'); pe.append(name);
        put32(pe, textVirt);                // VirtualSize
        put32(pe, textRva);                 // VirtualAddress
        put32(pe, textRawAligned);          // SizeOfRawData
        put32(pe, headersSize);             // PointerToRawData
        put32(pe, 0); put32(pe, 0);         // Relocations / Linenumbers
        put16(pe, 0); put16(pe, 0);
        put32(pe, 0x60000020);              // CODE|EXECUTE|READ

        pe.resize(headersSize, '\0');
        pe.append(textPadded);
        return pe;
    }
}

namespace cflat_winmd
{
    // ---- model -> tables -------------------------------------------------------------------
    namespace
    {
        struct EmitCtx
        {
            Writer w;
            uint32_t asmRefMscorlib = 0;      // AssemblyRef rows
            uint32_t asmRefFoundation = 0;
            uint32_t trObject = 0, trEnum = 0, trValueType = 0, trDelegate = 0, trGuidAttr = 0;
            uint32_t mrGuidCtor = 0;          // MemberRef row for GuidAttribute..ctor
            std::unordered_map<std::string, uint32_t> typeDefRow;   // full name -> TypeDef row
        };

        void SplitName(const std::string& full, std::string& ns, std::string& name)
        {
            auto dot = full.rfind('.');
            if (dot == std::string::npos) { ns = ""; name = full; }
            else { ns = full.substr(0, dot); name = full.substr(dot + 1); }
        }

        // Encode one signature element for a method/field type.
        void EncodeType(EmitCtx& c, std::string& sig, const TypeRef& t)
        {
            for (int i = 0; i < t.pointerDepth; i++) sig.push_back((char)0x0f);   // PTR
            if (t.isArray) sig.push_back((char)0x1d);                             // SZARRAY
            std::string fund = WinrtFundamentalToCFlat(t.fullName);
            auto byName = [&](uint8_t etag) {
                auto it = c.typeDefRow.find(t.fullName);
                if (it != c.typeDefRow.end()) { sig.push_back((char)etag); CompressU(sig, CodeIndex(0, it->second, TypeDefOrRef)); }
                else sig.push_back((char)ET_OBJECT);   // unknown named type -> degrade
            };
            if (t.fullName == "Void") sig.push_back((char)ET_VOID);
            else if (fund == "bool") sig.push_back((char)ET_BOOLEAN);
            else if (fund == "u16" && t.fullName == "Char16") sig.push_back((char)ET_CHAR);
            else if (fund == "i8") sig.push_back((char)ET_I1);
            else if (fund == "u8") sig.push_back((char)ET_U1);
            else if (fund == "i16") sig.push_back((char)ET_I2);
            else if (fund == "u16") sig.push_back((char)ET_U2);
            else if (fund == "i32") sig.push_back((char)ET_I4);
            else if (fund == "u32") sig.push_back((char)ET_U4);
            else if (fund == "i64") sig.push_back((char)ET_I8);
            else if (fund == "u64") sig.push_back((char)ET_U8);
            else if (fund == "f32") sig.push_back((char)ET_R4);
            else if (fund == "f64") sig.push_back((char)ET_R8);
            else if (fund == "string") sig.push_back((char)ET_STRING);
            else if (fund == "object") sig.push_back((char)ET_OBJECT);
            else byName(ET_CLASS);
        }

        std::string MethodSig(EmitCtx& c, const Method& m)
        {
            std::string sig;
            sig.push_back((char)(m.isStatic ? 0x00 : 0x20));   // HASTHIS for instance methods
            CompressU(sig, (uint32_t)m.params.size());
            EncodeType(c, sig, m.returnType);
            for (auto& p : m.params) EncodeType(c, sig, p.type);
            return sig;
        }

        std::string FieldSig(EmitCtx& c, const TypeRef& t)
        {
            std::string sig; sig.push_back((char)0x06);   // FIELD
            EncodeType(c, sig, t);
            return sig;
        }

        uint32_t AddTypeRef(EmitCtx& c, uint32_t scopeCoded, const std::string& full)
        {
            std::string ns, name; SplitName(full, ns, name);
            return c.w.Add(T_TypeRef, { CCod(scopeCoded, ResolutionScope),
                                        CStr(c.w.heaps.String(name)), CStr(c.w.heaps.String(ns)) });
        }
    }

    bool WriteWinmd(const Model& model, const std::string& assemblyName,
                    const std::string& path, std::string& err)
    {
        EmitCtx c;
        Writer& w = c.w;

        // Module (deterministic MVID from the assembly name via FNV-1a -> 16 bytes).
        uint8_t mvid[16]; uint64_t h = 1469598103934665603ull;
        for (char ch : assemblyName) { h ^= (uint8_t)ch; h *= 1099511628211ull; }
        for (int i = 0; i < 16; i++) mvid[i] = (uint8_t)(h >> ((i % 8) * 8)) ^ (uint8_t)(i * 31);
        mvid[7] = (mvid[7] & 0x0f) | 0x40; mvid[8] = (mvid[8] & 0x3f) | 0x80;   // v4 variant bits
        w.Add(T_Module, { CU16(0), CStr(w.heaps.String(assemblyName + ".winmd")),
                          CGuid(w.heaps.Guid(mvid)), CGuid(0), CGuid(0) });

        // Assembly (this winmd) + AssemblyRefs for the types we reference.
        w.Add(T_Assembly, { CU32(0x8004 /*SHA1*/), CU16(1), CU16(0), CU16(0), CU16(0),
                            CU32(0x0200 /*WindowsRuntime content type*/), CBlob(0),
                            CStr(w.heaps.String(assemblyName)), CStr(0) });
        auto addAsmRef = [&](const std::string& name, uint16_t major) {
            return w.Add(T_AssemblyRef, { CU16(major), CU16(0), CU16(0), CU16(0), CU32(0),
                                          CBlob(0), CStr(w.heaps.String(name)), CStr(0), CBlob(0) });
        };
        c.asmRefMscorlib  = addAsmRef("mscorlib", 4);
        c.asmRefFoundation = addAsmRef("Windows.Foundation.FoundationContract", 4);
        uint32_t scopeMscorlib  = CodeIndex(2 /*AssemblyRef*/, c.asmRefMscorlib, ResolutionScope);
        uint32_t scopeFoundation = CodeIndex(2, c.asmRefFoundation, ResolutionScope);

        c.trObject    = AddTypeRef(c, scopeMscorlib, "System.Object");
        c.trEnum      = AddTypeRef(c, scopeMscorlib, "System.Enum");
        c.trValueType = AddTypeRef(c, scopeMscorlib, "System.ValueType");
        c.trDelegate  = AddTypeRef(c, scopeMscorlib, "System.MulticastDelegate");
        c.trGuidAttr  = AddTypeRef(c, scopeFoundation, "Windows.Foundation.Metadata.GuidAttribute");

        // MemberRef GuidAttribute..ctor(u4,u2,u2,u1 x8) returning void.
        {
            std::string sig; sig.push_back((char)0x20);   // HASTHIS
            CompressU(sig, 11); sig.push_back((char)ET_VOID);
            sig.push_back((char)ET_U4); sig.push_back((char)ET_U2); sig.push_back((char)ET_U2);
            for (int i = 0; i < 8; i++) sig.push_back((char)ET_U1);
            c.mrGuidCtor = w.Add(T_MemberRef, { CCod(CodeIndex(1 /*TypeRef*/, c.trGuidAttr, MemberRefParent), MemberRefParent),
                                                CStr(w.heaps.String(".ctor")), CBlob(w.heaps.Blob(sig)) });
        }

        // Pre-assign TypeDef rows so signatures can reference sibling types by token. TypeDef
        // rid 1 is the special <Module> pseudo-type (owns global members; EnumTypeDefs excludes
        // it), so real types start at rid 2. We create them below in this same order.
        uint32_t nextTypeDef = 2;
        for (auto& i : model.interfaces) c.typeDefRow[i.fullName] = nextTypeDef++;
        for (auto& e : model.enums)      c.typeDefRow[e.fullName] = nextTypeDef++;
        for (auto& s : model.structs)    c.typeDefRow[s.fullName] = nextTypeDef++;
        for (auto& r : model.runtimeClasses) c.typeDefRow[r.fullName] = nextTypeDef++;

        std::vector<std::vector<Col>> ifaceImpls;   // collected, then sorted by Class
        std::vector<std::vector<Col>> custAttrs;    // collected, then sorted by Parent
        std::vector<std::vector<Col>> constants;

        auto addTypeDef = [&](const std::string& full, uint32_t flags, uint32_t extendsCoded) -> uint32_t {
            std::string ns, name; SplitName(full, ns, name);
            uint32_t fieldStart = w.Rows(T_Field) + 1;
            uint32_t methodStart = w.Rows(T_MethodDef) + 1;
            return w.Add(T_TypeDef, { CU32(flags), CStr(w.heaps.String(name)), CStr(w.heaps.String(ns)),
                                      CCod(extendsCoded, TypeDefOrRef), CTab(fieldStart, T_Field), CTab(methodStart, T_MethodDef) });
        };

        auto addGuidAttr = [&](uint32_t typeDefRow, const std::string& iid) {
            uint8_t g[16]; if (!ParseGuid(iid, g)) return;
            std::string val; val.push_back(0x01); val.push_back(0x00);
            val.append((const char*)g, 16);
            val.push_back(0x00); val.push_back(0x00);
            custAttrs.push_back({ CCod(CodeIndex(3 /*TypeDef*/, typeDefRow, HasCustomAttribute), HasCustomAttribute),
                                  CCod(CodeIndex(3 /*MemberRef*/, c.mrGuidCtor, CustomAttributeType), CustomAttributeType),
                                  CBlob(w.heaps.Blob(val)) });
        };

        auto addMethods = [&](const std::vector<Method>& methods) {
            for (auto& m : methods)
            {
                uint16_t flags = mdPublic | mdHideBySig | (m.isStatic ? mdStatic : (mdVirtual | mdNewSlot | mdAbstract));
                uint32_t paramStart = w.Rows(T_Param) + 1;
                w.Add(T_MethodDef, { CU32(0 /*RVA*/), CU16(0 /*ImplFlags*/), CU16(flags),
                                     CStr(w.heaps.String(m.name)), CBlob(w.heaps.Blob(MethodSig(c, m))),
                                     CTab(paramStart, T_Param) });
                uint16_t seq = 1;
                for (auto& p : m.params)
                    w.Add(T_Param, { CU16(p.dir == ParamDir::Out || p.dir == ParamDir::RetVal ? 0x0002 : 0x0001),
                                     CU16(seq++), CStr(w.heaps.String(p.name)) });
            }
        };

        // TypeDef rid 1: the <Module> pseudo-type (must precede all real types).
        addTypeDef("<Module>", 0, 0);

        // Interfaces.
        for (auto& iface : model.interfaces)
        {
            uint32_t row = addTypeDef(iface.fullName,
                tdPublic | tdInterface | tdAbstract | tdWindowsRuntime | tdImport, 0);
            addMethods(iface.methods);
            if (!iface.iid.empty()) addGuidAttr(row, iface.iid);
            for (auto& req : iface.requires_)
            {
                auto it = c.typeDefRow.find(req);
                uint32_t ifaceTok = it != c.typeDefRow.end()
                    ? CodeIndex(0 /*TypeDef*/, it->second, TypeDefOrRef)
                    : CodeIndex(1 /*TypeRef*/, AddTypeRef(c, scopeFoundation, req), TypeDefOrRef);
                ifaceImpls.push_back({ CTab(row, T_TypeDef), CCod(ifaceTok, TypeDefOrRef) });
            }
        }
        // Enums.
        for (auto& e : model.enums)
        {
            uint32_t row = addTypeDef(e.fullName, tdPublic | tdSealed | tdWindowsRuntime,
                CodeIndex(1 /*TypeRef*/, c.trEnum, TypeDefOrRef));
            // value__ instance field carrying the underlying type, then literal members.
            TypeRef ut; ut.fullName = e.underlying;
            w.Add(T_Field, { CU16(fdPublic | fdSpecialName | fdRTSpecialName),
                             CStr(w.heaps.String("value__")), CBlob(w.heaps.Blob(FieldSig(c, ut))) });
            for (auto& m : e.members)
            {
                uint32_t frow = w.Add(T_Field, { CU16(fdPublic | fdStatic | fdLiteral | fdHasDefault),
                                                 CStr(w.heaps.String(m.name)), CBlob(w.heaps.Blob(FieldSig(c, ut))) });
                std::string cval; bool wide = (e.underlying == "Int64" || e.underlying == "UInt64");
                if (wide) for (int i = 0; i < 8; i++) cval.push_back((char)((m.value >> (8 * i)) & 0xff));
                else for (int i = 0; i < 4; i++) cval.push_back((char)(((uint32_t)m.value >> (8 * i)) & 0xff));
                constants.push_back({ CU8(wide ? ET_I8 : ET_I4), CU8(0),
                                      CCod(CodeIndex(0 /*Field*/, frow, HasConstant), HasConstant),
                                      CBlob(w.heaps.Blob(cval)) });
            }
            (void)row;
        }
        // Value structs.
        for (auto& s : model.structs)
        {
            addTypeDef(s.fullName, tdPublic | tdSealed | tdSequentialLayout | tdWindowsRuntime,
                CodeIndex(1, c.trValueType, TypeDefOrRef));
            for (auto& f : s.fields)
                w.Add(T_Field, { CU16(fdPublic), CStr(w.heaps.String(f.name)), CBlob(w.heaps.Blob(FieldSig(c, f.type))) });
        }
        // Runtime classes.
        for (auto& rc : model.runtimeClasses)
        {
            uint32_t row = addTypeDef(rc.fullName, tdPublic | tdSealed | tdWindowsRuntime,
                CodeIndex(1, c.trObject, TypeDefOrRef));
            for (auto& iface : rc.interfaces)
            {
                auto it = c.typeDefRow.find(iface);
                uint32_t tok = it != c.typeDefRow.end()
                    ? CodeIndex(0, it->second, TypeDefOrRef)
                    : CodeIndex(1, AddTypeRef(c, scopeFoundation, iface), TypeDefOrRef);
                ifaceImpls.push_back({ CTab(row, T_TypeDef), CCod(tok, TypeDefOrRef) });
            }
        }

        // Emit the sorted tables (InterfaceImpl by Class, Constant by Parent, CustomAttribute by
        // Parent) and mark them sorted.
        std::sort(ifaceImpls.begin(), ifaceImpls.end(), [](auto& a, auto& b) { return a[0].value < b[0].value; });
        for (auto& r : ifaceImpls) w.Add(T_InterfaceImpl, r);
        std::sort(constants.begin(), constants.end(), [](auto& a, auto& b) { return a[2].value < b[2].value; });
        for (auto& r : constants) w.Add(T_Constant, r);
        std::sort(custAttrs.begin(), custAttrs.end(), [](auto& a, auto& b) { return a[0].value < b[0].value; });
        for (auto& r : custAttrs) w.Add(T_CustomAttribute, r);
        w.sortedMask = (1ull << T_InterfaceImpl) | (1ull << T_Constant) | (1ull << T_CustomAttribute);

        std::string image = BuildMetadataImage(w);
        std::string pe = WrapPe(image);

        FILE* fp = nullptr;
        if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) { err = "cannot open '" + path + "' for writing"; return false; }
        fwrite(pe.data(), 1, pe.size(), fp);
        fclose(fp);
        return true;
    }
}
