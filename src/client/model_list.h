#pragma once
#include "common/text_encoding.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>

namespace chat {
inline std::vector<std::string> ParseModelList(const std::string& body) {
    auto json=nlohmann::json::parse(body,nullptr,false);
    if(json.is_discarded())throw std::runtime_error("The model endpoint returned invalid JSON. Check the provider URL.");
    const auto* data=&json;
    if(json.is_object()) {
        if(json.contains("error")&&!json["error"].is_null())throw std::runtime_error("The provider reported an error instead of a model list. Check the URL, API key and provider access.");
        if(json.contains("data"))data=&json["data"];
        else if(json.contains("models"))data=&json["models"];
    }
    if(!data->is_array())throw std::runtime_error("Expected a model array in data or models. This URL may not support model discovery; enter a model ID manually.");
    std::vector<std::string> models;
    for(const auto& item:*data) {
        const auto* value=&item;
        if(item.is_object()) {
            if(item.contains("id"))value=&item["id"];
            else if(item.contains("model"))value=&item["model"];
        }
        if(!value->is_string())continue;
        auto id=value->get<std::string>();
        if(id.empty()||id.size()>512||std::any_of(id.begin(),id.end(),[](unsigned char c){return c<=32||c==127;}))continue;
        try{Wide(id);}catch(const std::exception&){continue;}
        models.push_back(std::move(id));
    }
    std::sort(models.begin(),models.end());models.erase(std::unique(models.begin(),models.end()),models.end());
    if(models.empty())throw std::runtime_error(data->empty()?"The provider returned no models. Enter a model ID manually.":"The provider returned no usable model IDs. Enter a model ID manually.");
    return models;
}
inline std::string ModelHttpError(unsigned status) {
    std::string message="Could not load models (HTTP "+std::to_string(status)+"). ";
    if(status==401)message+="API key rejected. Check or replace the key.";
    else if(status==403)message+="Access denied. Check the key's permissions and provider access.";
    else if(status==404||status==405)message+="This provider may not support GET /models. Check the base URL or enter a model ID manually.";
    else if(status==400||status==422)message+="The provider rejected the model-list request. Check the base URL and required URL query parameters; this provider may require a separate models endpoint. You can enter a model ID manually.";
    else if(status==429)message+="Provider rate limit or quota reached. Wait and retry or check your quota.";
    else if(status>=500)message+="The provider is unavailable. Try again later.";
    else message+="Check the provider URL and access, or enter a model ID manually.";
    return message;
}
inline std::string ModelErrorDetail(const std::string& body,const std::string& key) {
    // Only show structured error messages, never raw HTML or request bodies.
    auto json=nlohmann::json::parse(body,nullptr,false);
    if(!json.is_object())return {};
    const nlohmann::json* value=nullptr;
    if(json.contains("error")) {
        auto& error=json["error"];
        if(error.is_string())value=&error;
        else if(error.is_object()&&error.contains("message"))value=&error["message"];
    }
    if(!value&&json.contains("detail")) {
        auto& detail=json["detail"];
        if(detail.is_string())value=&detail;
        else if(detail.is_array()&&!detail.empty()&&detail[0].is_object()&&detail[0].contains("msg"))value=&detail[0]["msg"];
    }
    if(!value||!value->is_string())return {};
    auto message=value->get<std::string>();
    if(!key.empty())for(size_t pos=0;(pos=message.find(key,pos))!=message.npos;pos+=10)message.replace(pos,key.size(),"[redacted]");
    try{auto wide=Wide(message);if(wide.size()>240)wide=wide.substr(0,239)+L"\u2026";for(auto& c:wide)if(c<32||c==127)c=L' ';return Utf8(wide);}catch(...){return {};}
}
}
