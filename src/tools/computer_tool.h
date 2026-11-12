#pragma once
#include "client/chat_client.h"
#include <nlohmann/json.hpp>

namespace chat {
struct ComputerResult { nlohmann::json output; Attachment screenshot; };
std::string ComputerDefinition();
// The settings animation supplies its own timed frames; agent actions use smooth motion.
ComputerResult RunComputer(const nlohmann::json& args, const Cancellation& cancel, bool smoothPointer=true);
}
