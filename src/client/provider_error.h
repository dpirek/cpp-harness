#pragma once
#include "common/text_encoding.h"
#include <nlohmann/json.hpp>

namespace chat {
inline std::string ProviderResponseText(std::string body,const std::string& key={}) {
    if(body.empty())return "(empty response body)";
    // Decode JSON escapes before redacting an echoed credential.
    auto json=nlohmann::json::parse(body,nullptr,false);
    if(!json.is_discarded())body=json.dump(2,' ',false,nlohmann::json::error_handler_t::replace);
    if(!key.empty())for(size_t pos=0;(pos=body.find(key,pos))!=body.npos;pos+=10)body.replace(pos,key.size(),"[redacted]");
    constexpr size_t limit=16384;
    bool truncated=body.size()>limit;
    if(truncated)body.resize(limit);
    // Replace malformed UTF-8 rather than losing the entire diagnostic.
    int count=MultiByteToWideChar(CP_UTF8,0,body.data(),static_cast<int>(body.size()),nullptr,0);
    std::wstring wide(count,L' ');
    if(count)MultiByteToWideChar(CP_UTF8,0,body.data(),static_cast<int>(body.size()),wide.data(),count);
    for(auto& c:wide)if((c<32&&c!=L'\n'&&c!=L'\r'&&c!=L'\t')||c==127)c=L' ';
    if(truncated)wide+=L"\n[response truncated at 16 KB]";
    std::wstring display;
    for(size_t i=0;i<wide.size();++i){if(wide[i]==L'\n'&&(i==0||wide[i-1]!=L'\r'))display+=L'\r';display+=wide[i];}
    return Utf8(display);
}
}
