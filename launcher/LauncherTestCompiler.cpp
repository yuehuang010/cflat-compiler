#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    fs::path workingDirectory = fs::current_path();
    fs::path versionPath = workingDirectory / "version.txt";
    std::string version = "unknown";
    {
        std::ifstream input(versionPath);
        if (input) std::getline(input, version);
    }

    for (int i = 1; i < argc; ++i)
    {
        std::string argument = argv[i];
        if (argument == "--init-local")
        {
            std::error_code ec;
            fs::create_directories(workingDirectory / ".cflat", ec);
            if (ec)
            {
                std::cerr << "test compiler: init failed: " << ec.message() << "\n";
                return 11;
            }
            std::ofstream marker(workingDirectory / ".cflat" / "initialized", std::ios::binary);
            marker << version << "\n";
            return marker ? 0 : 12;
        }
        if (argument == "--version")
        {
            std::ofstream marker(workingDirectory / "run.marker", std::ios::binary | std::ios::app);
            marker << version << "\n";
            std::cout << version << "\n";
            return 0;
        }
        if (argument == "--exit-code")
        {
            if (i + 1 >= argc) return 13;
            return std::stoi(argv[++i]);
        }
    }
    return 0;
}
