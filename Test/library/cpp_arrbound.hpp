// ARRBOUND fixtures: a mutable static (refused as a bound) and constant C++ values (folded bounds).
namespace arrb {
struct Mut { static int n; };
inline int Mut::n = 3;
struct K { static constexpr int n = 3; static const int m = 4; };
constexpr int gk = 5;
enum class Col { R = 2 };
}
