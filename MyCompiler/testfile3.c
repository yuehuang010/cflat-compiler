extern void printf(const char* argv, ...);

int Add(int a, int b)
{
	printf("Adding Int.\n");
	return a + b;
}

float Add(float a, float b)
{
	printf("Adding float.\n");
	return a + b;
}

struct MyStruct1
{
	int num = 1;
	int Read() { printf("Reading MyStruct1.\n");  return num; }
};

struct MyStruct2
{
	int num = 2;
	int Read() { printf("Reading MyStruct2.\n"); return num; }
};

int ReadExt(MyStruct1 my) { printf("ReadExt MyStruct1.\n"); return my.num; }
int ReadExt(MyStruct2 my) { printf("ReadExt MyStruct2.\n"); return my.num; }

extern void main()
{
	MyStruct1 struct1 = MyStruct1();
	MyStruct2 struct2 = MyStruct2();

	printf("struct1.Read=%d\n", struct1.Read());
	printf("struct2.Read=%d\n", struct2.Read());
	printf("struct1.ReadExt=%d\n", struct1.ReadExt());
	printf("struct2.ReadExt=%d\n", struct2.ReadExt());

	auto number = Add(1, 2);
	printf("number=%d\n", number);

	auto number_float = Add(1.f, 2.f);
	printf("number=%f\n", number_float);
}
