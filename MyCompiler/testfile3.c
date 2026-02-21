void printf(const char* argv, ...);

struct MyStruct
{
	int num1 = 1;
	int num2 = 2;
	int num3 = 3;
};


struct MyStruct2
{
	MyStruct myStruct = MyStruct();
};


int testInnerStruct()
{
	MyStruct2 myStruct2 = MyStruct2();
	myStruct2.myStruct.num1 = 100;
	myStruct2.myStruct.num2++;
	// return myStruct2.myStruct.num1;
	return myStruct2.myStruct.num1 + myStruct2.myStruct.num2 + myStruct2.myStruct.num3;
}

int testStruct()
{
	MyStruct myStruct = MyStruct();
	return myStruct.num1 + myStruct.num2 + myStruct.num3;
}

void main()
{
	printf("%d\n", testInnerStruct());
	printf("%d\n", testStruct());
}
