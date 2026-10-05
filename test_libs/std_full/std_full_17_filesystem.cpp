#include <string>
#include <cstdio>
#include <filesystem>
#include <fstream>
int main()
{
    int failures = 0;
    namespace fs = std::filesystem;
    fs::path root("scratch/std_full_C2/filesystem");
    fs::remove_all(root);
    fs::create_directories(root / "sub" / "deep");
    fs::path file = root / "sub" / "item.txt";
    { std::ofstream out(file); out << "abcdef"; }
    fs::path renamed = file;
    renamed.replace_extension(".dat");
    if (file.stem() != "item" || file.extension() != ".txt" || renamed.extension() != ".dat") failures |= 1;
    fs::path normalized = (root / "sub" / ".." / "sub" / "item.txt").lexically_normal();
    fs::path relative = file.lexically_relative(root);
    if (normalized != file.lexically_normal() || relative.generic_string() != "sub/item.txt" || file.generic_string().empty()) failures |= 2;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec || !fs::is_regular_file(file, ec) || ec || !fs::exists(fs::status(file, ec))) failures |= 4;
    auto time = fs::last_write_time(file, ec);
    if (ec) failures |= 4;
    fs::path copy = root / "copy.txt";
    fs::copy_file(file, copy, fs::copy_options::none, ec);
    if (ec || fs::file_size(copy) != 6) failures |= 8;
    fs::resize_file(copy, 3, ec);
    if (ec || fs::file_size(copy) != 3) failures |= 8;
    fs::path moved = root / "moved.txt";
    fs::rename(copy, moved, ec);
    if (ec || !fs::exists(moved)) failures |= 8;
    int count = 0;
    for (auto i = fs::recursive_directory_iterator(root); i != fs::recursive_directory_iterator(); ++i) ++count;
    if (count != 4) failures |= 16;
    ec.clear();
    bool missing_exists = fs::exists(root / "missing", ec);
    if (missing_exists || ec) failures |= 32;
    auto missing_size = fs::file_size(root / "missing", ec);
    if (!ec) failures |= 32;
    ec.clear();
    if (fs::temp_directory_path().empty() || fs::current_path().empty()) failures |= 64;
    fs::remove_all(root);
    if (failures & 1) std::puts("FAIL path decomposition");
    if (failures & 2) std::puts("FAIL lexical path operations");
    if (failures & 4) std::puts("FAIL filesystem status/clock");
    if (failures & 8) std::puts("FAIL copy/resize/rename");
    if (failures & 16) std::puts("FAIL recursive iteration count");
    if (failures & 32) std::puts("FAIL error_code overloads");
    if (failures & 64) std::puts("FAIL filesystem paths");
    if (failures == 0) std::puts("PASS std_full_17_filesystem");
    return failures;
}
