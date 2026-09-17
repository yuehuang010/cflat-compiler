// Second user group for section M101: a nested specialization naming classes from TWO user
// headers needs both groups unioned into the request. No standard header here either.
#pragma once

namespace nest2 {

struct Tag
{
    int t;
    Tag() : t(11) {}
    Tag(int v) : t(v) {}
};

}
