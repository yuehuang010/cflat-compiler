namespace cppms
{
    class Value
    {
    public:
        explicit Value(int v) : value(v) {}
        int get() const { return value; }
        int value;
    };

    inline int get(Value item) { return item.get() + 900; }

    class ParenValue
    {
    public:
        explicit ParenValue(int v) : value(v) {}
        int get() const { return value; }
        int value;
    };

    struct Cell
    {
        int x;
        explicit Cell(int v) : x(v) {}
    };

    class Chain
    {
    public:
        explicit Chain(int v) : value(v) {}
        Cell get() const { return Cell(value); }
        int value;
    };

    struct Plain
    {
        int value;
        explicit Plain(int v) : value(v) {}
    };
}

inline int size(cppms::Plain item) { return item.value + 5; }
