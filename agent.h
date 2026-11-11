#pragma once
#include "chat_client.h"

namespace chat {
enum class UpdateKind { Delta, Status, SegmentEnd, Tool };
struct AgentUpdate { UpdateKind kind; std::wstring text; };
struct AgentResult { Reply reply; std::vector<Message> history; };
using OnAgentUpdate = std::function<void(AgentUpdate)>;
AgentResult RunAgent(const Config& config, std::vector<Message> history,
    const std::filesystem::path& directory, const Cancellation& cancel,
    const Endpoint& endpoint = {}, const OnAgentUpdate& onUpdate = {});
}
