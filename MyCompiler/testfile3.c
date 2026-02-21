void printf(const char* argv, ...);

//int testArray()
//{
//	int arraySize = 30;
//	int[arraySize] array;
//
//	int i = 0;
//	while (i < arraySize)
//	{
//		array[i] = i;
//		i++;
//	}
//	
//	i = 0;
//	while (i < arraySize)
//	{
//		printf("array[%d]=%d\n", i, array[i]);
//		i++;
//	}
//
//	return 0;
//}

//int testDoWhileLoop()
//{
//	int count = 30;
//	do
//	{
//		printf("%d\n", count);
//		count--;
//	}
//	while (count > 0);
//
//	return 0;
//}

int testForLoop()
{
	int sum = 0;
	for (int i = 0; i < 30; i++)
	{
		sum += i;
	}

	return sum;
}

void main()
{
	// testArray();
	// testDoWhileLoop();
	testForLoop();
}
