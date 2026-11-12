#pragma once
#include "client/chat_client.h"

namespace chat {
struct ToolResult {
    std::wstring output;
    unsigned exitCode = 1;
    bool timedOut = false, cancelled = false, truncated = false;
};
ToolResult RunPowerShell(const std::wstring& command, const std::filesystem::path& directory,
    const Cancellation& cancel, unsigned timeoutSeconds = 30);
}
