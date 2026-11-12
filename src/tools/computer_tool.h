#pragma once
#include "client/chat_client.h"
#include <nlohmann/json.hpp>

namespace chat {
struct ComputerResult { nlohmann::json output; Attachment screenshot; };
std::string ComputerDefinition();
ComputerResult RunComputer(const nlohmann::json& args, const Cancellation& cancel);
}
