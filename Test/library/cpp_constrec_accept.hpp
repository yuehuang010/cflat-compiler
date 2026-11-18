#pragma once
#include "cpp_interop_basic.h"
namespace cppi
{
    inline int cr_take_const_pair(const CrPair* p) { return p->get(); }
}
