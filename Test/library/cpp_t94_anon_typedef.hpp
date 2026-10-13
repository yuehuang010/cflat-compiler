// T94: anonymous struct / union / enum typedefs inside a namespace or a class. Clang names the
// record by its typedef name for linkage, so `t94a::TS` is a complete record type.
namespace t94a { typedef struct { int v; } TS; }
namespace t94b { namespace inner { typedef struct { int v; } NTS; } }
namespace t94c { typedef union { int v; float f; } TU; }
namespace t94d { typedef enum { EA = 3, EB } TE; }
struct t94o { typedef struct { int v; } I; };
