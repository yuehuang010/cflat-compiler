#pragma once
namespace t53_first {
enum class SizeofE { value = 4 };
enum class DeclE { value = 5 };
enum class TemplateE { value = 6 };
template <class T> struct Box { T value{}; };
}
