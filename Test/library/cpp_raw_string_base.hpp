#pragma once

#include <string>

namespace rawstr
{
struct Base
{
    int selected_ = 0;
    std::string value_;

    explicit Base(const char* value) : selected_(1), value_(value) {}
    explicit Base(const std::string& value) : selected_(2), value_(value) {}

    int selected() const { return selected_; }
    bool matches() const { return value_ == "raw \"quoted\" \\tail"; }
};
}
