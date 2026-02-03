void printf(const char* argv, ...);

float g_num_f = 111.f;

/*
int : 100
int : 0X000064
long : 100
long : 0X00000000000064
float : 100.000000
float : 0X42C80000
double : 100.000000
double : 0X4059000000000000
*/

struct myStruct
{
	int num1 = 1;
	int num2 = 2;
	int num3 = 3;
	int result = 0;
};

int main()
{
	auto myS = myStruct();
	myS.num1++;
	myS.result = myS.num1 + myS.num2 + myS.num3;
	int num = myS.num1 + myS.num2 + myS.num3;

	return 0;
}
