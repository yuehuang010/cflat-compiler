extern void printf(const char* argv, ...);

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

// Interface tests

interface IReadable
{
    int Read();
};

interface IScalable
{
    int Scale(int factor);
};

struct Counter : IReadable
{
    int count = 10;
    int Read() { return count; }
};

struct ScaledValue : IReadable, IScalable
{
    int value = 3;
    int Read() { return value; }
    int Scale(int factor) { return value * factor; }
};

bool testInterface()
{
    bool result = true;
    Counter c = Counter();
    ScaledValue s = ScaledValue();

    result &= Test("counter.Read", c.Read(), 10);   // 10
    result &= Test("scaledValue.Read", s.Read(), 3);   // 3
    result &= Test("scaledValue.Scale2", s.Scale(2), 6);   // 3*2=6
    result &= Test("scaledValue.Scale5", s.Scale(5), 15);   // 3*5=15
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

bool testBuiltinIdentifiers()
{
    bool result = true;
    result &= TestStr("__FILE__", __FILE__, "testfile2.c");
    result &= TestStr("__FUNCTION__", __FUNCTION__, "testBuiltinIdentifiers");
    result &= Test("__LINE__", __LINE__, 238);
    return result;
}

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
    result &= testInterface();
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

    if (result)
    {
        printf("All Test Passed.\n");
        return 0;
    }

    return 1;
}
