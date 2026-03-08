extern void printf(const char* argv, ...);

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

struct MyStruct
{
	int num1 = 1;
	int num2 = 2;
	int num3 = 3;
};

int TotalByValue(MyStruct mystruct)
{
	return mystruct.num1 + mystruct.num2 + mystruct.num2;
}

int TotalByPointer(MyStruct* mystruct)
{
	return mystruct->num1 + mystruct->num2 + mystruct->num2;
}

struct MyStruct1
{
	int num = 1;
	int Read() { return num; }
};

struct MyStruct2
{
	int num = 2;
	int Read() { return num; }
};

// Overloaded Extension function
int ReadExt(MyStruct1 my) { return my.num; }
int ReadExt(MyStruct2 my) { return my.num; }

extern void main()
{
	MyStruct my = MyStruct();
	MyStruct1 struct1 = MyStruct1();
	MyStruct2 struct2 = MyStruct2();

	bool result = true;
	result &= Test("testArray", testArray(), 435);
	result &= testNamedParameters();
	result &= Test("testTotalByValue", my.TotalByValue(), 5);
	result &= Test("testTotalByPointer", my.TotalByPointer(), 5);
	result &= Test("struct1.Read", struct1.Read(), 1);
	result &= Test("struct2.Read", struct2.Read(), 2);
	result &= Test("struct1.ReadExt", struct1.ReadExt(), 1);
	result &= Test("struct2.ReadExt", struct2.ReadExt(), 2);

	if (result)
	{
		printf("All Test Passed\n");
	}
}
