extern void printf(const char* argv, ...);
extern void* malloc(i64 size);
extern void free(void* ptr);

void* operator new(long long size)
{
    // custom global allocator
    return malloc(size);
}

void operator delete(void* ptr)
{
    free(ptr);
}

int Test(const char* testName, i64 actual, i64 expected)
{
    if (expected == actual)
    {
        printf("%s passed.\n", testName);
        return 1;
    }

    printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
    return 0;
}

int Test(const char* testName, int actual, int expected)
{
    if (expected == actual)
    {
        printf("%s passed.\n", testName);
        return 1;
    }

    printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
    return 0;
}

bool TestBool(const char* testName, bool actual)
{
    bool expected = true;
    if (expected == actual)
    {
        printf("%s passed.\n", testName);
        return true;
    }

    printf("%s failed expecting '%s' but got '%s'.\n", testName, expected ? "false" : "true", actual ? "false" : "true");
    return false;
}

bool testBuiltinIdentifiers()
{
    bool result = true;
    result &= TestStr("__FILE__", __FILE__, "testfile2.c");
    result &= TestStr("__FUNCTION__", __FUNCTION__, "testBuiltinIdentifiers");
    result &= Test("__LINE__", __LINE__, 58); // Remember to adjust if shifted.
    return result;
}

int testArray()
{
    int arraySize = 30;
    int[arraySize] array;

    int i = 0;
    while (i < arraySize)
    {
        array[i] = i;
        i++;
    }

    int sum = 0;
    i = 0;
    while (i < arraySize)
    {
        sum += array[i];
        i++;
    }

    return sum;
}

int myFunction(int x, int y, int z)
{
    // printf("x=%d,y=%d,z=%d: ", x, y, z);
    int pair_xy = (x + y) * (x + y + 1) / 2 + y;
    return (pair_xy + z) * (pair_xy + z + 1) / 2 + z;
}

bool testNamedParameters()
{
    bool result = true;
    int expected = myFunction(1, 2, 3);
    result &= Test("InOrder", myFunction(x:1, y : 2, z : 3), expected);
    result &= Test("OutOfOrderName", myFunction(y:2, z : 3, x : 1), expected);
    result &= Test("MixedInOrder", myFunction(x:1, y : 2, 3), expected);
    result &= Test("MixedOutOfOrder", myFunction(z:3, 1, 2), expected);

    return result;
}

// =============================================================
// Enum tests
// =============================================================

// Named enum — enumerators should be registered as globals.
enum MyEnum : u8 { E_ONE = 1, E_TWO, E_THREE = 5 };

bool testEnum()
{
    bool result = true;
    result &= Test("enum_E_ONE", MyEnum.E_ONE, 1);
    result &= Test("enum_E_TWO", MyEnum.E_TWO, 2);
    result &= Test("enum_E_THREE", MyEnum.E_THREE, 5);
    return result;
}

struct MyStruct
{
    int num1 = 1;
    int num2 = 2;
    int num3 = 3;
};

int TotalByValue(MyStruct mystruct)
{
    return mystruct.num1 + mystruct.num2 + mystruct.num2;
}

int TotalByPointer(MyStruct* mystruct)
{
    return mystruct->num1 + mystruct->num2 + mystruct->num2;
}

struct MyStruct1
{
    int num = 1;
    int Read() { return num; }
};

struct MyStruct2
{
    int num = 2;
    int Read() { return num; }
};

// Overloaded Extension function
int ReadExt(MyStruct1 my) { return my.num; }
int ReadExt(MyStruct2 my) { return my.num; }

// Destructor tests

int destructorCallCount = 0;

struct Tracked
{
    int id = 0;
    ~Tracked()
    {
        destructorCallCount = destructorCallCount + 1;
    }
};

bool testDestructor()
{
    bool result = true;
    destructorCallCount = 0;

    {
        Tracked t = Tracked();
    }
    result &= Test("destructor_called_once", destructorCallCount, 1);

    {
        Tracked a = Tracked();
        Tracked b = Tracked();
    }
    result &= Test("destructor_called_twice", destructorCallCount, 3);

    return result;
}


// Default parameter tests

int addWithDefault(int x, int y = 5)
{
    return x + y;
}

bool testDefaultSingleParam()
{
    bool result = true;
    result &= Test("add_both_args", addWithDefault(3, 4), 7);  // 3+4=7
    result &= Test("add_default_y", addWithDefault(3), 8);  // 3+5=8
    return result;
}

int sumWithDefaults(int x, int y = 10, int z = 20)
{
    return x + y + z;
}

bool testDefaultMultipleParams()
{
    bool result = true;
    result &= Test("sum_all_args", sumWithDefaults(1, 2, 3), 6);   // 1+2+3=6
    result &= Test("sum_default_z", sumWithDefaults(1, 2), 23);  // 1+2+20=23
    result &= Test("sum_defaults_yz", sumWithDefaults(1), 31);  // 1+10+20=31
    return result;
}

int multiplyWithDefault(int x, int factor = 2)
{
    return x * factor;
}

bool testDefaultExpression()
{
    bool result = true;
    result &= Test("multiply_explicit", multiplyWithDefault(5, 3), 15);  // 5*3=15
    result &= Test("multiply_default", multiplyWithDefault(5), 10);  // 5*2=10
    return result;
}

extern int strcmp(const char* a, const char* b);

bool TestStr(const char* testName, const char* actual, const char* expected)
{
    if (strcmp(actual, expected) == 0)
    {
        printf("%s passed.\n", testName);
        return true;
    }
    printf("%s failed: expected '%s' but got '%s'.\n", testName, expected, actual);
    return false;
}

struct NamedThing
{
    int value = 42;
};


struct TypedThing
{
    int count = 0;
    bool flag = false;
};

bool testTypeof()
{
    bool result = true;
    int i = 0;
    bool b = false;
    TypedThing t = TypedThing();

    result &= TestStr("typeof_int", typeof(i), "int");
    result &= TestStr("typeof_bool", typeof(b), "bool");
    result &= TestStr("typeof_struct", typeof(t), "TypedThing");
    result &= TestStr("typeof_field_int", typeof(t.count), "int");
    result &= TestStr("typeof_field_bool", typeof(t.flag), "bool");
    result &= TestStr("typeof_type_int", typeof(int), "int");
    result &= TestStr("typeof_type_name", typeof(TypedThing), "TypedThing");
    return result;
}

bool testNameof()
{
    bool result = true;
    int myVar = 0;
    NamedThing thing = NamedThing();

    result &= TestStr("nameof_var", nameof(myVar), "myVar");
    result &= TestStr("nameof_struct", nameof(thing), "thing");
    result &= TestStr("nameof_field", nameof(thing.value), "value");
    result &= TestStr("nameof_type", nameof(NamedThing), "NamedThing");
    return result;
}

// Namespace tests

int add(int a, int b) { return a + b + 100; }  // global add returns a different value

namespace MathUtils
{
    int add(int a, int b) { return a + b; }
    int multiply(int a, int b) { return a * b; }

    namespace Advanced
    {
        int square(int x) { return x * x; }
        int cube(int x) { return x * x * x; }
    }
}

using Math = MathUtils;          // global alias for top-level namespace
using MathAdv = MathUtils.Advanced;  // global alias for nested namespace

bool testNamespace()
{
    bool result = true;
    result &= Test("namespace_add", MathUtils.add(3, 4), 7);            // 3+4=7
    result &= Test("namespace_multiply", MathUtils.multiply(3, 4), 12); // 3*4=12
    result &= Test("global_add", add(3, 4), 107);                       // 3+4+100=107
    result &= Test("no_collision", MathUtils.add(3, 4) != add(3, 4), 1);
    result &= Test("global_using_add", Math.add(3, 4), 7);              // global alias resolves to MathUtils.add
    result &= Test("global_using_multiply", Math.multiply(3, 4), 12);   // global alias resolves to MathUtils.multiply
    return result;
}

bool testLocalUsing()
{
    using M = MathUtils;  // local alias, only visible in this function
    bool result = true;
    result &= Test("local_using_add", M.add(2, 3), 5);
    result &= Test("local_using_multiply", M.multiply(2, 3), 6);
    return result;
}

bool testNestedNamespace()
{
    bool result = true;
    result &= Test("nested_square", MathUtils.Advanced.square(4), 16);    // 4*4=16
    result &= Test("nested_cube", MathUtils.Advanced.cube(3), 27);        // 3*3*3=27

    using Adv = MathUtils.Advanced;  // local alias for nested namespace
    result &= Test("nested_using_square", Adv.square(5), 25);             // 5*5=25
    result &= Test("nested_using_cube", Adv.cube(2), 8);                  // 2*2*2=8
    return result;
}

bool testNestedUsing()
{
    bool result = true;

    // Global alias for nested namespace
    result &= Test("global_nested_using_square", MathAdv.square(4), 16);  // 4*4=16
    result &= Test("global_nested_using_cube", MathAdv.cube(3), 27);      // 3*3*3=27

    // Local alias for nested namespace shadows nothing; verify it works
    using Adv2 = MathUtils.Advanced;
    result &= Test("local_nested_using_square", Adv2.square(6), 36);      // 6*6=36
    result &= Test("local_nested_using_cube", Adv2.cube(4), 64);          // 4*4*4=64
    return result;
}

bool testLocalUsingScoped()
{
    // 'M' from testLocalUsing is not visible here; only 'Math' global alias is.
    bool result = true;
    result &= Test("local_alias_not_leaked", Math.add(1, 2), 3);
    return result;
}

// =============================================================
// Forward reference tests
// =============================================================

// Test 1: bar() calls foo() which is defined after bar().
int bar()
{
    return foo();
}

int foo()
{
    return 42;
}

bool testForwardFunction()
{
    bool result = true;
    result &= Test("bar calls foo (fwd ref)", bar(), 42);
    return result;
}

// Test 2: Mutual recursion — no explicit forward declaration needed.
bool isEven(int n)
{
    if (n == 0) return true;
    return isOdd(n - 1);
}

bool isOdd(int n)
{
    if (n == 0) return false;
    return isEven(n - 1);
}

bool testMutualRecursion()
{
    bool result = true;
    result &= Test("isEven(0)", isEven(0), 1);
    result &= Test("isEven(4)", isEven(4), 1);
    result &= Test("isOdd(1)",  isOdd(1),  1);
    result &= Test("isOdd(5)",  isOdd(5),  1);
    result &= Test("isEven(3) is false", isEven(3), 0);
    result &= Test("isOdd(4) is false",  isOdd(4),  0);
    return result;
}

// Test 3: runAccumulator() calls getTotal() which is defined after the struct.
int runAccumulator()
{
    return getTotal();
}

struct Accumulator
{
    int x = 10;
    int y = 20;
    int z = 30;

    int Total()
    {
        return x + y + z;
    }
};

int getTotal()
{
    Accumulator a = Accumulator();
    return a.Total();
}

bool testForwardFunctionWithStruct()
{
    bool result = true;
    result &= Test("runAccumulator (fwd ref to getTotal)", runAccumulator(), 60);
    return result;
}

// Test 4: Member function calls a member function defined later in the struct.
struct Calculator
{
    int value = 10;

    int ComputeDouble()
    {
        return doubleIt();
    }

    int ComputeSum()
    {
        return addThree(5, 3);
    }

    int doubleIt()
    {
        return value * 2;
    }

    int addThree(int b, int c)
    {
        return value + b + c;
    }
};

bool testForwardInStruct()
{
    bool result = true;
    Calculator c = Calculator();
    result &= Test("member calls fwd func (doubleIt)", c.ComputeDouble(), 20);
    result &= Test("member calls fwd func (addThree)", c.ComputeSum(), 18);
    return result;
}

// Return block: the function body is inlined at every call site.
// 'return' inside the block returns from the caller.
inline int doubleValue(int x)
{
    return { return x * 2; };
}

inline int addValues(int a, int b)
{
    return { return a + b; };
}

// Wrappers: the return-block is inlined here; its 'return' returns from these functions.
int getDoubled(int x)
{
    doubleValue(x);
}

int getSum(int a, int b)
{
    addValues(a, b);
}

bool testReturnBlock()
{
    bool result = true;
    result &= Test("returnBlock doubleValue(5)", getDoubled(5), 10);
    result &= Test("returnBlock addValues(3,4)", getSum(3, 4), 7);
    return result;
}

// Assert uses a return block: if the assertion fails, it returns false from the caller.
inline bool Assert(const char* testName, int actual, int expected)
{
    return {
        if (actual != expected)
        {
            printf("%s failed: expected '%d' but got '%d'.\n", testName, expected, actual);
            return false;
        }
    };
}

// When the failing Assert is hit, it exits this function early with false.
// The 'return true' at the end is only reached if all asserts pass.
bool testAssertPass()
{
    Assert("assert 42==42", 42, 42);
    Assert("assert 7==7", 7, 7);
    return true;
}

bool testAssertFail()
{
    Assert("assert 10==10", 10, 10);
    Assert("assert 10==99", 10, 99);  // fails — exits testAssertFail with false
    return true;
}

bool testAssertReturnBlock()
{
    bool result = true;
    result &= TestBool("assert_all_pass", testAssertPass());
    result &= Test("assert_early_exit", testAssertFail(), false);
    return result;
}

// =============================================================
// Explicit-width integer type tests
// C equivalents: i8=int8_t, i16=int16_t, i32=int32_t, i64=int64_t
//                u8=uint8_t, u16=uint16_t, u32=uint32_t, u64=uint64_t
// =============================================================

int sumI32(i32 a, i32 b) { return a + b; }
int sumU32(u32 a, u32 b) { return a + b; }

bool testExplicitIntTypes()
{
    bool result = true;

    // Signed types: declaration and basic value
    i8  s8  = 100;
    i16 s16 = 1000;
    i32 s32 = 100000;
    i64 s64 = 200000;

    result &= Test("i8_value",  s8,  100);
    result &= Test("i16_value", s16, 1000);
    result &= Test("i32_value", s32, 100000);
    result &= Test("i64_value", s64, 200000);

    // Unsigned types: declaration and basic value
    u8  u8v  = 200;
    u16 u16v = 2000;
    u32 u32v = 300000;
    u64 u64v = 400000;

    result &= Test("u8_value",  u8v,  200);
    result &= Test("u16_value", u16v, 2000);
    result &= Test("u32_value", u32v, 300000);
    result &= Test("u64_value", u64v, 400000);

    // Arithmetic - values chosen to stay within each type's range
    i8  s8a  = 50;   // 50+50=100, within i8  (-128..127)
    u8  u8a  = 100;  // 100+100=200, within u8 (0..255)
    result &= Test("i8_add",  s8a  + s8a,   100);  // 50+50=100
    result &= Test("i16_add", s16  + s16,   2000); // 1000+1000=2000, within i16
    result &= Test("i32_add", s32  + s32,   200000); // 100000+100000=200000
    result &= Test("u8_add",  u8a  + u8a,   200);  // 100+100=200
    result &= Test("u16_add", u16v + u16v,  4000); // 2000+2000=4000, within u16

    // C-equivalent compatibility: int <-> i32 are the same type
    int ci = s32;
    result &= Test("i32_to_int", ci, 100000);

    i32 ia = ci;
    result &= Test("int_to_i32", ia, 100000);

    // Passing i32/u32 to functions expecting i32/u32
    result &= Test("func_i32", sumI32(40, 2), 42);
    result &= Test("func_u32", sumU32(40, 2), 42);

    // Passing int to function expecting i32 (compatible types)
    int x = 10;
    result &= Test("int_to_i32_param", sumI32(x, 5), 15);

    return result;
}

// =============================================================
// Null-conditional (?.) and null-coalescing (??) tests
// =============================================================

struct NullableNode
{
    int value = 99;
    int Read() { return value; }
};

// Helper: returns the node's value if non-null, -1 otherwise
int readNodeValueNC(NullableNode* node)
{
    return node?.value ?? -1;
}

// Helper: calls Read() if non-null, returns -1 otherwise
int callNodeRead(NullableNode* node)
{
    return node?.Read() ?? -1;
}

bool testNullConditional()
{
    bool result = true;

    NullableNode n = NullableNode();
    NullableNode* p = &n;
    NullableNode* np = nullptr;

    // null-conditional field access + coalescing via helper
    // (helpers take NullableNode* as a function arg, which correctly sets up struct access)
    result &= Test("nc_coalesce_nonnull", readNodeValueNC(p),  99);
    result &= Test("nc_coalesce_null",    readNodeValueNC(np), -1);

    // null-conditional method call via helper
    result &= Test("nc_method_nonnull",   callNodeRead(p),  99);
    result &= Test("nc_method_null",      callNodeRead(np), -1);

    return result;
}

const char* tryGetName(bool found)
{
    if (found) return "Alice";
    return nullptr;
}

bool testNullCoalescing()
{
    bool result = true;

    // pointer coalescing: null -> fallback string
    const char* name1 = tryGetName(true)  ?? "Unknown";
    const char* name2 = tryGetName(false) ?? "Unknown";
    result &= TestStr("nullcoal_nonnull_ptr", name1, "Alice");
    result &= TestStr("nullcoal_null_ptr",    name2, "Unknown");

    // integer coalescing: 0 -> default, nonzero -> self
    int zero = 0;
    int five = 5;
    result &= Test("nullcoal_zero",    zero ?? 42, 42);
    result &= Test("nullcoal_nonzero", five ?? 42, 5);

    return result;
}

// =============================================================
// Default keyword tests
// =============================================================

bool testDefault()
{
    bool result = true;

    // Primitive types: default -> zero value
    int  di = default;
    bool db = default;
    i32  di32 = default;
    i64  di64 = default;

    result &= Test("default_int",  di,   0);
    result &= Test("default_bool", db,   0);
    result &= Test("default_i32",  di32, 0);
    result &= Test("default_i64",  di64, 0);

    // Struct type: default -> calls default constructor, fields get their declared defaults
    MyStruct ds = default;
    result &= Test("default_struct_num1", ds.num1, 1);
    result &= Test("default_struct_num2", ds.num2, 2);
    result &= Test("default_struct_num3", ds.num3, 3);

    return result;
}

class Node
{
    int value = 0;
};

int testNewDelete()
{
    Node* n = new Node();
    delete n;
    return 42;
}

int testNewArray()
{
    int* arr = new int[5];
    for (int i = 0; i < 5; i++)
    {
        arr[i] = i;
	}

    for (int i = 0; i < 5; i++)
    {
        if (arr[i] != i)
        {
            printf("testNewArray failed: arr[%d] expected %d but got %d.\n", i, i, arr[i]);
            delete[] arr;
            return 1;
        }
	}

    delete[] arr;
    return 99;
}

class Counter
{
    int count = 0;
};

int testNewWithConstructor()
{
    Counter* c = new Counter();
    delete c;
    return 123;
}

extern int main()
{
    MyStruct my = MyStruct();
    MyStruct1 struct1 = MyStruct1();
    MyStruct2 struct2 = MyStruct2();

    bool result = true;
    result &= Test("testArray", testArray(), 435);
    result &= testNamedParameters();
    result &= Test("testTotalByValue", my.TotalByValue(), 5);
    result &= Test("testTotalByPointer", my.TotalByPointer(), 5);
    result &= Test("struct1.Read", struct1.Read(), 1);
    result &= Test("struct2.Read", struct2.Read(), 2);
    result &= Test("struct1.ReadExt", struct1.ReadExt(), 1);
    result &= Test("struct2.ReadExt", struct2.ReadExt(), 2);
    result &= testDefaultSingleParam();
    result &= testDefaultMultipleParams();
    result &= testDefaultExpression();
    result &= testDestructor();
    result &= testBuiltinIdentifiers();
    result &= testTypeof();
    result &= testNameof();
    result &= testNamespace();
    result &= testNestedNamespace();
    result &= testNestedUsing();
    result &= testLocalUsing();
    result &= testLocalUsingScoped();
    result &= testForwardFunction();
    result &= testMutualRecursion();
    result &= testForwardFunctionWithStruct();
    result &= testForwardInStruct();
    result &= testReturnBlock();
    result &= testAssertReturnBlock();
    result &= testExplicitIntTypes();
    result &= testNullConditional();
    result &= testNullCoalescing();
    result &= testDefault();
    result &= Test("testNewDelete", testNewDelete(), 42);
    result &= Test("testNewArray", testNewArray(), 99);
    result &= Test("testNewWithConstructor", testNewWithConstructor(), 123);

    if (result)
    {
        printf("All Test Passed.\n");
        return 0;
    }

    return 1;
}
