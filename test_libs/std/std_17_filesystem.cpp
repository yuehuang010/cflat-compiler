// C++20 equivalent of std_17_filesystem.cb (compile-time parity baseline)
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdio>
int main() {
    int failures = 0;
    namespace fs = std::filesystem;
    fs::path root = "std_17_filesystem_tmp";
    fs::remove_all(root); fs::create_directory(root);
    fs::path file = root; file.append("sample.txt");
    { std::ofstream out(file); out << "abcde"; }
    auto filename = file.filename().string(); auto ext = file.extension().string(); auto parent = file.parent_path();
    bool exists = fs::exists(file); auto size = fs::file_size(file);
    auto removed = fs::remove_all(root);
#define CHECK(name, expr) do { if (!(expr)) { std::printf("FAIL %s\n", name); failures |= 1; } } while (0)
    CHECK("path_decomposition", filename == "sample.txt" && ext == ".txt" && parent == root);
    CHECK("filesystem_ops", exists && size == 5 && removed == 2);
    if (!failures) std::printf("PASS std_17_filesystem\n");
    return failures;
}
