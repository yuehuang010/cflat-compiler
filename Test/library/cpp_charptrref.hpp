#pragma once

namespace cpp_charptrref {
inline char replacement[] = "replacement";
inline int int_replacement = 19;
inline int ref_slot(const char*& value) { value = replacement; return 11; }
inline int ref_slot_int(const int*& value) { value = &int_replacement; return 14; }
inline int const_ref_slot(const char* const& value) { return value == nullptr ? 21 : 22; }
inline int by_value(const char* value) { return value == nullptr ? 31 : 32; }
inline int mutable_ref_slot(char*& value) { value = replacement; return 41; }
inline char* pointer_rvalue() { return replacement; }
inline int const_ref_rvalue(const char* const& value) { return value == nullptr ? 61 : 62; }
inline int by_value_rvalue(const char* value) { return value == nullptr ? 63 : 64; }
inline int mutable_rvalue_ref(char*&& value) { return value == nullptr ? 71 : 72; }
inline int choose(char*& value) { return 51; }
inline int choose(const char*& value) { return 52; }
inline int choose(const char* value) { return 53; }
struct CprReferenceCtor
{
    int value;
    CprReferenceCtor(const char*& text) { text = replacement; value = 73; }
};
struct CprBase { int value = 81; };
struct CprDerived : CprBase { int derived = 82; };
inline CprBase baseObject;
inline int base_pointer_ref(CprBase*& value) { value = &baseObject; return 71; }
inline int const_base_pointer_ref(const CprBase*& value) { value = &baseObject; return 72; }
inline int const_base_slot_ref(CprBase* const& value) { return value == nullptr ? 80 : 81; }
inline int base_pointer_value(CprBase* value) { return value == nullptr ? 90 : 91; }
inline char* nestedPointerSlot = replacement;
inline char* const* exactNestedPointer = &nestedPointerSlot;
inline int nested_const_pointer_ref(char* const*& value)
{
    return &value == &exactNestedPointer && *value == replacement ? 101 : 102;
}
inline int nested_mutable_pointer_ref(char**& value) { return value == nullptr ? 111 : 112; }
inline int ambiguous(const char* const& value) { return 1; }
inline int ambiguous(const char* value) { return 2; }
inline int mutable_ambiguous(char*& value) { return 3; }
inline int mutable_ambiguous(char* value) { return 4; }
using CprRefSlotFn = int (*)(const char*&);
inline CprRefSlotFn ref_slot_function() { return &ref_slot; }
}
