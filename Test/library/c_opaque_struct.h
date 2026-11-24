#pragma once
/* A C record that is only ever forward-declared: it stays an unsized opaque shell. */
struct Opq;
typedef struct Opq Opq;
static inline int opq_tag(Opq* p) { return p == 0 ? 42 : 1; }
