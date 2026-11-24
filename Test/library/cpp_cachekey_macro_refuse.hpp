#pragma once
// CACHE-KEY: CKEYM_LIMIT is found here but neither a constant nor an lvalue, so it is refused.
extern int ckeym_g;
#define CKEYM_LIMIT (ckeym_g + 1)
