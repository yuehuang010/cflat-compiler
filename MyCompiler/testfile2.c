void printf(const char* argv, ...);

int Test(const char* testName, int expected, int actual)
{
    if (expected == actual)
    {
        printf("%s passed.\n", testName);
        return 1;
    }

    printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
    return 0;
}

int mySimpleFunction(int argc)
{
    int var = argc;
    return var;
}

int main() {
    Test("mySimpleFunction", 11, mySimpleFunction(11));
    return 0;
}

