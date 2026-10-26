#pragma once
struct T450R2Eq
{
    mutable int mark = 0;
    bool operator==(int) { mark = 1; return true; }
    bool operator==(int) const { mark = 2; return true; }
};

struct T450R2Spaceship
{
    mutable int mark = 0;
    int operator<=>(int) { mark = 3; return -1; }
    int operator<=>(int) const { mark = 4; return -1; }
};

struct T450R2Free {};
inline int operator+(T450R2Free&, int) { return 31; }
inline int operator+(const T450R2Free&, int) { return 42; }

struct T450R2Compound
{
    mutable int mark = 0;
    int operator+=(int) { mark = 1; return 1; }
    int operator+=(int) const { mark = 2; return 2; }
};

struct T450R2Holder
{
    mutable T450R2Eq eq;
    mutable T450R2Spaceship spaceship;
    mutable T450R2Free freeMut;
    T450R2Free freeNorm;
    mutable T450R2Compound compoundMut;
    T450R2Compound compoundNorm;
    const T450R2Compound compoundFixed{};
};

inline const T450R2Holder t450r2Holder{};
inline T450R2Holder t450r2MutableHolder{};

struct T450R2Pick
{
    int pick() { return 11; }
    int pick() const { return 22; }
    int operator+=(int) { return 81; }
    int operator+=(int) const { return 82; }
};

inline const T450R2Pick t450r2PickArray[2]{};
struct T450R2PointerHolder { mutable const T450R2Pick* mcp; };
inline const T450R2PointerHolder pp{t450r2PickArray};
inline const T450R2Pick* const gpc = t450r2PickArray;
inline const T450R2Pick* gpm = t450r2PickArray;
inline T450R2Pick t450r2Array[2]{};

namespace t450r2_tpl
{
struct TemplateFree {};
template<class T> inline int operator+(T&, int) { return 51; }
template<class T> inline int operator+(const T&, int) { return 62; }
struct TemplateHolder { mutable TemplateFree m; TemplateFree n; };
inline const TemplateHolder templateHolder{};
}
