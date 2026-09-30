/* C typedefs / enums spelled like CFlat keyword primitives: GetType must resolve them, not the builtin. */
typedef unsigned char u8;
typedef struct KwPt { int x; int y; } i16;
typedef enum { KW_RED = 200, KW_GREEN = 7 } u16;
enum KwBig { KW_BA = 0x7fffffff, KW_BB = -3, KW_BC = 5000000000 };
enum KwSmall { KW_SA = 1, KW_SB = 250 };
enum { KW_ANON1 = 11, KW_ANON2 = 12 };
typedef int i32;
