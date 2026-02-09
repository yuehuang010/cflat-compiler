void printf(const char* argv, ...);

int fooInt = 0;
double fooDouble;
float fooFloat = 10.f;

void testGlobalVariable()
{
    int num = fooInt;
    float num2 = fooFloat;
    double num3 = fooDouble;
}

int main()
{
    testGlobalVariable();
	return 0;
}
