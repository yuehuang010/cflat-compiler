#include "JsonRpcLoop.h"
#include "platform/PlatformIo.h"
#include <format>
#include <iostream>

JsonRpcLoop::JsonRpcLoop(int protocolFd, bool verbose)
    : protocolFd_(protocolFd)
    , verbose_(verbose)
{
}

JsonRpcLoop::~JsonRpcLoop()
{
    if (protocolFd_ >= 0)
        _close(protocolFd_);
}

static int ReadByte(bool verbose)
{
    unsigned char b;
    int n = _read(0, &b, 1);
    if (n != 1)
    {
        if (verbose)
            std::cerr << std::format("[lsp] _read returned {}, errno={}\n", n, errno);
        return -1;
    }
    return static_cast<int>(b);
}

bool JsonRpcLoop::ReadMessage(std::string& out)
{
    size_t contentLength = 0;
    std::string line;
    int lineCount = 0;
    while (true)
    {
        line.clear();
        while (true)
        {
            int c = ReadByte(verbose_);
            if (c < 0)
            {
                if (verbose_) std::cerr << std::format("[lsp] ReadByte returned -1 in header loop, lineCount={}\n", lineCount);
                return false;
            }
            if (c == '\r')
            {
                int next = ReadByte(verbose_);
                if (next < 0) { if (verbose_) std::cerr << "[lsp] ReadByte after CR returned -1\n"; return false; }
                if (next == '\n') break;
                line += static_cast<char>(next);
                break;
            }
            line += static_cast<char>(c);
        }

        if (verbose_) std::cerr << std::format("[lsp] header line[{}]: '{}'\n", lineCount, line);
        lineCount++;

        if (line.empty())
            break;  // blank line = end of headers

        const std::string_view prefix = "Content-Length: ";
        if (line.size() > prefix.size() && line.starts_with(prefix))
            contentLength = std::stoul(line.substr(prefix.size()));
    }

    if (verbose_) std::cerr << std::format("[lsp] contentLength={}\n", contentLength);

    if (contentLength == 0)
        return false;

    out.resize(contentLength);
    size_t read = 0;
    while (read < contentLength)
    {
        int n = _read(0, out.data() + read, static_cast<unsigned int>(contentLength - read));
        if (n <= 0)
            return false;
        read += n;
    }
    return true;
}

void JsonRpcLoop::Run(MessageHandler handler)
{
    if (verbose_) std::cerr << "[lsp] ReadMessage loop starting\n";
    std::string body;
    while (ReadMessage(body))
    {
        if (verbose_) std::cerr << "[lsp] got message: " << body.substr(0, 60) << "\n";

        nlohmann::json parsed;
        try
        {
            parsed = nlohmann::json::parse(body);
        }
        catch (const nlohmann::json::parse_error& e)
        {
            std::cerr << "[lsp] failed to parse JSON: " << e.what() << "\n";
            continue;
        }

        if (!parsed.is_object())
        {
            std::cerr << "[lsp] JSON is not an object\n";
            continue;
        }

        std::string method = parsed.value("method", "");
        handler(parsed);

        if (method == "exit")
            break;
    }
}

void JsonRpcLoop::Send(nlohmann::json msg)
{
    std::string body = msg.dump();

    std::string frame = std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);

    std::lock_guard<std::mutex> lock(writeMutex_);
    _write(protocolFd_, frame.data(), static_cast<unsigned int>(frame.size()));
}
