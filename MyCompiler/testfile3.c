extern void printf(const char* argv, ...);


bool Test(const char* testName, int actual, int expected)
{
	if (expected == actual)
	{
		printf("%s passed.\n", testName);
		return true;
	}

	printf("%s failed expecting '%d' but got '%d'.\n", testName, expected, actual);
	return false;
}

bool Test(const char* testName, float actual, float expected)
{
	if (expected == actual)
	{
		printf("%s passed (float).\n", testName);
		return true;
	}

	printf("%s failed expecting '%f' but got '%f'.\n", testName, expected, actual);
	return false;
}

bool Test(const char* testName, double actual, double expected)
{
	if (expected == actual)
	{
		printf("%s passed (double).\n", testName);
		return true;
	}

	printf("%s failed expecting '%lf' but got '%lf'.\n", testName, expected, actual);
	return false;
}

bool Test(const char* testName, bool actual)
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


int shortCircuitCounter = 0;

bool FuncTrue() { shortCircuitCounter++; return true; }

bool FuncFalse() { shortCircuitCounter++; return false; }

//bool testLogicalAnd()
//{
//	return FuncTrue() && !FuncFalse();
//}
//
//bool testLogicalOr()
//{
//	return FuncFalse() || FuncTrue();
//}

bool testShortCircuit()
{
	bool result = true;
	shortCircuitCounter = 0;
	result &= Test("FuncTrue() || FuncTrue() || FuncTrue()", FuncTrue() || FuncTrue() || FuncTrue());
	result &= Test("shortCircuitCounter", shortCircuitCounter, 1);

	shortCircuitCounter = 0;
	result &= Test("FuncFalse() || FuncFalse() || FuncTrue()", FuncFalse() || FuncTrue() || FuncTrue());
	result &= Test("shortCircuitCounter", shortCircuitCounter, 2);

	shortCircuitCounter = 0;
	result &= Test("FuncFalse() || FuncFalse() || FuncFalse()", FuncFalse() || FuncFalse() || FuncFalse());
	result &= Test("shortCircuitCounter", shortCircuitCounter, 3);

	shortCircuitCounter = 0;
	result &= Test("FuncFalse() && FuncTrue() && FuncTrue()", FuncFalse() && FuncTrue() && FuncTrue());
	result &= Test("shortCircuitCounter", shortCircuitCounter, 1);

	shortCircuitCounter = 0;
	result &= Test("FuncTrue() && FuncFalse() && FuncTrue()", FuncTrue() && FuncFalse() && FuncTrue());
	result &= Test("shortCircuitCounter", shortCircuitCounter, 2);

	shortCircuitCounter = 0;
	result &= Test("FuncTrue() && FuncTrue() && FuncTrue()", FuncTrue() && FuncTrue() && FuncTrue());
	result &= Test("shortCircuitCounter", shortCircuitCounter, 3);

	return result;
}

extern void main()
{
	//testLogicalAnd();
	//testLogicalOr();
	 testShortCircuit();
}
