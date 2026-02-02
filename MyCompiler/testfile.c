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

int myFunctionArgument(int argc) {
    int var = argc;
    return var;
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


int main(int argc, char** argv) {

    Test("testIfElseStatement", 11, testIfElseStatement());
    Test("myWhileLoopNotEnter", 0, myWhileLoopNotEnter());
    Test("myWhileLoopDecrement", 0, myWhileLoopDecrement());
    Test("myWhileLoopBreak", 5, myWhileLoopBreak());
    Test("mySimpleFunction", 11, myFunctionArgument(11));
    Test("myWhileLoopDecrement", 0, myWhileLoopDecrement());
    return 0;
}