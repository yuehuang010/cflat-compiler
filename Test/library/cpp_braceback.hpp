#pragma once
#include <initializer_list>
#include <vector>

namespace braceback {
struct Element {
    int value;
    inline static int made = 0;
    inline static int gone = 0;
    inline static int copies = 0;

    Element(int v) : value(v) { ++made; }
    Element(const Element& other) : value(other.value) { ++made; ++copies; }
    ~Element() { ++gone; }
};

struct ReferenceElement {
    int value;
    inline static int made = 0;
    inline static int gone = 0;

    ReferenceElement(int& source) : value(source) { ++made; ++source; }
    ReferenceElement(const ReferenceElement& other) : value(other.value) { ++made; }
    ~ReferenceElement() { ++gone; }
};

struct OrderedElement {
    int value;
    inline static int events = 0;

    static void event(int digit) { events = events * 10 + digit; }
    OrderedElement(int v) : value(v) { event(v); }
    OrderedElement(const OrderedElement& other) : value(other.value) { event(7); }
    OrderedElement(OrderedElement&& other) : value(other.value) { event(8); }
    ~OrderedElement() { event(value % 10 + 4); }
};

struct DerivedOrderedElement : OrderedElement {
    DerivedOrderedElement(int v) : OrderedElement(v) {}
};

template <class T>
inline const T* keep(std::initializer_list<T> values) { return values.begin(); }

inline int read(const Element* values) { return values[0].value * 10 + values[1].value; }
inline int function_read(std::initializer_list<Element> values) {
    return 200 + values.begin()[0].value * 10 + values.begin()[1].value;
}
inline int reference_read(std::initializer_list<ReferenceElement> values) {
    return values.begin()[0].value * 10
        + (values.size() > 1 ? values.begin()[1].value : 0);
}
inline int ordered_read(const OrderedElement* values) {
    return values[0].value * 10 + values[1].value;
}
inline int ordered_function_read(std::initializer_list<OrderedElement> values) {
    return values.begin()[0].value * 10 + values.begin()[1].value;
}
inline int ordered_function_read_marked(std::initializer_list<OrderedElement> values, int) {
    return values.begin()[0].value * 10 + values.begin()[1].value;
}
inline int ordered_mark() { OrderedElement::event(9); return 0; }
inline void reset_ordered_events() { OrderedElement::events = 0; }
inline int ordered_events() { return OrderedElement::events; }

struct Sink {
    int which;
    int value;
    Sink(std::initializer_list<Element> values)
        : which(3), value(values.begin()[0].value * 10 + values.begin()[1].value) {}
};

using Vec = std::vector<Element>;
inline int vector_read(const Vec& values) {
    return 400 + values[0].value * 10 + values[1].value;
}

inline int made() { return Element::made; }
inline int gone() { return Element::gone; }
inline int copies() { return Element::copies; }
}
