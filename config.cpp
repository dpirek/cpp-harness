#include "config.h"
#include "text_encoding.h"
#include <fstream>
#include <map>
#include <winhttp.h>
#include <wincrypt.h>
#include "third_party/nlohmann/json.hpp"
#pragma comment(lib,"crypt32.lib")

namespace chat {
namespace {
std::string Trim(std::string value) {
    auto start = value.find_first_not_of(" \t\r\n");
    if (start == value.npos) return {};
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}
std::string Environment(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
    if (!copied || copied >= size) return {};
    value.resize(copied);
    return Utf8(value);
}
}
void ValidateConfig(const Config& config,bool requireKey) {
    if(requireKey&&config.key.empty())throw std::runtime_error("Open Settings and enter an API key.");
    if(config.key.find_first_of("\r\n")!=config.key.npos)throw std::runtime_error("The API key contains an invalid newline.");
    if(Trim(config.model).empty())throw std::runtime_error("Enter a model ID.");
    auto url=Wide(config.url);URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    parts.dwHostNameLength=parts.dwUserNameLength=parts.dwPasswordLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=static_cast<DWORD>(-1);
    if(config.url.find_first_of("\r\n\t ")!=config.url.npos||!WinHttpCrackUrl(url.c_str(),0,0,&parts)||
       (parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)||!parts.dwHostNameLength||parts.dwUserNameLength||parts.dwPasswordLength||config.url.find('#')!=config.url.npos)
        throw std::runtime_error("Enter a valid HTTP or HTTPS chat completions URL without credentials or a fragment.");
}
void SaveConfig(const std::filesystem::path& directory,const Config& config) {
    ValidateConfig(config);
    DATA_BLOB input{static_cast<DWORD>(config.key.size()),reinterpret_cast<BYTE*>(const_cast<char*>(config.key.data()))},encrypted{};
    if(!CryptProtectData(&input,L"Chat provider key",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted))throw std::runtime_error("Could not protect the API key.");
    std::vector<unsigned char> bytes(encrypted.pbData,encrypted.pbData+encrypted.cbData);LocalFree(encrypted.pbData);
    auto json=nlohmann::json{{"url",config.url},{"model",config.model},{"protected_key",bytes}};
    auto temporary=directory/L"provider-settings.json.tmp",destination=directory/L"provider-settings.json";
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<json.dump(2);file.flush();if(!file)throw std::runtime_error("Could not save provider settings.");}
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not replace provider settings.");
}
Config LoadConfig(const std::filesystem::path& directory,bool requireKey) {
    auto settings=directory/L"provider-settings.json";
    if(std::filesystem::exists(settings)) {
        try {
            std::ifstream file(settings,std::ios::binary);auto json=nlohmann::json::parse(file);
            Config config;config.url=json.at("url").get<std::string>();config.model=json.at("model").get<std::string>();
            auto bytes=json.at("protected_key").get<std::vector<unsigned char>>();
            DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()),bytes.data()},plain{};
            if(!CryptUnprotectData(&encrypted,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))throw std::runtime_error("key");
            config.key.assign(reinterpret_cast<char*>(plain.pbData),plain.cbData);SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
            ValidateConfig(config,requireKey);return config;
        }catch(...){throw std::runtime_error("Could not read saved provider settings. Open Settings to save them again.");}
    }
    std::map<std::string, std::string> values;
    auto file = directory / L".env";
    std::ifstream stream(file, std::ios::binary);
    if (!stream && std::filesystem::exists(file)) throw std::runtime_error("Could not read the project .env file.");
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first = false;
        line = Trim(line);
        if (line.empty() || line.front() == '#') continue;
        if (line.compare(0, 7, "export ") == 0) line = Trim(line.substr(7));
        auto equal = line.find('=');
        if (equal == line.npos) continue;
        auto name = Trim(line.substr(0, equal));
        auto value = Trim(line.substr(equal + 1));
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front()) value = value.substr(1, value.size() - 2);
        else {
            auto comment = value.find(" #");
            if (comment != value.npos) value = Trim(value.substr(0, comment));
        }
        values[name] = value;
    }
    Config config;
    auto resolve=[&](std::initializer_list<std::pair<const wchar_t*,const char*>> names) {
        for(const auto& name:names) {auto value=Environment(name.first);if(!value.empty())return value;}
        for(const auto& name:names) {auto value=values[name.second];if(!value.empty())return value;}
        return std::string{};
    };
    config.key=resolve({{L"AI_API_KEY","AI_API_KEY"},{L"OPEN_ROUTER_KEY","OPEN_ROUTER_KEY"},{L"OPENROUTER_API_KEY","OPENROUTER_API_KEY"}});
    auto model=resolve({{L"AI_MODEL","AI_MODEL"},{L"OPENROUTER_MODEL","OPENROUTER_MODEL"}});
    if(!model.empty())config.model=model;
    auto url=resolve({{L"AI_BASE_URL","AI_BASE_URL"},{L"AI_PROVIDER_URL","AI_PROVIDER_URL"}});
    if(!url.empty())config.url=url;
    ValidateConfig(config,requireKey);
    return config;
}
}
