void printf(const char* argv, ...);

int testArray()
{
	int arraySize = 30;
	int[arraySize] array;

	int i = 0;
	while (i < arraySize)
	{
		array[i] = i;
		i++;
	}
	
	i = 0;
	while (i < arraySize)
	{
		printf("array[i]=%d\n", i, array[i]);
		i++;
	}

	return 0;
}

void main()
{
	testArray();
}
