#pragma once
#include "third_party/nlohmann/json.hpp"
#include "text_encoding.h"
#include <string>
#include <algorithm>

namespace chat {
inline std::wstring ToolOutput(const std::wstring& text) {
    auto first=text.find_first_not_of(L" \t\r\n");
    if(first==text.npos)return text;
    auto last=text.find_last_not_of(L" \t\r\n");
    auto trimmed=text.substr(first,last-first+1);std::wstring language,body=trimmed;
    if(trimmed[0]==L'{'||trimmed[0]==L'[') {
        try {
            auto json=nlohmann::json::parse(Utf8(trimmed),nullptr,false);
            if(!json.is_discarded()&&(json.is_object()||json.is_array())){language=L"json";body=Wide(json.dump(2));}
            else {
                auto next=trimmed.find_first_not_of(L" \t\r\n",1);
                if(next!=trimmed.npos&&((trimmed[0]==L'{'&&trimmed[next]==L'"')||(trimmed[0]==L'['&&(trimmed[next]==L'{'||trimmed[next]==L'['||trimmed[next]==L'"'))))language=L"json";
            }
        }catch(...){return text;}
    } else if(trimmed[0]==L'<'&&trimmed.back()==L'>'&&trimmed.size()>2&&
        ((trimmed[1]>=L'A'&&trimmed[1]<=L'Z')||(trimmed[1]>=L'a'&&trimmed[1]<=L'z')||trimmed[1]==L'?'||trimmed.compare(0,4,L"<!--")==0))language=L"xml";
    if(language.empty())return text;
    size_t longest=0,run=0;for(auto c:body){run=c==L'`'?run+1:0;if(run>longest)longest=run;}
    std::wstring fence((std::max)(size_t(3),longest+1),L'`');
    return fence+language+L"\n"+body+L"\n"+fence;
}
}
