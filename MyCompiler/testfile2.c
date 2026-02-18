void printf(const char* argv, ...);
void* malloc(int size)
{
    printf("malloc size = %d\n", size);
    return (void*)size;
}

void free(void* ptr)
{
    printf("free ptr = %p\n", ptr);
}

void testPointers()
{
    int number = 10;
    printf("number=%d, &number=%p\n", number, &number);

    auto mall = malloc(10);
    free(mall);
}
