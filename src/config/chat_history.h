#pragma once
#include "client/chat_client.h"
#include "common/text_encoding.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <random>

namespace chat {
struct SavedChat {
    std::string id;
    std::wstring title, draft;
    std::vector<Message> messages, context;
};
struct ChatHistory {
    std::string activeId;
    std::vector<SavedChat> chats;
};
inline bool ValidChatId(const std::string& id) {
    return id.size()==5&&id.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")==id.npos;
}
inline std::filesystem::path ChatWorkspace(const std::filesystem::path& directory,const std::string& id) {
    if(!ValidChatId(id))throw std::runtime_error("Invalid chat workspace ID.");
    auto path=directory/L".workspace"/id;std::filesystem::create_directories(path);return path;
}
inline SavedChat NewSavedChat(const std::filesystem::path& directory,const ChatHistory& history) {
    static constexpr char alphabet[]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device random;
    for(int attempt=0;attempt<100;++attempt) {
        std::string id;for(int i=0;i<5;++i)id+=alphabet[random()%62];
        bool exists=false;for(const auto& chat:history.chats)if(chat.id==id)exists=true;
        if(exists||std::filesystem::exists(directory/L".workspace"/id))continue;
        std::filesystem::create_directories(directory/L".workspace");
        if(std::filesystem::create_directory(directory/L".workspace"/id))return {id,L"New conversation",{},{},{}};
    }
    throw std::runtime_error("Could not allocate a unique chat workspace.");
}
inline nlohmann::json HistoryMessages(const std::vector<Message>& messages) {
    auto result=nlohmann::json::array();
    for(const auto& message:messages) {
        nlohmann::json item={{"role",Utf8(message.role==L"streaming"?L"partial":message.role)},{"text",Utf8(message.text)},{"activity",Utf8(message.activity)},{"tool_call_id",message.toolCallId}};
        item["tool_calls"]=nlohmann::json::array();
        for(const auto& call:message.toolCalls)item["tool_calls"].push_back({{"id",call.id},{"name",call.name},{"arguments",call.arguments}});
        item["attachments"]=nlohmann::json::array();
        for(const auto& a:message.attachments)item["attachments"].push_back({{"name",Utf8(a.name)},{"mime",a.mime},{"data",a.data},{"bytes",a.bytes}});
        result.push_back(std::move(item));
    }
    return result;
}
inline std::vector<Message> ReadHistoryMessages(const nlohmann::json& items,bool context) {
    if(!items.is_array())throw std::runtime_error("Invalid history messages.");
    std::vector<Message> result;
    for(const auto& item:items) {
        Message message{Wide(item.at("role").get<std::string>()),Wide(item.at("text").get<std::string>())};
        bool apiRole=message.role==L"user"||message.role==L"assistant"||message.role==L"tool";
        if(!apiRole&&(context||(message.role!=L"partial"&&message.role!=L"error"&&message.role!=L"tool_display")))throw std::runtime_error("Invalid history role.");
        message.activity=Wide(item.value("activity",std::string{}));message.toolCallId=item.value("tool_call_id",std::string{});
        for(const auto& call:item.at("tool_calls"))message.toolCalls.push_back({call.at("id").get<std::string>(),call.at("name").get<std::string>(),call.at("arguments").get<std::string>()});
        for(const auto& a:item.at("attachments"))message.attachments.push_back({Wide(a.at("name").get<std::string>()),a.at("mime").get<std::string>(),a.at("data").get<std::string>(),a.at("bytes").get<size_t>()});
        result.push_back(std::move(message));
    }
    return result;
}
inline ChatHistory LoadChatHistory(const std::filesystem::path& directory) {
    auto path=directory/L"chat-history.json";
    if(!std::filesystem::exists(path))return {};
    try {
        if(std::filesystem::file_size(path)>256ULL*1024*1024)throw std::runtime_error("History too large.");
        std::ifstream stream(path,std::ios::binary);auto json=nlohmann::json::parse(stream);
        if(json.at("version")!=1||!json.at("chats").is_array())throw std::runtime_error("Unsupported history format.");
        ChatHistory result;result.activeId=json.at("active_chat_id").get<std::string>();std::set<std::string> ids;
        for(const auto& item:json.at("chats")) {
            SavedChat chat;chat.id=item.at("id").get<std::string>();
            if(!ValidChatId(chat.id)||!ids.insert(chat.id).second)throw std::runtime_error("Invalid chat ID.");
            chat.title=Wide(item.at("title").get<std::string>());chat.draft=Wide(item.value("draft",std::string{}));
            chat.messages=ReadHistoryMessages(item.at("messages"),false);chat.context=ReadHistoryMessages(item.at("context"),true);
            result.chats.push_back(std::move(chat));
        }
        if(!result.activeId.empty()&&!ids.count(result.activeId))throw std::runtime_error("Missing active chat.");
        return result;
    }catch(...){throw std::runtime_error("Could not read chat-history.json. The existing file has been left unchanged; history saving is disabled for this session. Move or repair that file before restarting.");}
}
inline void SaveChatHistory(const std::filesystem::path& directory,const ChatHistory& history) {
    nlohmann::json json={{"version",1},{"active_chat_id",history.activeId},{"chats",nlohmann::json::array()}};
    for(const auto& chat:history.chats)json["chats"].push_back({{"id",chat.id},{"title",Utf8(chat.title)},{"draft",Utf8(chat.draft)},{"messages",HistoryMessages(chat.messages)},{"context",HistoryMessages(chat.context)}});
    auto bytes=json.dump(2);
    if(bytes.size()>256ULL*1024*1024)throw std::runtime_error("Chat history exceeds the 256 MiB storage limit.");
    auto temporary=directory/L"chat-history.json.tmp",destination=directory/L"chat-history.json";
    {std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));stream.flush();if(!stream)throw std::runtime_error("Could not save chat history.");}
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not replace chat history.");
}
}
