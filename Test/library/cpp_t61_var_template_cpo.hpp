#pragma once

#include <ranges>
#include <sstream>

// Shaped like std::views::istream<T>: the inner variable-template CPO `views::probe<T>` shares
// its name with the outer stream type `cpp_t61::probe`, and `views` is a namespace alias.
namespace cpp_t61
{
    template <class C> struct basic_probe_stream : std::basic_istringstream<C>
    {
        explicit basic_probe_stream(const C* text) : std::basic_istringstream<C>(text) {}
    };
    using probe = basic_probe_stream<char>;
    namespace ranges
    {
        namespace views
        {
            template <class T> struct probe_cpo
            {
                auto operator()(std::istream& input) const
                {
                    return std::ranges::istream_view<T>(input);
                }
            };
            template <class T> inline constexpr probe_cpo<T> probe{};
        }
    }
    namespace views = ranges::views;
}
