#pragma once
#include "client/chat_client.h"
#include <nlohmann/json.hpp>

namespace chat {
// Standard {"mcpServers":{"name":{"command":...,"args":[],"env":{}}}} configuration.
// Remote entries use url and optional headers instead of command/args/env.
void ValidateMcpConfig(const nlohmann::json& config);
nlohmann::json LoadMcpConfig(const std::filesystem::path& directory);
void SaveMcpConfig(const std::filesystem::path& directory,const nlohmann::json& config);
using McpStatus=std::function<void(const std::wstring&)>;
class McpClient {
public:
    McpClient(const nlohmann::json& config,const std::filesystem::path& directory,
        const Cancellation& cancel,const McpStatus& status={});
    ~McpClient();
    McpClient(const McpClient&)=delete;
    McpClient& operator=(const McpClient&)=delete;
    std::vector<std::string> Definitions() const;
    bool HasTool(const std::string& name) const;
    std::string Label(const std::string& name) const;
    nlohmann::json Call(const std::string& name,const nlohmann::json& arguments);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
