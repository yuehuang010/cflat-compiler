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

/* c10: C header globals, results and fields carry the const of their native CFlat spelling. */
typedef const char* T10_PCSTR;
struct T10RS { const int a; int b; };
struct T10FS { const int arr[2]; const char* const nm; T10_PCSTR pc; struct { const int x; int y; } in; };
union T10UC { const int k; int m; };
extern const char t10KStr[6];
extern const int* t10KPArr[2];
extern struct T10RS t10RS;
extern struct T10FS t10FS;
extern T10_PCSTR t10Pc;
const char* t10Name(void);
T10_PCSTR t10Name2(void);
const int* t10Ip(void);
const char** t10Pp(void);
int t10TakeMut(char* s);
int t10TakeConst(const char* s);

struct C13PS { int x; int y; };
struct C13PS* c13PSResult(void);
const struct C13PS* c13ConstPSResult(void);
typedef struct C13PS* C13PSP;
C13PSP c13TypedefPSResult(void);
typedef struct { int x; int y; } C13Anon;
C13Anon* c13AnonResult(void);
union C13U { int x; int y; };
union C13U* c13UnionResult(void);
struct C13Opaque;
struct C13Opaque* c13OpaqueResult(void);
