import "string.c";

extern void printf(const char* fmt, ...);
extern int strcmp(const char* a, const char* b);

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

bool testLength()
{
    String s = String();
    s.Init("hello");
    bool result = true;
    result &= Test("length_hello",  s.Length(), 5);
    s.Init("");
    result &= Test("length_empty",  s.Length(), 0);
    s.Init("a");
    result &= Test("length_single", s.Length(), 1);
    return result;
}

bool testData()
{
    String s = String();
    s.Init("world");
    bool result = true;
    result &= TestStr("data_world", s.Data(), "world");
    return result;
}

bool testCharAt()
{
    String s = String();
    s.Init("hello");
    bool result = true;
    result &= Test("charAt_0", s.CharAt(0), 'h');
    result &= Test("charAt_1", s.CharAt(1), 'e');
    result &= Test("charAt_4", s.CharAt(4), 'o');
    return result;
}

bool testEquals()
{
    String s = String();
    s.Init("hello");
    bool result = true;
    result &= Test("equals_match",    s.Equals("hello"),  true);
    result &= Test("equals_mismatch", s.Equals("world"),  false);
    result &= Test("equals_empty",    s.Equals(""),       false);
    result &= Test("equals_prefix",   s.Equals("hell"),   false);
    return result;
}

bool testStartsWith()
{
    String s = String();
    s.Init("hello world");
    bool result = true;
    result &= Test("startsWith_match",    s.StartsWith("hello"),       true);
    result &= Test("startsWith_full",     s.StartsWith("hello world"), true);
    result &= Test("startsWith_mismatch", s.StartsWith("world"),       false);
    result &= Test("startsWith_longer",   s.StartsWith("hello world!"),false);
    result &= Test("startsWith_empty",    s.StartsWith(""),             true);
    return result;
}

bool testEndsWith()
{
    String s = String();
    s.Init("hello world");
    bool result = true;
    result &= Test("endsWith_match",    s.EndsWith("world"),        true);
    result &= Test("endsWith_full",     s.EndsWith("hello world"),  true);
    result &= Test("endsWith_mismatch", s.EndsWith("hello"),        false);
    result &= Test("endsWith_longer",   s.EndsWith("!hello world"), false);
    result &= Test("endsWith_empty",    s.EndsWith(""),              true);
    return result;
}

extern int main()
{
    bool result = true;
    result &= testLength();
    result &= testData();
    result &= testCharAt();
    result &= testEquals();
    result &= testStartsWith();
    result &= testEndsWith();

    if (result)
    {
        printf("All Test Passed.\n");
        return 0;
    }

    return 1;
}
