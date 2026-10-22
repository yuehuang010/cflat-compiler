# p3: data_mut() through a pointer alias of a by-value string param writes through / leaks

Pre-existing, found by the T353 review (T353 fixed direct `s.data_mut()` on a by-value param).

## Repro
```cflat
int f(string s) {
    string* p = &s;
    char* d = p->data_mut();   // copy-on-write is not installed for the param slot
    d[0] = 'X';
    return 0;
}
// caller: string a = "abc".copy(); f(a); -> a now reads "Xbc" (writes through to the caller)
// caller: f("lit")                       -> the installed copy leaks
```

## Root cause
markDataMutParamOwning (MainListener_PostfixExpression.cpp) recognizes only a receiver that names the
param directly; a receiver reached through `string*` pointing at the param slot misses the gate.

## Fix direction
Resolve a `p->data_mut()` receiver whose pointer is bound to `&param` (alias tracking) to the param, or
mark the param owning-if-installed whenever its address is taken.
