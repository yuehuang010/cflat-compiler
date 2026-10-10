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
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clang
{
    class CompilerInstance;
    class TranslationUnitDecl;
    class Decl;
    class FunctionDecl;
    class CXXRecordDecl;
    class Preprocessor;
}

namespace llvm
{
    class LLVMContext;
    class Module;
}

namespace cflat_cinterop
{
    // Tail of the refusal for a C++ variable read by value whose initializer is not a constant
    // initialization: folding would skip the initializer, and no live binding exists on this path.
    inline constexpr const char* kCxxNotConstantVariableRefusal =
        "is not initialized by a constant expression, so CFlat cannot read it by value";

    // CFLAT_CXX_EAGER_BODIES selects a different C++ group parse, so it is part of cache identity.
    inline bool CxxEagerBodies() { return std::getenv("CFLAT_CXX_EAGER_BODIES") != nullptr; }

    // One CFlat identity for every canonical C++ spelling used by extraction and backend lookup.
    std::string CxxForeignIdentity(const std::string& spelling);
    // A requested type named by the member call that returns it, because its own name is
    // private to a class (a range view's nested __iterator).
    inline constexpr std::string_view kCxxAccessFreeSpellingPrefix = "__remove_cvref(decltype(";
    inline bool IsCxxAccessFreeSpelling(const std::string& spelling)
    {
        return spelling.starts_with(kCxxAccessFreeSpellingPrefix);
    }
    // Rewrites every class nested in a specialization (`A<...>::B`, `A<...>::B<...>`) in
    // generated source to an alias declared through an explicit instantiation, where access
    // is not checked ([temp.spec.general]). Access semantics of everything else are unchanged.
    std::string AliasCxxNestedSpecializationNames(const std::string& source);

    // Split a canonical std::function<R(P...)> spelling into its return and parameter spellings.
    bool SplitStdFunctionSpelling(const std::string& spelling, std::string& ret,
                                  std::string& params);

    // True for a virtual member that cflat can only reach through a clang-emitted thunk. The
    // definition in CClangExtract.cpp carries the reasoning for exactly which members those are.
    struct RawCxxMember;
    bool CxxMemberNeedsVirtualThunk(const RawCxxMember& m);

    /*
     * Name of the extern "C" THUNK that stands in for such a member. Its body is an ordinary C++
     * virtual call, so Clang emits the vftable load, the vbtable adjustment and the return
     * adjustment; cflat only calls the symbol. Keyed on the member's mangled name so the
     * request-source builder and the extractor derive the same name independently.
     */
    inline std::string CxxVirtualThunkName(const std::string& linkageName)
    {
        std::string out = "__cflat_vthk_";
        for (char c : linkageName)
            out += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || c == '_' ? c : '_';
        return out;
    }

    struct RawRecord;

    // True for a constructor cflat can only reach through a clang-emitted placement-new thunk:
    // its class has VIRTUAL BASES, so the real symbol takes an implicit most-derived argument.
    bool CxxCtorNeedsVbaseThunk(const RawRecord& rec, const RawCxxMember& m);

    inline constexpr std::string_view kCxxVbaseCtorThunkPrefix = "__cflat_vctor_";

    // Declared after the header in every stub; it lands outside file scope iff the header
    // leaves a namespace or brace scope open.
    inline constexpr char kHeaderScopeSentinel[] = "__cflat_header_scope_sentinel";

    /*
     * Name of the extern "C" THUNK that constructs such a class. Its body is `::new (p) T(args)`,
     * so Clang owns the implicit most-derived flag / VTT argument; cflat only calls the symbol.
     * Keyed on the constructor's mangled name, so the request-source builder and the extractor
     * derive the same name independently.
     */
    inline std::string CxxVbaseCtorThunkName(const std::string& linkageName)
    {
        std::string out(kCxxVbaseCtorThunkPrefix);
        for (char c : linkageName)
            out += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || c == '_' ? c : '_';
        return out;
    }

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
        bool sretAfterThis = false;    // MS ABI: an instance method's sret slot follows `this`
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

    // Ref-qualifier on a non-static C++ member function. The integer values are kept in the
    // clang-independent extraction record and in the backend caches.
    enum CxxRefQualifier { CxxRefQualifierNone = 0, CxxRefQualifierLValue = 1,
                           CxxRefQualifierRValue = 2 };

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
        // Generated wrappers only: per parameter, the arithmetic type clang converts it into
        // for a `const S&` of the selected callee ("" if none; empty if no parameter is).
        std::vector<std::string> paramTemporaryTypes;
        // Generated wrappers only: the wrapper materializes a brace list and its result can
        // carry an address, so it may refer into the dead list (WrapperResultBorrowsBraceList).
        bool resultBorrowsBraceList = false;
        // With resultBorrowsBraceList: each materialized list's element type, in source order.
        std::vector<std::string> braceListElementTypes;
        // Generated wrappers only: the callee that receives a literal's decayed pointer temporary
        // or a reference-to-pointer parameter (WrapperLiteralPointerTemporaries).
        std::string calleeIdentity;
        bool variadic = false;
        bool isCxx = false;
        bool isInline = false;
        bool isStaticInline = false;
        bool isNoexcept = false;
        std::string bindRefusal;
        // The clang diagnostic lines behind a refusal, when one is known (never displayed).
        std::string refusalCause;
        // Clang's ABI arrangement for this declaration. Filled only in cxxMode.
        RawAbi abi;
        std::string file;
        std::string physicalFile;  // real file if `#line` / a macro expansion differs from `file`
        int line = 1;
        int col = 0;
    };

    // A callable C++ function template that can be instantiated from CFlat argument types.
    // A non-defaulted non-type parameter is published only when it is integral or an enumeration,
    // so CFlat can spell it as an integer literal; template-template parameters, non-type packs
    // and non-integral non-type parameters are intentionally not published.
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
        std::vector<std::string> parameterTypes;
        std::vector<std::string> parameterNames;
        std::vector<uint8_t> forwardingReferenceParameters;
        std::vector<unsigned> forwardingReferenceTemplateParameterIndices;
        // One char per template parameter, in order: 'T' type, 'P' type pack,
        // 'N' non-defaulted integral non-type, 'd' defaulted non-type (SFINAE helper).
        std::string templateParameterKinds;
        bool hasParameterPack = false;
        bool isConst = false;
        bool isNoexcept = false;
        int access = 0; // AccessPublic
        std::string file;
        std::string physicalFile;  // real file if `#line` / a macro expansion differs from `file`
        int line = 1;
        int col = 0;
    };

    struct RawEnum
    {
        std::string name;
        std::string enumType;
        std::string underlyingType;
        std::string promotedType;   // integral promotion target of an unscoped enum
        bool isScoped = false;
        long long value = 0;
        std::string file;
        std::string physicalFile;  // real file if `#line` / a macro expansion differs from `file`
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
        bool isPromoted = false;
        bool isZeroSize = false;     // C++ [[no_unique_address]] field has no storage in this record
        bool isConst = false;        // the field itself is const-qualified
        bool isMutable = false;      // C++ mutable field; ignores constness of its parent
        unsigned bitWidth = 0;
        uint64_t offsetBytes = 0;
        // Clang's size and alignment of the field type in bytes (0 for a bitfield). Lets the
        // registrar embed a field whose TYPE it cannot map as an opaque, correctly sized blob.
        uint64_t sizeBytes = 0;
        uint64_t alignBytes = 0;
        uint64_t bitOffset = 0;     // bitfield only: Clang's absolute bit offset in the record
        int access = AccessPublic;  // C++ only; C records are all public
    };

    // A constructor TEMPLATE, as declared: the template head and the parameter types as
    // written (dependent), so a clang-resolved overload mirror can redeclare it faithfully.
    struct RawCxxCtorTemplate
    {
        std::string head;                  // "template <class T, unsigned long N>"
        std::vector<std::string> paramTypes;
        std::vector<uint8_t> defaulted;    // aligned with paramTypes
        bool variadic = false;
        bool isDeleted = false;
        bool isExplicit = false;
        int access = 0;                    // RawAccess
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
    bool isVolatile = false;       // volatile-qualified instance method
        int refQualifier = CxxRefQualifierNone;
        bool isVirtual = false;
        bool isNoexcept = false;
        bool isDeleted = false;
        bool isDefaulted = false;
        bool isImplicit = false;
        bool isTemplateSpecialization = false;
        // No CALLABLE definition is available: the member is implicit, defaulted, or inline, and
        // Clang did not emit a body for it into the companion module. When definition emission
        // (ExtractRequest::emitDefinitions) does produce the body, this is cleared - the symbol
        // then exists in the companion bitcode the backend links in.
        bool needsLocalDefinition = false;
        // needsLocalDefinition was cleared only because the extraction ASSUMED an inline body
        // (assumeInlineDefinitions): no module carries it until a definition request emits it.
        bool definitionAssumed = false;
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
        /*
         * Set with an incomplete-by-value bindRefusal when that type is a member class (or member
         * class template specialization) clang has a definition to instantiate on demand (MSVC
         * bitset<N>::reference): its clang spelling, so the use site can request just it.
         */
        std::string lazyNestedSpelling;
        // The clang diagnostic lines behind a refusal, when one is known (never displayed).
        std::string refusalCause;
        // Copy / move constructor and copy / move assignment recognition, so the backend can
        // bind `T y = x;`, `y = x;` and `T y = move x;` without re-deriving it from the params.
        bool isCopyCtor = false;
        bool isMoveCtor = false;
        bool isDefaultCtor = false;
        bool isCopyAssign = false;
        bool isMoveAssign = false;
        bool isPureVirtual = false;
        bool isOverride = false;
        bool isFinal = false;
        // A CXXConversionDecl (`operator int`, `explicit operator double`, `operator bool`).
        // Its CFlat registration name is "operator <CFlat spelling of retType>", which only the
        // backend's C-to-CFlat type map can produce, so the flag - not the name - travels here.
        bool isConversion = false;
        // Declared `explicit`, on a CONSTRUCTOR or a CONVERSION operator alike. Neither is a
        // candidate for the ONE implicit user-defined conversion C++ allows; both stay
        // reachable spelled out - a ctor as `T(x)`, a conversion at a cast or in a condition.
        bool isExplicit = false;
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
        int64_t vtableOffsetBytes = 0;
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
        bool isFloatConstant = false;
        double floatValue = 0.0;
        int access = AccessPublic;
        std::string initializerFailure;
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
        bool isTriviallyRelocatable = false;
        bool specialMembersPending = false;
        // M4 class surface. Every flag is Clang's own answer, never derived from the member list.
        bool isPolymorphic = false;         // has a virtual function or a virtual base
        bool hasBases = false;              // any base class
        bool hasVirtualBases = false;       // virtual inheritance: rejected, the VTT is not modelled
        bool isFinal = false;               // final class: a reference to it names a complete object
        bool isAbstract = false;            // has an unoverridden pure virtual: cannot be created
        bool hasFriendOperators = false;    // declares a friend operator (found only by ADL)
        std::vector<RawCxxBase> bases;      // DIRECT bases, in declaration order
        // EVERY virtual base, direct or indirect, at its offset in THIS class's complete-object
        // layout - the only place a shared virtual base's offset is fixed.
        std::vector<RawCxxBase> virtualBases;
        // Non-empty when the C++ layout could not be flattened into a CFlat struct (virtual
        // inheritance, or a bitfield / anonymous member in a class that has bases). The record
        // stays an opaque shell and every use site reports this text.
        std::string layoutRefusal;
        bool hasTrivialDefaultCtor = false;
        bool hasTrivialCopyCtor = false;
        // Clang's implicit copy/move assignment is trivial AND not deleted: a byte copy, no symbol.
        bool hasTrivialCopyAssign = false;
        bool hasTrivialMoveAssign = false;
        bool hasTrivialDtor = true;
        // MS ABI: a by-value parameter of this class is destroyed by the CALLEE, so the caller
        // must not destroy the copy it passed (Itanium: caller-destroyed).
        bool paramDestroyedInCallee = false;
        bool hasDeletedDefaultCtor = false;
        bool hasDeletedCopyCtor = false;
        bool hasDefaultCtor = false;
        bool hasCopyCtor = false;
        // A public, non-deleted constructor TEMPLATE. Its specializations are never listed as
        // members, so only a clang-resolved `T(args)` wrapper can reach them.
        bool hasCtorTemplate = false;
        // A non-deleted `operator=` TEMPLATE: it can beat the bound copy / move assignment.
        bool hasAssignTemplate = false;
        bool isAggregate = false;
        std::vector<RawCxxCtorTemplate> ctorTemplates;
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
        std::string physicalFile;  // real file if `#line` / a macro expansion differs from `file`
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
        // The alias template's target as a STRUCTURED pattern: a dotted target base plus its
        // argument list as written, so a fixed/reordered/partial argument survives the hop.
        std::string cxxAliasTargetBase;
        std::vector<std::string> cxxAliasArgs;
        // Positional with cxxAliasParams; "" where the parameter declares no default.
        std::vector<std::string> cxxAliasParamDefaults;
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
        bool isIntegerLiteralZero = false;
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

    // A global variable a header declares (`extern int x;`) or a .c file defines (`int x = 5;`).
    // ctype is the canonical C spelling of the variable's type, consumed by the same string-based
    // mapper as RawSig param/return types. C++ namespace constexpr values carry their qualified
    // lookup name and folded integer so they can bind without a linker symbol.
    struct RawGlobalVar
    {
        std::string name;
        std::string qualifiedName;
        // Emitted symbol name. Empty for C; in C++ it is Clang's mangling, which is the only
        // name that finds a namespace-scope object in the library or companion module.
        std::string linkageName;
        std::string ctype;
        bool isConst = false;
        bool isCompileTimeConstant = false;
        int64_t constantValue = 0;
        bool isFloatConstant = false;
        double floatValue = 0.0;
        bool isCxxConstexpr = false;
        bool isInternalLinkage = false;  // C++ internal linkage: one object per import group
        uint64_t constInitHash = 0;      // internal const object: hash of its evaluated value
        // decltype request of an ENUMERATOR (ns::format_first_only, ios_base::beg): ctype names
        // its enum type, constantValue its value. Request-only; never cached as a global.
        bool isEnumerator = false;
        // MS ABI: the object is __declspec(dllimport) data (MSVC's std::cout) - reached via __imp_.
        bool isDllImport = false;
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

    struct CxxMacroProbe
    {
        std::string name;
        std::string file;
        std::string aliasTarget;
        int line = 1;
        int col = 0;
    };

    /*
     * One demand-driven companion per live C++ import group. While a chunk is harvested with
     * emitDefinitions, the extractor records here what it would have handed Clang's CodeGen,
     * and emits nothing. After CFlat code generation the group replays the record ONCE through
     * one CodeGenerator, for only the symbols the program actually uses
     * (EmitCxxDemandCompanion). Pointers stay valid while the group's Interpreter lives.
     */
    struct CxxDemandPlan
    {
        // Declarations to show CodeGen (HandleTopLevelDecl), deduplicated, in first-seen order.
        std::vector<clang::Decl*> decls;
        std::unordered_set<const clang::Decl*> declSeen;
        // Bound entities by mangled name, as opaque clang::GlobalDecl values.
        std::unordered_map<std::string, void*> bound;
        // Records whose vtable a request would have emitted, by vtable symbol name.
        std::unordered_map<std::string, const clang::CXXRecordDecl*> vtables;
        std::vector<std::string> weakPromoteSymbols;
        std::vector<std::string> weakPromotePerGroupSymbols;
        std::string cxxImportGroupKey;
        // Retry-renamed generated wrappers: program-visible name -> name clang emits.
        std::unordered_map<std::string, std::string> renamed;
        unsigned recordedChunks = 0;
        // Set by the group for its demand pass: parse a body the header parse skipped (and the
        // skipped bodies it names). False when there is none or it does not compile.
        std::function<bool(clang::FunctionDecl*)> materializeBody;
        // Same, for everything CodeGen can reach from the demanded functions (one walk up front).
        std::function<void(std::vector<const clang::FunctionDecl*>, std::string&, std::string&)>
            materializeReachable;

        void Add(clang::Decl* decl)
        {
            if (decl != nullptr && declSeen.insert(decl).second) decls.push_back(decl);
        }
    };

    struct CxxDemandStats
    {
        unsigned demanded = 0;       // program symbols this group can define
        unsigned definitions = 0;    // definitions in the emitted module
        unsigned unresolved = 0;     // demanded symbols the module still only declares
    };

    struct ExtractRequest
    {
        // Called immediately before a full frontend parse that includes the C++ header group.
        // Macro prepasses and incremental request chunks do not invoke it.
        std::function<bool(const char*)> cxxHeaderParseGuard;
        // Virtual main-file name for the in-memory stub (e.g. "cflat_hdr_stub.c"). When
        // `source` is empty, `realPath` names a real .c file on disk to parse instead.
        std::string mainFileName;
        std::string source;
        std::string realPath;

        // Driver args (target, -I, -D, ...) - same list BuildLibclangArgs produced for libclang.
        std::vector<std::string> args;

        bool wantMacros = false;            // header-bind path harvests macros; .c path does not
        std::vector<CxxMacroProbe> cxxMacroProbes;
        bool requireInScope = false;        // keep only decls whose file is under inScopeDirs
        std::vector<std::string> inScopeDirs;
        bool checkHeaderScope = false;      // reject a stub sentinel left inside a header scope
        std::string scopeHeaderPath;        // fallback path for a missing sentinel
        bool definitionsOnly = false;       // .c auto-extern: only functions defined in this TU
        bool wantIncludes = false;          // deep header-cache: record every transitively included file
        bool skipFunctionBodies = false;    // header bind: parse declarations only, skip function bodies
        // C inline bodies: emit referenced C99 inline definitions (available_externally, as
        // clang does above -O0) without running LLVM passes, so one parse covers transitive demand.
        bool emitReferencedInlineDefinitions = false;
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
        // Record the demand plan while keeping import-time body emission disabled.
        bool demandOnlyDefinitions = false;
        bool RecordsDefinitionDemand() const { return emitDefinitions || demandOnlyDefinitions; }
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
        std::string cxxRequestMarkerPrefix = "__cflat_req_";
        // Original generated prefix source, retained with demand chunks for cache replay.
        std::string demandPrefixSource;
        // Byte offset in the parsed source where the prefix (less what this group has already
        // seen) starts; npos when the source does not follow that layout.
        size_t demandPrefixOffset = std::string::npos;
        // Appended to every virtual / vbase-constructor thunk name. An incremental group's chunks
        // share one scope, so a thunk that a later request repeats needs a name of its own.
        std::string cxxThunkSuffix;
        // Stable identity of the C++ import TU, shared by every request and cache replay.
        std::string cxxImportGroupKey;
        // Request mode for a generated deduction wrapper. Only these ordinary free wrapper
        // declarations are exported; the included header remains available to CodeGen.
        std::vector<std::string> cxxFunctionWrapperNames;
        bool cxxWrapperBatch = false;
        // A live Interpreter's POISONED bodies: specializations emptied because their instantiation
        // failed in an earlier chunk, with the reason. A body reaching one is refused, not bound.
        std::unordered_map<const clang::FunctionDecl*, std::string>* poisonedFunctions = nullptr;
        // Per failed instantiation, the clang error group that named it (error plus notes).
        const std::unordered_map<const clang::FunctionDecl*, std::string>* errorCauses = nullptr;
        // A live group's demand plan: with emitDefinitions set, CodeGen work is recorded here
        // instead of emitted, and ExtractResult::bitcode stays empty (see CxxDemandPlan).
        CxxDemandPlan* demandPlan = nullptr;
        // A live group's free-operator candidate index, keyed by header TU root. Owned by the
        // group so its Decl pointers die with the Interpreter; null = index per call, no cache.
        std::unordered_map<const clang::Decl*, std::vector<clang::Decl*>>* operatorIndex = nullptr;
        // Header extraction may need one retry after forcing a named specialization complete.
        bool autoInstantiateCxxTypes = true;
        /*
         * R1 demand-only special members. A live group's header harvest leaves the implicit /
         * defaulted special members and inline-vtable virtuals of its records undefined and flags
         * each record that has such work (RawRecord::specialMembersPending). A completion request
         * (this flag on a type request) defines them for its marker records - and the in-scope
         * bases and by-value fields those reach - before the members are harvested.
         */
        bool completeCxxSpecialMembers = false;
        // A live group's records with deferred special members, by CFlat name: filled by the
        // header harvest, read by a completion request (whose markers are placeholders, so a
        // private nested record completes too).
        std::unordered_map<std::string, const clang::CXXRecordDecl*>* specialMemberRecords = nullptr;
        /*
         * Destination for a PCH built from `source` (an include-only prologue) instead of an
         * extraction. The caller supplies driver args selecting `-x c++-header`; the output path
         * and the precompile action are set on the invocation directly, because createInvocation
         * reduces the driver job to a parse and keeps neither. Every later request TU for the
         * same group passes `-include-pch <path>` and drops the includes from its own source, so
         * the group's headers are parsed once rather than once per request.
         */
        std::string pchOutputPath;
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
        std::vector<std::string> classTemplateNames;
        std::vector<std::string> classTemplateSpecializations;
        std::vector<RawEnum> enums;
        std::vector<RawRecord> records;
        std::vector<RawTypedef> typedefs;
        std::vector<RawFunctionPointerAbi> functionPointerAbis;
        std::vector<RawGlobalVar> globals;
        std::vector<RawMacro> macros;
        std::vector<RawFuncMacro> funcMacros;
        std::vector<CxxMacroProbe> macroProbes;
        bool headerScopeOpen = false;  // C++ macro prepass: the stub's scope sentinel is inside a brace
        std::vector<std::string> includedFiles;  // populated only when req.wantIncludes
        // Namespace-scope C++ using-directives, as CFlat dotted namespace pairs. The backend
        // resolves these at lookup time instead of duplicating every nominated declaration.
        std::vector<std::pair<std::string, std::string>> usingDirectives;
        // Namespace aliases (`namespace a = b::c;`), as CFlat dotted (alias, target) pairs. The
        // backend registers them as namespace aliases so a use through the alias resolves.
        std::vector<std::pair<std::string, std::string>> namespaceAliases;
        // Namespace-scope `using ns::C;` of a class or class template, as CFlat dotted (alias,
        // target) pairs. Not bound; the backend only checks them for cross-import conflicts.
        std::vector<std::pair<std::string, std::string>> classUsings;
        // Names of generated default-argument wrappers whose declarations or bodies carried
        // parse/Sema errors and were therefore withheld from CodeGen.
        std::vector<std::string> droppedCxxDefaultWrappers;
        // Mangled names of internal-linkage namespace-scope objects whose storage the companion
        // module emits; promoted to weak_odr after codegen so the program module can bind them.
        std::vector<std::string> weakPromoteSymbols;
        // Mutable statics keep a per-import-group name when their local linkage is promoted.
        std::vector<std::string> weakPromotePerGroupSymbols;
        // Internal-linkage (static) inline functions the program binds: (mangled, program name).
        // The program name folds the body's ODR hash, so distinct bodies never merge.
        std::vector<std::pair<std::string, std::string>> localInlineAliases;

        // Companion module produced when req.emitDefinitions is set: raw LLVM bitcode bytes
        // holding the C++ definitions Clang emitted for the bound surface (linkonce_odr inline
        // bodies, vtables/RTTI with their COMDATs, guard variables, static initializers). Empty
        // when nothing needed emitting. Plain bytes, so it round-trips through the disk cache.
        std::string bitcode;
        unsigned emittedDefinitions = 0;   // number of definitions in `bitcode`, for -v
        // Definitions were recorded into a group demand plan instead of emitted into `bitcode`.
        bool demandRecorded = false;
        struct DemandReplayChunk
        {
            uint64_t order = 0;
            // With prefixOffset set, `source` holds the WHOLE prefix at that offset: the parsing
            // group strips what it has seen, which differs between the storing and replaying
            // compile (a replay of a subset of one compile's chunks lacks earlier declarations).
            std::string source;
            std::string prefixSource;
            size_t prefixOffset = std::string::npos;
            std::vector<ExtractRequest::CxxTypeRequest> typeRequests;
            std::string markerPrefix;
            std::string thunkSuffix;
            std::vector<std::string> wrapperNames;
            std::vector<std::string> inScopeDirs;
            std::vector<CxxMacroProbe> macroProbes;
            // Failed helper bodies from this request must keep their original replay verdict.
            std::vector<std::pair<std::string, std::string>> poisonedBodies;
            std::vector<std::string> noDeferBodyKeys;
            std::string scopeHeaderPath;
            bool headerHarvest = false;
            bool wantMacros = false;
            bool requireInScope = false;
            bool checkHeaderScope = false;
            bool wrapperBatch = false;
            bool autoInstantiate = true;
            bool completeSpecialMembers = false;
            // Chunks parsed right after this one by the same step (a header harvest's
            // default-argument wrapper batch); stored with it, replayed after it.
            std::vector<DemandReplayChunk> follow;
        };
        DemandReplayChunk demandReplayChunk;

        // Count of "unknown type name" errors raised inside an #included header (not the
        // in-memory stub itself). This is the signature of a non-self-contained header that
        // relies on a prerequisite being included first (e.g. tlhelp32.h needs windows.h):
        // the missing base type (HANDLE/DWORD/...) makes clang drop the dependent function
        // declarations. The header-bind path uses this to fail loudly and suggest grouping
        // the prerequisite, instead of silently registering the error-recovered remnants.
        // Stub-local errors (the intentional macro-probe "type name where an expression was
        // expected" diagnostics) are excluded by source location, so they do not inflate it.
        // A structural sentinel adds one error when the stub itself is left inside a header scope.
        unsigned prereqErrors = 0;
        std::string firstPrereqError;            // formatted text of the first such error
        std::string firstError;                  // first clang error, including wrapper requests
        // A requested template specialization whose completion failed. This is separate from
        // firstError because request TUs may contain tolerated errors unrelated to this type.
        std::string invalidCxxTypeRequestError;
        std::vector<std::string> incompleteCxxTypeSpellings;

        // Errors clang raised INSIDE one of the headers the caller asked to bind (in-scope
        // dirs only, so neither the in-memory stub's intentional macro-probe/wrapper errors nor
        // a collateral system-header instantiation failure counts). Such an error poisons the
        // whole bind: Sema marks the declarations invalid and companion CodeGen would then walk
        // an AST clang's own driver would never have handed it. The header-bind path reports the
        // diagnostic and refuses. Counted from a capped error list, so this is a lower bound.
        unsigned headerErrors = 0;
        std::string firstHeaderError;            // "message at file:line" for the first such error
    };

    // Attach the header-only macro collector to a live incremental preprocessor. The returned
    // closure disables collection after the initial header chunk has been parsed.
    std::function<void()> AttachCxxMacroPrepass(clang::Preprocessor& pp,
                                                const ExtractRequest& req,
                                                ExtractResult& out);

    // Parse the TU once and fill `out`. Returns false only on a hard failure to build a TU;
    // a TU produced with diagnostics still returns true (per-decl error recovery, like the
    // old -ferror-limit=0 path). `err` carries a human-readable reason on hard failure.
    bool ExtractCInterop(const ExtractRequest& req, ExtractResult& out, std::string& err);
    bool EmitCInlineBodies(const std::vector<std::string>& args,
                          const std::vector<std::string>& headers,
                          const std::vector<std::pair<std::string, bool>>& functions,
                          std::string& bitcode, std::string& err);

    bool ExtractCxxMacroPrepass(const ExtractRequest& req, ExtractResult& out, std::string& err);

    // Harvest one incremental PTU with the same visitor and ABI/codegen pipeline as a full TU.
    bool ExtractCxxIncremental(const ExtractRequest& req, clang::CompilerInstance& ci,
                               clang::TranslationUnitDecl* root,
                               clang::TranslationUnitDecl* headerRoot,
                               const std::vector<clang::TranslationUnitDecl*>& extraRoots,
                               llvm::Module* module,
                               ExtractResult& out, std::string& err,
                               bool checkHeader = false,
                               clang::TranslationUnitDecl* preludeRoot = nullptr,
                               const std::vector<clang::Decl*>* announcedDecls = nullptr);

    /*
     * The group's single demand CodeGen pass: replay `plan` through one CodeGenerator, request
     * exactly the recorded entities named in `demand` (the program's used-but-undefined
     * symbols), and return the finalized module as bitcode. Empty bitcode with true means the
     * program needs nothing from this group. With `targetContext`, CodeGen runs in that context
     * and the module itself is handed back through `moduleOut` as well, so the caller can link
     * it without reading the bitcode back.
     */
    bool EmitCxxDemandCompanion(clang::CompilerInstance& ci, CxxDemandPlan& plan,
                                const std::vector<std::string>& demand,
                                const std::unordered_map<const clang::FunctionDecl*, std::string>* poisoned,
                                bool verbose, std::string& bitcode, CxxDemandStats& stats,
                                std::string& err, llvm::LLVMContext* targetContext = nullptr,
                                std::unique_ptr<llvm::Module>* moduleOut = nullptr);
}
