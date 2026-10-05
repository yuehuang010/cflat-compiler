#pragma once
#include <type_traits>

namespace cpp_errc_probe {
template <class T> struct is_error_condition_enum : std::false_type {};

enum class ConditionEnum { accepted = 41 };
template <> struct is_error_condition_enum<ConditionEnum> : std::true_type {};

enum PlainConditionEnum { plain_accepted = 43 };
template <> struct is_error_condition_enum<PlainConditionEnum> : std::true_type {};

enum class UnregisteredEnum { rejected = 47 };

struct ErrorCondition {
    int value_ = 0;
    ErrorCondition() = default;
    template <class E, typename std::enable_if<is_error_condition_enum<E>::value, int>::type = 0>
    ErrorCondition(E value) : value_(static_cast<int>(value)) {}
    int value() const { return value_; }
};

struct ErrorCode {
    bool value_ = false;
    operator bool() const { return value_; }
};

inline bool operator==(const ErrorCondition& left, const ErrorCondition& right)
{ return left.value_ == right.value_; }
inline bool operator==(const ErrorCode& left, const ErrorCondition& right)
{ return left.value_ && right.value_ == 41; }
inline bool operator==(const ErrorCondition& left, const ErrorCode& right)
{ return right == left; }
inline bool operator<(const ErrorCondition& left, const ErrorCondition& right)
{ return left.value_ < right.value_; }
inline int by_const_ref(const ErrorCondition& value) { return value.value_; }
inline int by_value(ErrorCondition value) { return value.value_; }
} // namespace cpp_errc_probe
