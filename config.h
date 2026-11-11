#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace chat {
struct Config {
    std::string key;
    std::string model = "openrouter/auto";
    std::string url = "https://openrouter.ai/api/v1/chat/completions";
    std::vector<std::string> extraTools; // Runtime tool definitions; never persisted.
};
// Saved settings override environment values, which override the UTF-8 .env file.
Config LoadConfig(const std::filesystem::path& directory, bool requireKey = false);
void ValidateConfig(const Config& config, bool requireKey = false);
void SaveConfig(const std::filesystem::path& directory, const Config& config);
}
