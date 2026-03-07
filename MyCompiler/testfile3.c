void printf(const char* argv, ...);


//void* malloc(int size)
//{
//    printf("malloc size = %d\n", size);
//    return (void*)size;
//}
//
//void free(void* ptr)
//{
//    printf("free ptr = %p\n", ptr);
//}
//
//void testPointers()
//{
//    int number = 10;
//    printf("number=%d, &number=%p\n", number, &number);
//
//    auto mall = malloc(10);
//    free(mall);
//}



void main()
{
    int number = 10;
    int* ptr = 0;
    if (ptr)
    {
        printf("ptr is true\n");
    }
    else
    {
        printf("ptr is false\n");
    }

    printf("Entering While loop\n");
    ptr = &number;
    while (ptr)
    {
        ptr = nullptr;
        printf("ptr is true\n");
    }
    printf("ptr is false\n");

    // testPointers();
}
