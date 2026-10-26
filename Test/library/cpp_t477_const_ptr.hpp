#pragma once

struct T477ProbeInner { int x; };
struct T477ProbeOuter
{
    mutable int m;
    int n;
    T477ProbeInner inner;
    int touch() { return ++m; }
};

inline const T477ProbeOuter t477ProbeObject{1, 2, {3}};
inline const T477ProbeOuter* t477_probe_ptr() { return &t477ProbeObject; }
