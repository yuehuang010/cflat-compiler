// C++20 equivalent of std_17_95_path_join.cb (compile-time parity baseline)
#include <string>
#include <filesystem>
#include <cstdio>
int main() {
    int failures = 0;
    std::filesystem::path base("dir");
    std::filesystem::path child = base / "file.txt";
    std::string name = child.filename().string();
    if (name != "file.txt") { std::printf("FAIL path_join\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_17_95_path_join\n");
    return failures;
}
