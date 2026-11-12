#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace chat {
std::string DefaultSystemPrompt();
struct Config {
    std::string key;
    std::string model = "openrouter/auto";
    std::string url = "https://openrouter.ai/api/v1/chat/completions";
    std::string authHeader = "auto"; // auto, Authorization (Bearer), none, or a custom raw-key header.
    std::string modelsUrl; // Optional exact model discovery URL.
    std::string tokenLimitField = "max_tokens"; // max_completion_tokens for newer reasoning models, or none.
    bool streamUsage = true;
    std::string systemPrompt = DefaultSystemPrompt();
    int maxModelTurns = 8;
    int maxToolCalls = 16;
    std::vector<std::string> extraTools; // Runtime tool definitions; never persisted.
};
// Saved settings override environment values, which override the UTF-8 .env file.
Config LoadConfig(const std::filesystem::path& directory, bool requireKey = false);
void ValidateConfig(const Config& config, bool requireKey = false);
void SaveConfig(const std::filesystem::path& directory, const Config& config);
}
