#include "JsonRpcLoop.h"
#include <io.h>
#include <fcntl.h>
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
        {
            char msg[64];
            int len = sprintf_s(msg, sizeof(msg), "[lsp] _read returned %d, errno=%d\n", n, errno);
            _write(2, msg, len);
        }
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
                if (verbose_) { char msg[64]; int len = sprintf_s(msg, sizeof(msg), "[lsp] ReadByte returned -1 in header loop, lineCount=%d\n", lineCount); _write(2, msg, len); }
                return false;
            }
            if (c == '\r')
            {
                int next = ReadByte(verbose_);
                if (next < 0) { if (verbose_) _write(2, "[lsp] ReadByte after CR returned -1\n", 36); return false; }
                if (next == '\n') break;
                line += static_cast<char>(next);
                break;
            }
            line += static_cast<char>(c);
        }

        if (verbose_) { char msg[128]; int len = sprintf_s(msg, sizeof(msg), "[lsp] header line[%d]: '%s'\n", lineCount, line.c_str()); _write(2, msg, len); }
        lineCount++;

        if (line.empty())
            break;  // blank line = end of headers

        const std::string_view prefix = "Content-Length: ";
        if (line.size() > prefix.size() && std::string_view(line).substr(0, prefix.size()) == prefix)
            contentLength = std::stoul(line.substr(prefix.size()));
    }

    if (verbose_) { char msg[64]; int len = sprintf_s(msg, sizeof(msg), "[lsp] contentLength=%zu\n", contentLength); _write(2, msg, len); }

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
    if (verbose_) _write(2, "[lsp] ReadMessage loop starting\n", 32);
    std::string body;
    while (ReadMessage(body))
    {
        if (verbose_) { std::string dbg = "[lsp] got message: " + body.substr(0, 60) + "\n"; _write(2, dbg.c_str(), (unsigned int)dbg.size()); }

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
