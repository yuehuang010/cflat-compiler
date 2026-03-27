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

// Default parameter tests

int addWithDefault(int x, int y = 5)
{
	return x + y;
}

bool testDefaultSingleParam()
{
	bool result = true;
	result &= Test("add_both_args",  addWithDefault(3, 4), 7);  // 3+4=7
	result &= Test("add_default_y",  addWithDefault(3),    8);  // 3+5=8
	return result;
}

int sumWithDefaults(int x, int y = 10, int z = 20)
{
	return x + y + z;
}

bool testDefaultMultipleParams()
{
	bool result = true;
	result &= Test("sum_all_args",    sumWithDefaults(1, 2, 3), 6);   // 1+2+3=6
	result &= Test("sum_default_z",   sumWithDefaults(1, 2),    23);  // 1+2+20=23
	result &= Test("sum_defaults_yz", sumWithDefaults(1),       31);  // 1+10+20=31
	return result;
}

int multiplyWithDefault(int x, int factor = 2)
{
	return x * factor;
}

bool testDefaultExpression()
{
	bool result = true;
	result &= Test("multiply_explicit", multiplyWithDefault(5, 3), 15);  // 5*3=15
	result &= Test("multiply_default",  multiplyWithDefault(5),    10);  // 5*2=10
	return result;
}

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
	result &= testDefaultSingleParam();
	result &= testDefaultMultipleParams();
	result &= testDefaultExpression();

	if (result)
	{
		printf("All Test Passed.\n");
	}
}
