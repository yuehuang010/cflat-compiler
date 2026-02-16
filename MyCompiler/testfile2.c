void printf(const char* argv, ...);

struct Negative
{
    int negNumber = 10;

    int GetNumber()
    {
        return negNumber;
    }
};

struct Positive
{
    int posNumber = 5;

    Negative neg = Negative();

    Negative GetNegative()
    {
        return neg;
    }

    int GetNum()
    {
        return posNumber;
    }
};

int main()
{
    int number = 10;
    printf("number=%d\n", number);

    auto pos = Positive();


    int number2 = pos.GetNegative().GetNumber();

    printf("number2=%d\n", number2);
    printf("number3=%d\n", pos.GetNegative().GetNumber());
    auto neg = pos.GetNegative();
    int number4 = neg.GetNumber();
    printf("number4=%d\n", number4);
    return 0;
}
