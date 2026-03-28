extern void printf(const char* argv, ...);

bool TestVerbose = true;


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
		if (TestVerbose) printf("%s passed.\n", testName);
		return true;
	}

	if (TestVerbose) printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
	return false;
}

int Test(const char* testName, float actual, float expected)
{
	if (expected == actual)
	{
		if (TestVerbose) printf("%s passed (float).\n", testName);
		return true;
	}

	if (TestVerbose) printf("%s failed expecting '%f' but got '%f'.\n", testName, expected, actual);
	return false;
}

int Test(const char* testName, double actual, double expected)
{
	if (expected == actual)
	{
		if (TestVerbose) printf("%s passed (double).\n", testName);
		return true;
	}

	if (TestVerbose) printf("%s failed expecting '%lf' but got '%lf'.\n", testName, expected, actual);
	return false;
}

int Test(const char* testName, bool actual, bool expected)
{
	if (expected == actual)
	{
		if (TestVerbose) printf("%s passed.\n", testName);
		return true;
	}

	if (TestVerbose) printf("%s failed expecting '%s' but got '%s'.\n", testName, expected ? "false" : "true", actual ? "false" : "true");
	return false;
}

struct mystruct
{

};


bool testExplicitIntTypes()
{
    bool result = true;

    // Signed types: declaration and basic value
    i8  s8 = 100;
    i16 s16 = 1000;
    i32 s32 = 100000;
    i64 s64 = 200000;

    //result &= Test("i8_value", s8, 100);
    //result &= Test("i16_value", s16, 1000);
    //result &= Test("i32_value", s32, 100000);
    //result &= Test("i64_value", s64, 200000);

    // Unsigned types: declaration and basic value
    u8  u8v = 200;
    u16 u16v = 2000;
    u32 u32v = 300000;
    u64 u64v = 400000;

    //result &= Test("u8_value", u8v, 200);
    //result &= Test("u16_value", u16v, 2000);
    //result &= Test("u32_value", u32v, 300000);
    //result &= Test("u64_value", u64v, 400000);

    // Arithmetic - values chosen to stay within each type's range
    i8  s8a = 50;   // 50+50=100, within i8  (-128..127)
    u8  u8a = 100;  // 100+100=200, within u8 (0..255)

    // result &= Test("i8_add", s8a + s8a, 100);  // 50+50=100
    Test("is_s16", s16, 1000);
    Test("is_u16v", u16v, 2000); 
    Test("is_s16", s16, 1000);

    Test("i16_add", s16 + s16, 2000); // 1000+1000=2000, within i16

    // result &= Test("i32_add", s32 + s32, 200000); // 100000+100000=200000
    result &= Test("is_u16v", u16v, 2000); 
    result &= Test("u8_add", u8a + u8a, 200);  // 100+100=200
    result &= Test("is_u16v", u16v, 2000); 
    result &= Test("u16_add", u16v + u16v, 4000); // 2000+2000=4000, within u16

    // C-equivalent compatibility: int <-> i32 are the same type
    //int ci = s32;
    //result &= Test("i32_to_int", ci, 100000);

    //i32 ia = ci;
    //result &= Test("int_to_i32", ia, 100000);

    return result;
}


extern void main()
{
    testExplicitIntTypes();
}
