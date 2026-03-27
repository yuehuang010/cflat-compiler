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

extern void main()
{
	bool result = true;
	result &= Test("testLogicalAnd", testLogicalAnd(), true);
	result &= Test("testLogicalOr", testLogicalOr(), true);
	result &= Test("testShortCircuit();", testShortCircuit(), true);
}
