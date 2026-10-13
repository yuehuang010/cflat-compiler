namespace t102a {
struct ConvertAssign {
    inline static int ctor_count = 0;
    int value;
    ConvertAssign(int x) : value(x) { ++ctor_count; }
};
inline int convert_assign_value(const ConvertAssign& value) { return value.value; }
inline int convert_assign_ctor_count() { return ConvertAssign::ctor_count; }
}
