#pragma once

extern int t168KCArray[3];
typedef struct T168KCArrayPair { int x; int y; } T168KCArrayPair;
extern T168KCArrayPair t168KCPairs[2];
int t168KCArrayRead(int index);
int t168KCArrayAt(int values[]);
int t168KCPairSum(T168KCArrayPair values[]);
extern const int t183KCScalar;
extern const int t183KCArray[3];
extern const int* t183KCPtr;
extern int* const t183KCConstPtr;
extern int t183KCMutable;
extern int t183KCSecond;
int t183KCTakeMutable(int* p);
int t183KCTakeConst(const int* p);
