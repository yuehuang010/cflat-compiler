
void printf(const char* argv, ...);

int foobar = 0;
double foocat;

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

bool testFloatPoint()
{
    //float num1 = 10.0f;
    //float num2 = 11.0f;

    //return num1 > num2;
    return true;
}

struct MyStruct
{
    int num1 = 1;
    int num2 = 2;
    int num3 = 3;
};

int testStruct()
{
    auto myStruct = MyStruct();
    myStruct.num1 = 100;
    myStruct.num2++;
    return myStruct.num1 + myStruct.num2 + myStruct.num3;
}

int main(int argc, char** argv) {
    bool result = true;
    testEmptyFunction();
    testEmptyIfStatement();
    result &= Test("testConditional", testConditional(), 12);
    result &= Test("testIfElseStatement", testIfElseStatement(), 11);
    result &= Test("myWhileLoopNotEnter", myWhileLoopNotEnter(), 0);
    result &= Test("myWhileLoopDecrement", myWhileLoopDecrement(), 0);
    result &= Test("myWhileLoopBreak", myWhileLoopBreak(), 5);
    result &= Test("mySimpleFunction", myFunctionArgument(11), 11);
    result &= Test("testMultipleBreaks", testMultipleBreaks(), 20);
    // result &= TestBool("testFloatPoint", testFloatPoint());
    result &= Test("testStruct", testStruct(), 106);

    if (result)
    {
        printf("All Test Passed\n");
    }

    return 0;
}