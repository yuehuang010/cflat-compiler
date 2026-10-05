#include <string>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <sstream>
int main()
{
    int failures = 0;
    std::ostringstream explicit_out;
    explicit_out << "output";
    if (explicit_out.str() != "output") failures |= 16;
    std::stringstream ss;
    ss << "first";
    if (ss.str() != "first") failures |= 1;
    ss.str("second");
    std::string value;
    ss >> value;
    if (value != "second") failures |= 1;
    std::string source = "iterator";
    std::istringstream input(source);
    std::string copied{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (copied != source) failures |= 4;

    std::filesystem::create_directories("scratch/std_full_C2");
    const char* path = "scratch/std_full_C2/streams.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        bool opened = out.is_open();
        out.write("abcdef", 6);
        out.close();
        if (!opened || out.is_open()) failures |= 8;
    }
    {
        std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
        char part[3] = {};
        io.read(part, 3);
        if (!io.is_open() || io.gcount() != 3 || part[0] != 'a' || part[1] != 'b' || part[2] != 'c' || io.tellg() != std::streampos(3)) failures |= 8;
        io.seekg(0);
        char first = 0;
        io.get(first);
        if (first != 'a') failures |= 8;
        io.close();
    }
    {
        std::ofstream out(path, std::ios::app | std::ios::binary);
        out.put('g');
        out.close();
    }
    std::ifstream in;
    in.open(path, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (all != "abcdefg") failures |= 8;
    in.close();
    std::remove(path);
    if (failures & 1) std::puts("FAIL stringstream output/reset");
    if (failures & 2) std::puts("FAIL stringbuf operations");
    if (failures & 4) std::puts("FAIL istreambuf_iterator");
    if (failures & 8) std::puts("FAIL file streams");
    if (failures & 16) std::puts("FAIL ostringstream");
    if (failures == 0) std::puts("PASS std_full_11_file_and_string_streams");
    return failures;
}
