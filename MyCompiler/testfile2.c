void printf(const char* argv, ...);

struct MyStruct
{
    int num1 = 1;
    int num2 = 2;
    int num3 = 3;

    int Total() 
    {
        return num1 + num2 + num3;
        // return 10;
    }
};

int main()
{
    auto myStruct = MyStruct();

    int total;
    total = myStruct.Total();
    // total = Total(myStruct);
    printf("myStruct.Total() = %d", total);
	return 0;
}
