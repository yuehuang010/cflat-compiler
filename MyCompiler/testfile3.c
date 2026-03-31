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

// Interface tests

interface IReadable
{
	int Read();
};

interface IScalable
{
	int Scale(int factor);
};

struct Counter : IReadable
{
	int count = 10;
	int Read() { return count; }
};

struct ScaledValue : IReadable, IScalable
{
	int value = 3;
	int Read() { return value; }
	int Scale(int factor) { return value * factor; }
};

int extensionIRead(IReadable scaledValue)
{
	return scaledValue.Read();
}

bool testInterface()
{
	bool result = true;
	Counter c = Counter();
	ScaledValue s = ScaledValue();

	result &= Test("counter.Read", c.Read(), 10);   // 10
	result &= Test("scaledValue.Read", s.Read(), 3);   // 3
	result &= Test("scaledValue.extensionIRead", s.extensionIRead(), 3);   // 3
	result &= Test("scaledValue.Scale2", s.Scale(2), 6);   // 3*2=6
	result &= Test("scaledValue.Scale5", s.Scale(5), 15);   // 3*5=15
	return result;
}


extern void main()
{
	testInterface();
}
