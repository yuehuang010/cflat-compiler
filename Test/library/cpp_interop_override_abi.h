#pragma once

namespace cppoa
{
    struct Small8 { int a; int b; };
    struct Small16 { long long a; long long b; };
    struct Large24 { long long a; long long b; long long c; };

    class Dtor
    {
    public:
        static int dtorCount;
        int value;
        Dtor(int v) : value(v) {}
        Dtor(const Dtor& other) : value(other.value) {}
        ~Dtor() { ++dtorCount; }
    };
    inline int Dtor::dtorCount = 0;

    class Base
    {
    public:
        virtual ~Base() = default;
        virtual void set_value(int* out) { *out = 0; }
        virtual int integer() { return 0; }
        virtual double floating() { return 0.0; }
        virtual int* pointer() { return nullptr; }
        virtual int& reference() { static int value = 0; return value; }
        virtual Small8 small8() { return {}; }
        virtual Small16 small16() { return {}; }
        virtual Large24 large24() { return {}; }
        virtual int byvalue(Dtor value) { return value.value; }
    };

    template<class T>
    class GenericBase
    {
    public:
        virtual T value(Dtor value) { return T{}; }
    };

    template<class T>
    inline T call_generic_value(GenericBase<T>* value, Dtor argument)
    {
        return value->value(argument);
    }

    inline void call_set_value(Base* value, int* out)
    {
        value->set_value(out);
    }

    inline int call_small8(Base* value)
    {
        Small8 result = value->small8();
        return result.a * 10 + result.b;
    }

    inline int call_integer(Base* value)
    {
        return value->integer();
    }

    inline int call_large24(Base* value)
    {
        Large24 result = value->large24();
        return static_cast<int>(result.a * 1000000 + result.b * 1000 + result.c);
    }

    inline int call_small16(Base* value)
    {
        Small16 result = value->small16();
        return static_cast<int>(result.a * 1000000 + result.b);
    }

    inline int call_floating(Base* value)
    {
        return static_cast<int>(value->floating() * 2.0);
    }

    inline int call_pointer(Base* value)
    {
        int* result = value->pointer();
        return result == nullptr ? -1 : *result;
    }

    inline int call_reference(Base* value)
    {
        return value->reference();
    }

    inline int call_byvalue(Base* value)
    {
        Dtor input(23);
        return value->byvalue(input);
    }

    inline void reset_dtor_count() { Dtor::dtorCount = 0; }
    inline int dtor_count() { return Dtor::dtorCount; }
}
