
void printf(const char* argv, ...);

int fooInt = 0;
double fooDouble;
float fooFloat = 10.f;

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

void testGlobalVariable()
{
    int num = fooInt;
    float num2 = fooFloat;
    double num3 = fooDouble;
}

int myFunctionArgument(int argc) {
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

int myWhileLoopBreak() {
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

int myWhileLoopNotEnter() {
    int counter = 0;
    while (counter > 0)
    {
        counter--;
    }
    return counter;
}

int testIfElseStatement() {
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

void* malloc(int size)
{
    printf("malloc size = %d\n", size);
    return (void*)size;
}

void free(void* ptr)
{
    printf("free ptr = %p\n", ptr);
}

void testPointers()
{
    int number = 10;
    printf("number=%d, &number=%p\n", number, &number);

    auto mall = malloc(10);
    free(mall);
}


int main(int argc, char** argv) {
    bool result = true;
    testEmptyFunction();
    testEmptyIfStatement();
    testPointers;
    result &= Test("testConditional", testConditional(), 12);
    result &= Test("testIfElseStatement", testIfElseStatement(), 11);
    result &= Test("myWhileLoopNotEnter", myWhileLoopNotEnter(), 0);
    result &= Test("myWhileLoopDecrement", myWhileLoopDecrement(), 0);
    result &= Test("myWhileLoopBreak", myWhileLoopBreak(), 5);
    result &= Test("mySimpleFunction", myFunctionArgument(11), 11);
    result &= Test("testMultipleBreaks", testMultipleBreaks(), 20);
    result &= Test("testStruct", testStruct(), 106);
    result &= Test("testStructFunctionCall", testStructFunctionCall(), 36);
    result &= Test("testInnerStruct", testInnerStruct(), 106);
    result &= Test("testShortAdd", testShortAdd(), 20);
    result &= Test("testIntAdd", testIntAdd(), 20);
    result &= Test("testOrderOfOperation", testOrderOfOperation(), 11);
    testFloatAdd();
    testDoubleAdd();

    if (result)
    {
        printf("All Test Passed\n");
    }

    return 0;
}