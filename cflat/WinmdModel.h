// WinRT metadata model - the shared spine for both directions of the WinMD projection.
//
// This header is deliberately free of any Windows/LLVM/ANTLR types so it can be included
// by LLVMBackend.h without dragging the OS metadata COM headers into that (already /bigobj)
// translation unit. All `IMetaDataImport2` usage lives in WinmdExtract.cpp; all emit usage
// (later) lives in WinmdEmit.cpp. Both sides read and write THIS model, so emitted metadata
// and emitted code can never disagree on slot order, IIDs, or signatures.
//
// The model is plain data: a reader (consume) raises ECMA-335 metadata into it, and a builder
// (produce) lowers CFlat `[winrt]` types into it. typeMap is the one place WinRT fundamental
// types and CFlat types are bridged, shared by both directions.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace cflat_winmd
{
    // Logical kind of a WinRT type definition. Determined by the metadata base type / flags
    // (Enum -> System.Enum, Struct -> System.ValueType, Delegate -> System.MulticastDelegate,
    // Interface -> tdInterface flag, RuntimeClass -> a normal class).
    enum class Category { Unknown, Interface, RuntimeClass, Struct, Enum, Delegate, Attribute };

    // A resolved type reference as it appears in a signature (a param, return, or field type).
    // Fundamental types carry their WinRT name ("Int32", "String", "Object", "Guid", "Void");
    // named types carry their full "Namespace.TypeName". Pointer depth models BYREF/PTR (an
    // `out`/`ref` param adds one level); generic instantiations carry their argument list.
    struct TypeRef
    {
        std::string fullName;                 // "Int32" | "String" | "Guid" | "Namespace.IFoo"
        int pointerDepth = 0;                 // BYREF / PTR levels (out-params, raw pointers)
        bool isArray = false;                 // SZARRAY element
        bool isGenericVar = false;            // ELEMENT_TYPE_VAR/MVAR (an unbound generic param)
        int genericVarIndex = -1;             // index of the generic param when isGenericVar
        std::vector<TypeRef> genericArgs;     // GENERICINST arguments (e.g. IVector<Int32>)

        // Human-readable spelling for dumps/diagnostics (NOT the CFlat lowering - see typeMap).
        std::string Spelling() const
        {
            std::string base;
            if (isGenericVar)
                base = "T" + std::to_string(genericVarIndex);
            else
                base = fullName;
            if (!genericArgs.empty())
            {
                base += "<";
                for (size_t i = 0; i < genericArgs.size(); i++)
                {
                    if (i) base += ", ";
                    base += genericArgs[i].Spelling();
                }
                base += ">";
            }
            if (isArray) base += "[]";
            for (int i = 0; i < pointerDepth; i++) base += "*";
            return base;
        }
    };

    enum class ParamDir { In, Out, RetVal };

    struct Param
    {
        std::string name;
        TypeRef type;
        ParamDir dir = ParamDir::In;
    };

    // A logical method signature. WinRT methods are HRESULT at the ABI with the declared
    // return passed back through a trailing [out,retval] param; the reader raises that ABI
    // form to this logical form and the codegen lowers it back. `hresultImplicit` records
    // that the ABI carries the extra HRESULT (true for every projected WinRT method).
    struct Method
    {
        std::string name;
        TypeRef returnType;                   // logical return ("Void" when none)
        std::vector<Param> params;            // logical params (no `this`, no HRESULT)
        bool isStatic = false;
        bool hresultImplicit = true;
    };

    struct Field
    {
        std::string name;
        TypeRef type;
    };

    // method order == vtable slot order (after the 6 IUnknown+IInspectable slots).
    struct Interface
    {
        std::string fullName;                 // "Namespace.IFoo"
        std::string iid;                      // canonical GUID text, lowercase, no braces
        std::vector<std::string> requires_;   // required interface full names (QI-reachable)
        std::vector<std::string> genericParams; // names for IVector<T>; empty when non-generic
        std::vector<Method> methods;
    };

    struct Struct
    {
        std::string fullName;
        std::vector<Field> fields;
    };

    struct EnumMember
    {
        std::string name;
        int64_t value = 0;
    };

    struct Enum
    {
        std::string fullName;
        std::string underlying = "Int32";     // "Int32" | "UInt32"
        bool isFlags = false;
        std::vector<EnumMember> members;
    };

    struct Delegate
    {
        std::string fullName;
        std::string iid;
        std::vector<std::string> genericParams;
        Method invoke;                        // the Invoke signature
    };

    struct RuntimeClass
    {
        std::string fullName;
        std::string defaultInterface;         // the [default] interface full name (may be empty)
        std::vector<std::string> interfaces;  // all instance interfaces (incl. default)
        std::vector<std::string> factoryInterfaces;
        std::vector<std::string> staticInterfaces;
        bool activatable = false;             // default-activatable (parameterless construction)
    };

    // The whole projected surface of one or more .winmd files. Lookups are by full name.
    struct Model
    {
        std::vector<Interface> interfaces;
        std::vector<Struct> structs;
        std::vector<Enum> enums;
        std::vector<Delegate> delegates;
        std::vector<RuntimeClass> runtimeClasses;

        const Interface* FindInterface(const std::string& fullName) const
        {
            for (const auto& i : interfaces) if (i.fullName == fullName) return &i;
            return nullptr;
        }
    };

    // --- Shared type map (used by BOTH consume and produce) ---------------------------------
    // Returns the CFlat spelling for a WinRT fundamental/system type, or "" if `winrtName`
    // is not a fundamental (i.e. it is a user-defined named type that maps to itself).
    inline std::string WinrtFundamentalToCFlat(const std::string& winrtName)
    {
        if (winrtName == "Void")    return "void";
        if (winrtName == "Boolean") return "bool";
        if (winrtName == "Int8")    return "i8";
        if (winrtName == "UInt8")   return "u8";
        if (winrtName == "Int16")   return "i16";
        if (winrtName == "UInt16")  return "u16";
        if (winrtName == "Int32")   return "i32";
        if (winrtName == "UInt32")  return "u32";
        if (winrtName == "Int64")   return "i64";
        if (winrtName == "UInt64")  return "u64";
        if (winrtName == "Single")  return "f32";
        if (winrtName == "Double")  return "f64";
        if (winrtName == "Char16")  return "u16";   // UTF-16 code unit
        if (winrtName == "String")  return "string";
        if (winrtName == "Guid")    return "Guid";
        if (winrtName == "Object")  return "object";
        return "";
    }

    // Inverse of the above; returns "" when `cflatName` has no WinRT fundamental equivalent.
    inline std::string CFlatToWinrtFundamental(const std::string& cflatName)
    {
        if (cflatName == "void")   return "Void";
        if (cflatName == "bool")   return "Boolean";
        if (cflatName == "i8")     return "Int8";
        if (cflatName == "u8")     return "UInt8";
        if (cflatName == "i16")    return "Int16";
        if (cflatName == "u16")    return "UInt16";
        if (cflatName == "i32")    return "Int32";
        if (cflatName == "u32")    return "UInt32";
        if (cflatName == "i64")    return "Int64";
        if (cflatName == "u64")    return "UInt64";
        if (cflatName == "f32")    return "Single";
        if (cflatName == "f64")    return "Double";
        if (cflatName == "string") return "String";
        if (cflatName == "Guid")   return "Guid";
        if (cflatName == "object") return "Object";
        return "";
    }

    // --- Validation dump (Phase 0) ----------------------------------------------------------
    // A compact human-readable rendering of the whole model, used by `--dump-winmd` to confirm
    // the reader faithfully raised the metadata. Not a stable format - diagnostics only.
    inline std::string DumpMethod(const Method& m, const char* indent)
    {
        std::string s = indent;
        s += m.isStatic ? "static " : "";
        s += m.returnType.Spelling() + " " + m.name + "(";
        for (size_t i = 0; i < m.params.size(); i++)
        {
            if (i) s += ", ";
            if (m.params[i].dir == ParamDir::Out) s += "out ";
            s += m.params[i].type.Spelling();
            if (!m.params[i].name.empty()) s += " " + m.params[i].name;
        }
        s += ")\n";
        return s;
    }

    inline std::string DumpModel(const Model& model)
    {
        std::string s;
        s += "interfaces: " + std::to_string(model.interfaces.size()) +
             ", structs: " + std::to_string(model.structs.size()) +
             ", enums: " + std::to_string(model.enums.size()) +
             ", delegates: " + std::to_string(model.delegates.size()) +
             ", runtimeClasses: " + std::to_string(model.runtimeClasses.size()) + "\n\n";

        for (const auto& i : model.interfaces)
        {
            s += "interface " + i.fullName;
            if (!i.genericParams.empty())
            {
                s += "<";
                for (size_t k = 0; k < i.genericParams.size(); k++) { if (k) s += ", "; s += i.genericParams[k]; }
                s += ">";
            }
            s += "  [" + i.iid + "]\n";
            for (const auto& r : i.requires_) s += "  requires " + r + "\n";
            for (const auto& m : i.methods) s += DumpMethod(m, "  ");
            s += "\n";
        }
        for (const auto& e : model.enums)
        {
            s += "enum " + e.fullName + " : " + e.underlying + (e.isFlags ? " [flags]" : "") + "\n";
            for (const auto& mem : e.members) s += "  " + mem.name + " = " + std::to_string(mem.value) + "\n";
            s += "\n";
        }
        for (const auto& st : model.structs)
        {
            s += "struct " + st.fullName + "\n";
            for (const auto& f : st.fields) s += "  " + f.type.Spelling() + " " + f.name + "\n";
            s += "\n";
        }
        for (const auto& d : model.delegates)
        {
            s += "delegate " + d.fullName + "  [" + d.iid + "]\n";
            s += DumpMethod(d.invoke, "  ");
            s += "\n";
        }
        for (const auto& rc : model.runtimeClasses)
        {
            s += "runtimeclass " + rc.fullName + (rc.activatable ? " [activatable]" : "") + "\n";
            if (!rc.defaultInterface.empty()) s += "  default " + rc.defaultInterface + "\n";
            for (const auto& iface : rc.interfaces) if (iface != rc.defaultInterface) s += "  implements " + iface + "\n";
            for (const auto& f : rc.factoryInterfaces) s += "  factory " + f + "\n";
            for (const auto& st : rc.staticInterfaces) s += "  static " + st + "\n";
            s += "\n";
        }
        return s;
    }
}
