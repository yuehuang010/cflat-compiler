void printf(const char* argv, ...);

//
//int Test(const char* testName, int expected, int actual)
//{
//    if (expected == actual)
//    {
//        printf("%s passed.\n", testName);
//        return 1;
//    }
//
//    printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
//    return 0;
//}


bool Test(const char* testName, bool expected, bool actual)
{
    if (expected == actual)
    {
        printf("%s passed.\n", testName);
        return true;
    }

    printf("%s failed expecting '%s' but got '%s'.\n", testName, expected ? "false" : "true", actual ? "false" : "true");
    return false;
}

int mySimpleFunction(int argc)
{
    int var = argc;
    return var;
}



int main() {
    Test("Testing True", true, true);
    // Test("mySimpleFunction", 11, mySimpleFunction(11));
    return 0;
}
