
extern void printf(const char* argv, ...);
extern void* malloc(i64 size);
extern void free(void* ptr);

bool TestVerbose = true;

bool Test(const char* testName, long long actual, long long expected)
{
    if (expected == actual)
    {
        if (TestVerbose) printf("%s passed.\n", testName);
        return true;
    }

    if (TestVerbose) printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
    return false;
}

bool Test(const char* testName, int actual, int expected)
{
    if (expected == actual)
    {
        if (TestVerbose) printf("%s passed.\n", testName);
        return true;
    }

    if (TestVerbose) printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
    return false;
}

bool Test(const char* testName, float actual, float expected)
{
    if (expected == actual)
    {
        if (TestVerbose) printf("%s passed (float).\n", testName);
        return true;
    }

    if (TestVerbose) printf("%s failed expecting '%f' but got '%f'.\n", testName, expected, actual);
    return false;
}

bool Test(const char* testName, double actual, double expected)
{
    if (expected == actual)
    {
        if (TestVerbose) printf("%s passed (double).\n", testName);
        return true;
    }

    if (TestVerbose) printf("%s failed expecting '%lf' but got '%lf'.\n", testName, expected, actual);
    return false;
}

bool Test(const char* testName, bool actual, bool expected)
{
    if (expected == actual)
    {
        if (TestVerbose) printf("%s passed.\n", testName);
        return true;
    }

    if (TestVerbose) printf("%s failed expecting '%s' but got '%s'.\n", testName, expected ? "false" : "true", actual ? "false" : "true");
    return false;
}

int fooInt = 0;
double fooDouble;
float fooFloat = 10.f;

void testGlobalVariable()
{
    int num = fooInt;
    float num2 = fooFloat;
    double num3 = fooDouble;
}

int myFunctionArgument(int argc)
{
    int var = argc;
    return var;
}

void testEmptyFunction() {}
void testEmptyIfStatement()
{
    if (true) {}
    else {}

    if (false) {}
    else {}
}

int shortCircuitCounter = 0;

bool FuncTrue() { shortCircuitCounter++; return true; }

bool FuncFalse() { shortCircuitCounter++; return false; }

bool testLogicalAnd()
{
    return FuncTrue() && !FuncFalse();
}

bool testLogicalOr()
{
    return FuncFalse() || FuncTrue();
}

bool testShortCircuit()
{
    TestVerbose = false;
    bool result = true;
    shortCircuitCounter = 0;
    result &= Test("FuncTrue() || FuncTrue() || FuncTrue()", FuncTrue() || FuncTrue() || FuncTrue(), true);
    result &= Test("shortCircuitCounter", shortCircuitCounter, 1);

    shortCircuitCounter = 0;
    result &= Test("FuncFalse() || FuncFalse() || FuncTrue()", FuncFalse() || FuncTrue() || FuncTrue(), true);
    result &= Test("shortCircuitCounter", shortCircuitCounter, 2);

    shortCircuitCounter = 0;
    result &= Test("FuncFalse() || FuncFalse() || FuncFalse()", FuncFalse() || FuncFalse() || FuncFalse(), false);
    result &= Test("shortCircuitCounter", shortCircuitCounter, 3);

    shortCircuitCounter = 0;
    result &= Test("FuncFalse() && FuncTrue() && FuncTrue()", FuncFalse() && FuncTrue() && FuncTrue(), false);
    result &= Test("shortCircuitCounter", shortCircuitCounter, 1);

    shortCircuitCounter = 0;
    result &= Test("FuncTrue() && FuncFalse() && FuncTrue()", FuncTrue() && FuncFalse() && FuncTrue(), false);
    result &= Test("shortCircuitCounter", shortCircuitCounter, 2);

    shortCircuitCounter = 0;
    result &= Test("FuncTrue() && FuncTrue() && FuncTrue()", FuncTrue() && FuncTrue() && FuncTrue(), true);
    result &= Test("shortCircuitCounter", shortCircuitCounter, 3);

    TestVerbose = true;

    return result;
}

int myWhileLoopBreak()
{
    int count = 10;
    while (count > 0)
    {
        int index = 0;
        count = count - 1;

        if (count == 5)
            break;
    }

    return count;
}

int myWhileLoopDecrement()
{
    int counter = 10;
    while (counter > 0)
    {
        counter--;
    }

    return counter;
}

int myWhileLoopNotEnter()
{
    int counter = 0;
    while (counter > 0)
    {
        counter--;
    }
    return counter;
}

int testForLoop()
{
    int sum = 0;
    for (int i = 0; i < 30; i++)
    {
        sum += i;
    }

    return sum;
}

int testIfElseStatement()
{
    int count = 10;

    if (count == 10)
    {
        count = 1;
    }
    else
    {
        count = 2;
    }

    if (count == 1)
    {

        count = 11;
    }

    return count;
}

int testConditional()
{
    int count = 10;
    count = (count == 10) ? 11 : 12;
    return count;
}

int testMultipleBreaks()
{
    int n = 10;
    while (n == 10)
    {
        return 20;
        break;
        continue;
    }

    return n;
}

struct MyStruct
{
    int num1 = 1;
    int num2 = 2;
    int num3 = 3;

    int Total()
    {
        return num1 + num2 + num3;
    }

    int Add(int addition)
    {
        num1 = num1 + addition;
        num2 = num2 + addition;
        num3 = num3 + addition;
        return num1 + num2 + num3;
    }
};

int testStruct()
{
    auto myStruct = MyStruct();
    myStruct.num1 = 100;
    myStruct.num2++;
    return myStruct.num1 + myStruct.num2 + myStruct.num3;
}

int testStructFunctionCall()
{
    MyStruct myStruct = MyStruct();
    return myStruct.Add(10);
}

struct MyStruct2
{
    MyStruct myStruct = MyStruct();
};

int testInnerStruct()
{
    auto myStruct2 = MyStruct2();
    myStruct2.myStruct.num1 = 100;
    myStruct2.myStruct.num2++;
    return myStruct2.myStruct.num1 + myStruct2.myStruct.num2 + myStruct2.myStruct.num3;
}

short testShortAdd()
{
    short num1 = 10;
    short num2 = 10;
    return num1 + num2;
}

int testIntAdd()
{
    int num1 = 10;
    int num2 = 10;
    return num1 + num2;
}

double testDoubleAdd()
{
    double num1 = 10.0;
    double num2 = 10.0;
    return num1 + num2;
}

float testFloatAdd()
{
    float num1 = 10.0f;
    float num2 = 10.0f;
    return num1 + num2;
}

int testOrderOfOperation()
{
    int num1 = 1;
    int num2 = 2;
    int num3 = 3;
    int num4 = 4;

    int num5 = num1 + num2 * num3 + num4;
    return num5;
}

void testPointers()
{
    int number = 10;
    printf("number=%d, &number=%p\n", number, &number);

    auto mall = malloc(10);
    free(mall);
}

int switchBasic(int x)
{
    switch (x)
    {
        case 1: return 10;
        case 2: return 20;
        case 3: return 30;
        default: return 0;
    }
    return 0;
}

int switchBreak(int x)
{
    int result = 0;
    switch (x)
    {
        case 1: result = 10; break;
        case 2: result = 20; break;
        default: result = 99; break;
    }
    return result;
}

int switchNoDefault(int x)
{
    int result = 0;
    switch (x)
    {
        case 5: result = 5; break;
        case 6: result = 6; break;
    }
    return result;
}

int switchFallthrough(int x)
{
    int result = 0;
    switch (x)
    {
        case 1:
        case 2:
            result = 12;
            break;
        case 3:
            result = 3;
            break;
    }
    return result;
}

bool testNumericLiterals()
{
    bool result = true;
    result &= Test("decimal_literal", 123, 123);
    result &= Test("hex_literal", 0xFF, 255);
    result &= Test("octal_literal", 077, 63);
    result &= Test("unsigned_suffix", 123u, 123);
    result &= Test("long_suffix", 123L, 123);
    result &= Test("long_long_suffix", 123LL, 123);
    result &= Test("negative_literal", -42, -42);
    result &= Test("float_literal_f", 1.5f, 1.5f);
    result &= Test("double_literal", 1.5, 1.5);
    result &= Test("scientific_double", 1e3, 1000.0);
    return result;
}

extern int main(int argc, char** argv)
{
    // Test the tests
    bool result = true;
    result &= Test("TestInt", 1, 1);
    result &= Test("TestFloat", 1.0f, 1.0f);
    result &= Test("TestDouble", 1.0, 1.0);
    result &= Test("TestBool", true, true);

    if (!result)
    {
        printf("Test function is broken.\n");
        return 0;
    }

    testEmptyFunction();
    testEmptyIfStatement();
    testPointers();
    result &= Test("testConditional", testConditional(), 12);
    result &= Test("testIfElseStatement", testIfElseStatement(), 11);
    result &= Test("myWhileLoopNotEnter", myWhileLoopNotEnter(), 0);
    result &= Test("myWhileLoopDecrement", myWhileLoopDecrement(), 0);
    result &= Test("myWhileLoopBreak", myWhileLoopBreak(), 5);
    result &= Test("mySimpleFunction", myFunctionArgument(11), 11);
    result &= Test("testMultipleBreaks", testMultipleBreaks(), 20);
    result &= Test("testForLoop", testForLoop(), 435);
    result &= Test("testStruct", testStruct(), 106);
    result &= Test("testStructFunctionCall", testStructFunctionCall(), 36);
    result &= Test("testInnerStruct", testInnerStruct(), 106);
    result &= Test("testShortAdd", testShortAdd(), 20);
    result &= Test("testIntAdd", testIntAdd(), 20);
    result &= Test("testOrderOfOperation", testOrderOfOperation(), 11);
    result &= Test("testFloatAdd", testFloatAdd(), 20.0f);
    result &= Test("testDoubleAdd", testDoubleAdd(), 20.0);
    result &= Test("testLogicalAnd", testLogicalAnd(), true);
    result &= Test("testLogicalOr", testLogicalOr(), true);
    result &= Test("testShortCircuit", testShortCircuit(), true);
    result &= Test("testNumericLiterals", testNumericLiterals(), true);
    result &= Test("switch_case1",     switchBasic(1), 10);
    result &= Test("switch_case2",     switchBasic(2), 20);
    result &= Test("switch_case3",     switchBasic(3), 30);
    result &= Test("switch_default",   switchBasic(9),  0);
    result &= Test("switch_break1",    switchBreak(1), 10);
    result &= Test("switch_break2",    switchBreak(2), 20);
    result &= Test("switch_break_def", switchBreak(7), 99);
    result &= Test("switch_nodef_hit", switchNoDefault(5), 5);
    result &= Test("switch_nodef_miss",switchNoDefault(9), 0);
    result &= Test("switch_fall1",     switchFallthrough(1), 12);
    result &= Test("switch_fall2",     switchFallthrough(2), 12);
    result &= Test("switch_fall3",     switchFallthrough(3),  3);

    if (result)
    {
        printf("All Test Passed.\n");
        return 0;
    }

    return 1;
}