void printf(const char* argv, ...);

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

bool TestBool(const char* testName, bool actual)
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

int testArray()
{
	int arraySize = 30;
	int[arraySize] array;

	int i = 0;
	while (i < arraySize)
	{
		array[i] = i;
		i++;
	}
	
	int sum = 0;
	i = 0;
	while (i < arraySize)
	{
		sum += array[i];
		i++;
	}

	return sum;
}

int myFunction(int x, int y, int z)
{
	// printf("x=%d,y=%d,z=%d: ", x, y, z);
	int pair_xy = (x + y) * (x + y + 1) / 2 + y;
	return (pair_xy + z) * (pair_xy + z + 1) / 2 + z;
}

bool testNamedParameters()
{
	bool result = true;
	int expected = myFunction(1, 2, 3);
	result &= Test("InOrder", myFunction(x:1, y : 2, z : 3), expected);
	result &= Test("OutOfOrderName", myFunction( y:2, z:3, x:1), expected);
	result &= Test("MixedInOrder", myFunction(x:1, y:2, 3), expected);
	result &= Test("MixedOutOfOrder", myFunction(z:3, 1, 2 ), expected);

	return result;
}

void main()
{
	bool result = true;
	result &= Test("testArray", testArray(), 435);
	result &= testNamedParameters();

	if (result)
	{
		printf("All Test Passed\n");
	}
}
