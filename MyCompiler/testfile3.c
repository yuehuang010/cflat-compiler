extern void printf(const char* argv, ...);

bool TestVerbose = true;

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

struct mystruct
{

};


extern void main()
{
	mystruct my;
}
