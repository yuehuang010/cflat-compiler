// C-interop extraction via the clang C++ API (single parse).
//
// This header is deliberately free of any clang/LLVM types so it can be included
// by LLVMBackend.h without dragging the heavy clang C++ headers into that (already
// /bigobj) translation unit. All clang C++ usage lives in CClangExtract.cpp.
//
// The extractor parses a C translation unit ONCE and emits plain-data "spellings":
// function signatures, enum constants, records (structs/unions), typedefs, object-like
// macros (value-folded in-process), and function-like macros. The cflat backend then
// maps those spellings to its TypeAndValue/LLVM types via MapCTypeToTypeAndValue and
// feeds the existing Register*/cache machinery - none of which changes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cflat_cinterop
{
    // Split a canonical std::function<R(P...)> spelling into its return and parameter spellings.
    bool SplitStdFunctionSpelling(const std::string& spelling, std::string& ret,
                                  std::string& params);

    // One parameter's (or the result's) ABI arrangement as Clang computed it, spelled without a
    // single clang type so the backend can consume it and the caches can round-trip it. `kind`
    // mirrors clang::CodeGen::ABIArgInfo::Kind; coerceType/paddingType are LLVM IR type TEXT
    // (llvm::Type::print output, e.g. "i64", "[2 x i64]", "{ i64, i32 }", "<2 x float>").
    struct RawAbiSlot
    {
        enum Kind { Direct, Extend, Indirect, IndirectAliased, Ignore, Expand,
                    CoerceAndExpand, InAlloca, TargetSpecific, Unknown };
        int kind = Direct;
        std::string coerceType;
        std::string paddingType;
        bool signExt = false;
        bool zeroExt = false;
        bool inReg = false;
        bool canBeFlattened = false;   // Direct + struct coerce type -> one LLVM arg per element
        bool indirectByVal = false;
        bool indirectRealign = false;
        uint64_t indirectAlign = 0;
        uint64_t directOffset = 0;
        unsigned llvmArgIndex = 0;     // first LLVM argument index this slot occupies
        unsigned llvmArgCount = 1;     // number of LLVM arguments it consumes (0 for Ignore)
    };

    // Whole-function arrangement. `fnTypeText` is Clang's own llvm::FunctionType for the callee
    // (from convertFreeFunctionType); the backend compares the type it built against it and
    // refuses the declaration on any difference rather than emitting a silent ABI mismatch.
    struct RawAbi
    {
        bool valid = false;
        RawAbiSlot ret;
        std::vector<RawAbiSlot> params;
        unsigned callingConv = 0;
        std::string fnTypeText;
    };

    // ABI arrangement for the function type behind a foreign function pointer. The signature is
    // Clang's canonical function-prototype spelling, so typedefs of the same pointee share it.
    struct RawFunctionPointerAbi
    {
        std::string signature;
        std::string retType;
        std::vector<std::string> paramTypes;
        RawAbi abi;
    };

    struct RawDefaultArg
    {
        std::string kind;  // int, bool, enum, float, double, nullptr, or nonconst
        std::string value;
    };

    // A C function signature. Types are canonical C spellings (e.g. "int", "unsigned long long",
    // "struct Point *", "int (*)(int, int)") so the backend's string-based mapper consumes them
    // exactly as it did the libclang DesugaredSpelling.
    struct RawSig
    {
        std::string name;
        // CFlat spelling (C++ namespace separators are normalized to '.').
        std::string qualifiedName;
        // The target ABI linkage spelling. Empty for C declarations.
        std::string linkageName;
        std::string retType;
        std::vector<std::string> paramTypes;
        std::vector<std::string> paramNames;   // aligned with paramTypes (may be empty strings)
        std::vector<RawDefaultArg> defaultArgs;
        bool variadic = false;
        bool isCxx = false;
        bool isNoexcept = false;
        std::string bindRefusal;
        // Clang's ABI arrangement for this declaration. Filled only in cxxMode.
        RawAbi abi;
        std::string file;
        int line = 1;
        int col = 0;
    };

    // A callable C++ function template that can be instantiated from CFlat argument types.
    // Non-type parameters are allowed only when they have defaults; template-template parameters
    // and non-defaulted non-type parameters are intentionally not published.
    struct RawFunctionTemplate
    {
        enum Kind { Free = 0, StaticMember = 1, InstanceMember = 2 };
        std::string name;          // full CFlat dotted name
        std::string owner;         // CFlat record identity for a member, empty for a free function
        std::string memberName;    // simple member name, empty for a free function
        std::string cxxSpelling;   // qualified C++ spelling, e.g. ::cppt::twice
        int kind = Free;
        unsigned minArity = 0;
        unsigned maxArity = 0;
        unsigned typeParameterCount = 0;
        bool isConst = false;
        bool isNoexcept = false;
        int access = 0; // AccessPublic
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct RawEnum
    {
        std::string name;
        std::string enumType;
        std::string underlyingType;
        long long value = 0;
        std::string file;
        int line = 1;
        int col = 0;
    };

    // C++ access specifier, spelled without a clang enum. Mirrors clang::AccessSpecifier
    // order for public/protected/private; C fields are always Public.
    enum RawAccess { AccessPublic = 0, AccessProtected = 1, AccessPrivate = 2 };

    struct RawField
    {
        std::string name;
        std::string ctype;          // canonical C spelling of the field type
        bool isBitfield = false;
        unsigned bitWidth = 0;
        uint64_t offsetBytes = 0;
        // Clang's size and alignment of the field type in bytes (0 for a bitfield). Lets the
        // registrar embed a field whose TYPE it cannot map as an opaque, correctly sized blob.
        uint64_t sizeBytes = 0;
        uint64_t alignBytes = 0;
        uint64_t bitOffset = 0;     // bitfield only: Clang's absolute bit offset in the record
        int access = AccessPublic;  // C++ only; C records are all public
    };

    // One exported member function of a C++ class: an instance method, a static method, a
    // constructor, or the destructor. Structors carry Clang's Ctor_Complete / Dtor_Complete
    // linkage name; on Itanium/Darwin they also RETURN 'this', which `returnsThis` records so
    // the caller can ignore the result instead of mis-typing the callee.
    struct RawCxxMember
    {
        enum Kind { Instance = 0, StaticMethod = 1, Constructor = 2, Destructor = 3 };
        int kind = Instance;
        std::string name;              // simple source name; "__ctor" / "__dtor" for structors
        std::string linkageName;
        std::string retType;           // canonical spelling ("void" for structors)
        std::vector<std::string> paramTypes;
        std::vector<std::string> paramNames;
        std::vector<RawDefaultArg> defaultArgs; // aligned with paramTypes; entry 0 is `this`
        bool variadic = false;
        // The constructor is inherited from a base through a using-declaration, or has a
        // template parameter pack. Its concrete call shape is materialized by an on-demand
        // placement-new wrapper instead of a direct constructor symbol.
        bool requiresConstructorWrapper = false;
        bool isConst = false;          // const-qualified instance method
        bool isVirtual = false;
        bool isNoexcept = false;
        bool isDeleted = false;
        bool isDefaulted = false;
        bool isImplicit = false;
        // No CALLABLE definition is available: the member is implicit, defaulted, or inline, and
        // Clang did not emit a body for it into the companion module. When definition emission
        // (ExtractRequest::emitDefinitions) does produce the body, this is cleared - the symbol
        // then exists in the companion bitcode the backend links in.
        bool needsLocalDefinition = false;
        bool returnsThis = false;      // structor ABI hands 'this' back; the result is ignored
        /*
         * Non-empty when the member cannot be bound for a reason only the extractor can see, and
         * the text the use site should report. Today: a by-value parameter or return whose class
         * type is INCOMPLETE in this translation unit (e.g. a libc++ overload taking
         * `std::initializer_list<T>` when <initializer_list> was never included). Clang's ABI
         * classifier reads such a type's layout, so the member must be refused before the
         * arrangement is even attempted.
         */
        std::string bindRefusal;
        // Copy / move constructor and copy / move assignment recognition, so the backend can
        // bind `T y = x;`, `y = x;` and `T y = move x;` without re-deriving it from the params.
        bool isCopyCtor = false;
        bool isMoveCtor = false;
        bool isDefaultCtor = false;
        bool isCopyAssign = false;
        bool isMoveAssign = false;
        bool isPureVirtual = false;
        // A CXXConversionDecl (`operator int`, `explicit operator double`, `operator bool`).
        // Its CFlat registration name is "operator <CFlat spelling of retType>", which only the
        // backend's C-to-CFlat type map can produce, so the flag - not the name - travels here.
        bool isConversion = false;
        // A virtual override whose COVARIANT return type needs a pointer adjustment relative to
        // the overridden declaration's return type. Clang answers that with a return-adjusting
        // thunk it emits itself; cflat cannot synthesize one, so such a member is refused.
        bool covariantReturnNeedsAdjust = false;
        /*
         * M6 - Itanium vtable slot of a VIRTUAL member, from
         * ItaniumVTableContext::getMethodVTableIndex, relative to the address point of the
         * vtable of the class that DECLARES it. -1 when the member is not virtual.
         * A virtual destructor occupies TWO adjacent slots: the complete-object destructor (D1)
         * at vtableIndex and the DELETING destructor (D0), which also frees the storage, at
         * vtableIndexDeleting.
         */
        int vtableIndex = -1;
        int vtableIndexDeleting = -1;
        int access = AccessPublic;
        RawAbi abi;
        std::string file;
        int line = 1;
        int col = 0;
    };

    // A static data member with an out-of-line definition (so a real symbol exists).
    struct RawCxxStaticVar
    {
        std::string name;
        std::string ctype;
        std::string linkageName;
        bool isCompileTimeConstant = false;
        int64_t constantValue = 0;
        int access = AccessPublic;
        std::string file;
        int line = 1;
        int col = 0;
    };

    // One DIRECT base class of a C++ record, with the byte offset of its subobject inside the
    // complete object (ASTRecordLayout::getBaseClassOffset). A non-primary base of a multiply
    // inheriting class has a NON-ZERO offset, which every pointer crossing must add.
    struct RawCxxBase
    {
        std::string name;           // CFlat dotted spelling of the base class
        std::string canonicalType;  // canonical C++ spelling, including specialization arguments
        uint64_t offsetBytes = 0;
        int access = AccessPublic;
        bool isVirtual = false;
    };

    struct RawRecord
    {
        std::string name;           // tag name; empty for anonymous (caller synthesizes)
        bool isUnion = false;
        bool isCxx = false;
        bool isPacked = false;
        uint64_t sizeBytes = 0;
        uint64_t alignBytes = 0;
        bool isTrivial = false;
        // Trivially copyable is the predicate that decides whether a C++ record may cross a
        // by-value boundary as raw bytes; isTrivial additionally demands trivial default
        // construction, which the ABI does not care about.
        bool isTriviallyCopyable = false;
        // M4 class surface. Every flag is Clang's own answer, never derived from the member list.
        bool isPolymorphic = false;         // has a virtual function or a virtual base
        bool hasBases = false;              // any base class
        bool hasVirtualBases = false;       // virtual inheritance: rejected, the VTT is not modelled
        bool isAbstract = false;            // has an unoverridden pure virtual: cannot be created
        std::vector<RawCxxBase> bases;      // DIRECT bases, in declaration order
        // Non-empty when the C++ layout could not be flattened into a CFlat struct (virtual
        // inheritance, or a bitfield / anonymous member in a class that has bases). The record
        // stays an opaque shell and every use site reports this text.
        std::string layoutRefusal;
        bool hasTrivialDefaultCtor = false;
        bool hasTrivialCopyCtor = false;
        bool hasTrivialDtor = true;
        bool hasDeletedDefaultCtor = false;
        bool hasDeletedCopyCtor = false;
        bool hasDefaultCtor = false;
        bool hasCopyCtor = false;
        bool isAggregate = false;
        std::vector<RawCxxMember> members;
        std::vector<RawCxxStaticVar> staticVars;
        std::string qualifiedName;
        /*
         * cxxMode: Clang's CANONICAL spelling of the record's own type
         * (e.g. "std::__1::vector<int, std::__1::allocator<int>>"). This is the identity two CFlat
         * spellings of the same specialization agree on, and it is also how member signatures
         * spell the type, so the backend keys its foreign-spelling -> CFlat-name table on it.
         */
        std::string canonicalCtype;
        // Canonical hyphenated GUID of a header-COM interface's __declspec(uuid)/MIDL_INTERFACE
        // attribute (e.g. "db6f6ddb-ac77-4e88-8253-819df9bbf140"), or empty. Populated only by the
        // C++ uuid-harvest pass (the C parse never sees it - the SDK gates the attr on __cplusplus).
        std::string uuid;
        std::vector<RawField> fields;
        std::string file;
        int line = 1;
        int col = 0;
        // True when the record's definition is under the in-scope dirs (the bound header's own
        // dir / the --c-include roots). Records are collected regardless of scope so that an
        // in-scope struct's by-value dependency types (e.g. POINT in MSG, defined in the SDK's
        // shared/ dir) can still be registered; the backend keeps the transitive closure of
        // in-scope records and drops the rest. Always true on the requireInScope=false (.c) path.
        bool inScope = true;
    };

    // typedef Name = canonical spelling of the aliased type. Mirrors CollectCTypedefsLibclang.
    struct RawTypedef
    {
        std::string name;
        std::string qualifiedName;
        std::string underlying;
        std::string cxxSpecialization;
        bool isCxxAliasTemplate = false;
        std::string cxxAliasPattern;
        std::vector<std::string> cxxAliasParams;
        std::string file;
        int line = 1;
        int col = 0;
        bool isAnonymousRecord = false;
    };

    // An object-like macro folded in-process. Exactly one of int/float/string is meaningful,
    // selected by `kind`. naturalType carries the canonical spelling of the macro's value type
    // (e.g. "double", "char[8]", "int (*)(int, int)", "void *") so the backend can classify
    // pointer / function-pointer / float / string the same way the old __typeof__ probe did.
    struct RawMacro
    {
        enum Kind { Skip, Int, Float, String };
        std::string name;
        Kind kind = Skip;
        long long intValue = 0;
        double floatValue = 0.0;
        std::string stringValue;     // decoded characters (string kind)
        std::string naturalType;     // canonical type spelling of the folded expression
        // Object-like macro whose whole body is a single identifier (`#define A B`). Carried
        // even when the probe did not fold, so the binder can alias A onto whatever B names.
        std::string aliasTarget;
        std::string file;
        int line = 1;
        int col = 0;
    };

    // An externally-linkable global variable a header declares (`extern int x;`) or a .c file
    // defines (`int x = 5;`). ctype is the canonical C spelling of the variable's type, consumed
    // by the same string-based mapper as RawSig param/return types. Bound mutable, C-style.
    struct RawGlobalVar
    {
        std::string name;
        std::string ctype;
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct RawFuncMacro
    {
        std::string name;
        std::vector<std::string> params;
        std::string body;
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct ExtractRequest
    {
        // Virtual main-file name for the in-memory stub (e.g. "cflat_hdr_stub.c"). When
        // `source` is empty, `realPath` names a real .c file on disk to parse instead.
        std::string mainFileName;
        std::string source;
        std::string realPath;

        // Driver args (target, -I, -D, ...) - same list BuildLibclangArgs produced for libclang.
        std::vector<std::string> args;

        bool wantMacros = false;            // header-bind path harvests macros; .c path does not
        bool requireInScope = false;        // keep only decls whose file is under inScopeDirs
        std::vector<std::string> inScopeDirs;
        bool definitionsOnly = false;       // .c auto-extern: only functions defined in this TU
        bool wantIncludes = false;          // deep header-cache: record every transitively included file
        bool skipFunctionBodies = false;    // header bind: parse declarations only, skip function bodies
        bool verbose = false;               // emit extractor diagnostics and skip traces
        // C++ uuid-harvest pass: parse the header(s) as C++ and collect only record name -> uuid
        // (from __declspec(uuid)/MIDL_INTERFACE). No macros/sigs/enums are produced. The caller
        // stamps the harvested GUIDs onto the C-parse records so iidof() resolves header-COM IIDs.
        bool uuidHarvestCxx = false;
        // Parse the input as C++ and retain C++ qualified names/linkage identity.
        bool cxxMode = false;
        /*
         * M5 - run Clang CodeGen over the parsed C++ AST and emit the definitions the bound
         * surface needs (inline functions, inline methods/structors, vtables + RTTI of
         * polymorphic classes, inline static data members, plus everything they reference
         * transitively) into a companion LLVM module, returned as bitcode in
         * ExtractResult::bitcode. Requires cxxMode and function bodies (so the caller must not
         * set skipFunctionBodies). Never set in LSP analysis - CodeGen is emit-only work.
         */
        bool emitDefinitions = false;
        /*
         * Bind inline definitions as callable WITHOUT running CodeGen. Set in LSP analysis, which
         * emits no IR and links nothing: the surface it reports must match what a real compile
         * (where emitDefinitions is on) can call, or an inline method would look unknown in the
         * editor. Never set together with emitDefinitions - then the emitted module decides.
         */
        bool assumeInlineDefinitions = false;
        /*
         * M5b - concrete foreign type requests. Each entry names a C++ type by spelling
         * (`std::vector<int>`, `std::string`) and the CFlat identity to register it under. The
         * caller composes the stub source with one explicit instantiation DEFINITION plus one
         * marker typedef per request; the extractor resolves the typedef, keeps ONLY those
         * records, and gives each the requested CFlat spelling. The general declaration walk is
         * skipped in this mode - a libc++ translation unit is a catalog, not a bound surface.
         */
        struct CxxTypeRequest
        {
            std::string cxxSpelling;   // C++ source spelling, e.g. "std::vector<int>"
            std::string cflatName;     // CFlat identity to register, e.g. the mangled generic name
        };
        std::vector<CxxTypeRequest> cxxTypeRequests;
        // Request mode for a generated deduction wrapper. Only these ordinary free wrapper
        // declarations are exported; the included header remains available to CodeGen.
        std::vector<std::string> cxxFunctionWrapperNames;
        // Header extraction may need one retry after forcing a named specialization complete.
        bool autoInstantiateCxxTypes = true;
    };

    struct ExtractResult
    {
        // Target facts from the exact clang invocation that produced this AST. The backend uses
        // these for target-dependent scalar mappings and serializes them with header bindings.
        uint64_t longDoubleWidth = 0;
        bool longDoubleIsIEEEDouble = false;
        std::string targetTriple;
        std::vector<RawSig> sigs;
        std::vector<RawFunctionTemplate> functionTemplates;
        std::vector<RawEnum> enums;
        std::vector<RawRecord> records;
        std::vector<RawTypedef> typedefs;
        std::vector<RawFunctionPointerAbi> functionPointerAbis;
        std::vector<RawGlobalVar> globals;
        std::vector<RawMacro> macros;
        std::vector<RawFuncMacro> funcMacros;
        std::vector<std::string> includedFiles;  // populated only when req.wantIncludes
        // Names of generated default-argument wrappers whose declarations or bodies carried
        // parse/Sema errors and were therefore withheld from CodeGen.
        std::vector<std::string> droppedCxxDefaultWrappers;

        // Companion module produced when req.emitDefinitions is set: raw LLVM bitcode bytes
        // holding the C++ definitions Clang emitted for the bound surface (linkonce_odr inline
        // bodies, vtables/RTTI with their COMDATs, guard variables, static initializers). Empty
        // when nothing needed emitting. Plain bytes, so it round-trips through the disk cache.
        std::string bitcode;
        unsigned emittedDefinitions = 0;   // number of definitions in `bitcode`, for -v

        // Count of "unknown type name" errors raised inside an #included header (not the
        // in-memory stub itself). This is the signature of a non-self-contained header that
        // relies on a prerequisite being included first (e.g. tlhelp32.h needs windows.h):
        // the missing base type (HANDLE/DWORD/...) makes clang drop the dependent function
        // declarations. The header-bind path uses this to fail loudly and suggest grouping
        // the prerequisite, instead of silently registering the error-recovered remnants.
        // Stub-local errors (the intentional macro-probe "type name where an expression was
        // expected" diagnostics) are excluded by source location, so they do not inflate it.
        unsigned prereqErrors = 0;
        std::string firstPrereqError;            // formatted text of the first such error
        std::string firstError;                  // first clang error, including wrapper requests
    };

    // Parse the TU once and fill `out`. Returns false only on a hard failure to build a TU;
    // a TU produced with diagnostics still returns true (per-decl error recovery, like the
    // old -ferror-limit=0 path). `err` carries a human-readable reason on hard failure.
    bool ExtractCInterop(const ExtractRequest& req, ExtractResult& out, std::string& err);
}
