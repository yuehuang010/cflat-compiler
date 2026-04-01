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

int add(int a, int b) { return a + b + 100; }  // global add returns a different value

namespace MathUtils
{
	int add(int a, int b) { return a + b; }
	int multiply(int a, int b) { return a * b; }

	struct MyNumber
	{
		int value = default;
	};

	namespace Advanced
	{
		int square(int x) { return x * x; }
		int cube(int x) { return x * x * x; }
	}
}

using Math = MathUtils;          // global alias for top-level namespace
using MathAdv = MathUtils.Advanced;  // global alias for nested namespace

bool testNamespace()
{
	bool result = true;

	MathAdv.MyNumber num = MathAdv.MyNumber();
	num.value = 5;
	result &= Test("num.value", num.value, 5);            // 3+4=7

	result &= Test("namespace_add", MathUtils.add(3, 4), 7);            // 3+4=7
	result &= Test("namespace_multiply", MathUtils.multiply(3, 4), 12); // 3*4=12
	result &= Test("global_add", add(3, 4), 107);                       // 3+4+100=107
	result &= Test("no_collision", MathUtils.add(3, 4) != add(3, 4), 1);
	result &= Test("global_using_add", Math.add(3, 4), 7);              // global alias resolves to MathUtils.add
	result &= Test("global_using_multiply", Math.multiply(3, 4), 12);   // global alias resolves to MathUtils.multiply
	return result;
}

extern void main()
{
	testNamespace();
	return;
}


