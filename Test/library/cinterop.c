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
