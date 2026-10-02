// C++20 equivalent of std_17_96_directory_iterator.cb (compile-time parity baseline)
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdio>
int main() {
    int failures = 0;
    std::filesystem::path root = "std_17_directory_iterator_tmp";
    std::filesystem::remove_all(root); std::filesystem::create_directory(root);
    std::filesystem::path file = root / "entry.txt";
    { std::ofstream out(file); out << "entry"; }
    std::filesystem::directory_iterator entries(root);
    int count = 0;
    for (auto entry : entries) { (void)entry; ++count; }
    std::filesystem::remove_all(root);
    if (count != 1) { std::printf("FAIL directory_iterator: got %d want 1\n", count); failures |= 1; }
    if (!failures) std::printf("PASS std_17_96_directory_iterator\n");
    return failures;
}
