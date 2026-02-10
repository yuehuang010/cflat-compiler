void printf(const char* argv, ...);

struct MyStruct
{
    int num1 = 1;
    int num2 = 2;
    int num3 = 3;
};

struct MyStruct2
{
    auto myStruct = MyStruct();
};


MyStruct2 Function()
{
    auto myStruct = MyStruct2();
    return myStruct;
}

int main()
{
    auto mystruct2 = Function();

    printf("mystruct2.myStruct.num1 = %d", mystruct2.myStruct.num1);
	return 0;
}
