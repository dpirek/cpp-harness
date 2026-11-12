#pragma once
#include "config/config.h"
#include "client/call_metrics.h"
#include "common/attachments.h"
#include <windows.h>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace chat {
struct ToolCall { std::string id, name, arguments; };
struct Message {
    std::wstring role, text;
    std::vector<ToolCall> toolCalls;
    std::string toolCallId;
    std::wstring activity; // Local UI status; never serialized into API history.
    std::vector<Attachment> attachments;
};
struct Reply { std::wstring text; bool ok = false; bool cancelled = false; std::vector<ToolCall> toolCalls; CallMetrics metrics; };
using OnDelta = std::function<void(const std::wstring&)>;
class Cancellation {
public:
    Cancellation();
    ~Cancellation();
    Cancellation(const Cancellation&) = delete;
    Cancellation& operator=(const Cancellation&) = delete;
    void Cancel() const noexcept;
    bool IsCancelled() const noexcept;
    HANDLE Event() const noexcept { return event_; }
private:
    HANDLE event_;
};
// Explicit endpoint overrides support loopback transport tests.
struct Endpoint {
    std::wstring host = L"openrouter.ai";
    unsigned short port = 443;
    std::wstring path = L"/api/v1/chat/completions";
    bool secure = true;
    unsigned timeoutMs = 120000;
};
Endpoint ProviderEndpoint(const std::string& url);
// Accept a base URL or a full Chat Completions URL; preserve query parameters.
Endpoint InferenceEndpoint(const std::string& url);
std::wstring ProviderAuthHeaders(const Config& config,const Endpoint& endpoint);
std::vector<std::string> FetchModels(const Config& config, const Cancellation& cancel);
// MCP JSON-RPC POST; preserves the server-assigned session and negotiated version.
std::string McpHttpPost(const std::string& url,const std::vector<std::pair<std::string,std::string>>& headers,
    std::string& session,const std::string& version,const std::string& payload,const Cancellation& cancel);
std::string BuildPayload(const Config& config, const std::vector<Message>& messages);
Reply ParseReply(unsigned status, const std::string& body);
Reply Complete(const Config& config, const std::vector<Message>& messages,
    const Cancellation& cancellation, const Endpoint& endpoint = {}, const OnDelta& onDelta = {});
}
