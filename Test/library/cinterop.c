/* Real C (clang-compilable) fixture for the C-interop test.
   Compiled by clang-cl when imported from a .cb; the object is linked by lld. */

/* Externally-linkable global - the .c auto-extern path binds this as a
   declaration-only, mutable CFlat global. (c_handle_payload below is static,
   so it must NOT be bound - validates the internal-linkage skip.) */
int c_global_counter = 1000;

int c_add(int a, int b)
{
    return a + b;
}

int c_square(int x)
{
    return x * x;
}

/* Typedef chain regression: HANDLE -> void* must be chased by the auto-extern
   path so both the return type and parameter type register without being
   skipped as "unsupported". The body returns &c_handle_payload so the cflat
   side can round-trip identity through an opaque pointer. */
typedef void* C_HANDLE;
typedef long long C_LONG_PTR;

static int c_handle_payload = 0x5A5A5A5A;

C_HANDLE c_get_handle(void)
{
    return &c_handle_payload;
}

int c_handle_load(C_HANDLE h)
{
    return *(int*)h;
}

/* Nested typedef: SCK -> C_HANDLE -> void*. Mapper must follow >1 hop. */
typedef C_HANDLE SCK;

int c_sck_load(SCK s)
{
    return *(int*)s;
}

/* Typedef'd integer alias used in the signature - mapper must resolve through
   the chain to "long long" so the parameter lowers as i64, not gets dropped. */
C_LONG_PTR c_lp_double(C_LONG_PTR x)
{
    return x + x;
}

/* Returns (C_HANDLE)(C_LONG_PTR)-1 - the same bit pattern as the sentinel
   macro CB_INVALID_HANDLE declared in c_macro_helpers.h, used by the cflat
   side to verify the macro registers as a pointer (not an i64 constant). */
C_HANDLE c_get_invalid_handle(void)
{
    return (C_HANDLE)(C_LONG_PTR)-1;
}

/* A mini COM-style vtable: a struct of function pointers plus an object whose first
   field points at it. Exercises CFlat's C function-pointer struct fields (mapped to a
   thin function<>, so they are callable) and typed struct-pointer fields (so
   obj.vtbl->fn(&obj) dispatches with no hand-written cast - the same shape a real COM
   lpVtbl has). No allocation: the caller supplies the object by value. */
struct CbObj;
typedef struct CbVtbl {
    int (*add)(struct CbObj*, int, int);
    int (*get)(struct CbObj*);
} CbVtbl;
typedef struct CbObj {
    const CbVtbl* vtbl;
    int value;
} CbObj;

static int cb_vt_add(struct CbObj* self, int a, int b) { self->value += a + b; return self->value; }
static int cb_vt_get(struct CbObj* self) { return self->value; }
static const CbVtbl cb_global_vtbl = { cb_vt_add, cb_vt_get };

void cb_init_obj(CbObj* obj, int start)
{
    obj->vtbl = &cb_global_vtbl;
    obj->value = start;
}

/* Targets for the alias-macro legs in c_macro_helpers.h (Section U of
   test_c_interop.cb). Kept trivial: the legs assert the alias reaches THIS body. */
int c_triple(int x) { return x * 3; }
int CAA_GetObjectW(int value) { return 700 + value; }
int CAA_GetObject(int value) { return 400 + value; }
int CAA_GetReverse(int value) { return 500 + value; }
int CAA_GetReverseW(int value) { return 900 + value; }
int CAA_GetGroupW(int value) { return 1100 + value; }
int CAA_GetGroup(int value) { return 800 + value; }
int CAA_GetGroupRealFirst(int value) { return 1000 + value; }
int CAA_GetGroupRealFirstW(int value) { return 1200 + value; }
int CAA_LateTarget(int value) { return 1300 + value; }
int CAA_LateChainTarget(int value) { return 1400 + value; }
int CAA_GroupLateTarget(int value) { return 1500 + value; }
int CAA_DiffTarget(double value) { return 1600 + (int)value; }
int c_quad(int x)   { return x * 4; }
int c_alias_victim(int x) { return x * 10; }
int c_negate(int x)       { return -x; }
