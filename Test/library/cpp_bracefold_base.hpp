#pragma once

#include <string>

namespace bracefold
{
struct Base
{
    std::string value_;
    int selected_;

    explicit Base(const char* value) : value_(value), selected_(1) {}
    explicit Base(const std::string& value) : value_(value), selected_(2) {}

    int selected() const { return selected_; }
    bool matches(const char* expected) const { return value_ == expected; }
};
}
