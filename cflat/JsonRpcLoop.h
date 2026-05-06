#pragma once
#include <string>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>

// Reads Content-Length-framed JSON-RPC messages from stdin,
// dispatches each to the handler, and provides a thread-safe
// write method for sending responses/notifications.
class JsonRpcLoop
{
public:
    // protocolFd: a dup'd file descriptor for binary-mode stdout writes
    explicit JsonRpcLoop(int protocolFd, bool verbose = false);
    ~JsonRpcLoop();

    using MessageHandler = std::function<void(const nlohmann::json&)>;

    // Blocking loop — returns when "exit" message is received or stdin closes.
    void Run(MessageHandler handler);

    // Thread-safe: send a JSON-RPC message (response or notification).
    void Send(nlohmann::json msg);

private:
    bool ReadMessage(std::string& out);

    int protocolFd_;
    bool verbose_;
    std::mutex writeMutex_;
};
