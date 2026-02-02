int foobar = 0;
double foocat;

void printf(const char* argv, ...);

int Test(const char* testName, int expected, int actual) {
    if (expected == actual)
    {
        printf("%s passed.\n", testName);
        return 1;
    }

    printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
    return 0;
}

bool TestBool(const char* testName, bool expected, bool actual)
{
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

int main(int argc, char** argv) {
    bool result = true;
    testEmptyFunction();
    testEmptyIfStatement();
    result &= Test("testConditional", 12, testConditional());
    result &= Test("testIfElseStatement", 11, testIfElseStatement());
    result &= Test("myWhileLoopNotEnter", 0, myWhileLoopNotEnter());
    result &= Test("myWhileLoopDecrement", 0, myWhileLoopDecrement());
    result &= Test("myWhileLoopBreak", 5, myWhileLoopBreak());
    result &= Test("mySimpleFunction", 11, myFunctionArgument(11));
    result &= Test("testMultipleBreaks", 20, testMultipleBreaks());

    if (result)
    {
        printf("All Test Passed\n");
    }

    return 0;
}