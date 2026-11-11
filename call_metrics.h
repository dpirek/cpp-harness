#pragma once
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>

namespace chat {
struct CallMetrics {
    uint64_t sentTokens=0,receivedTokens=0,llmMs=0,elapsedMs=0;
    unsigned calls=0,sentReports=0,receivedReports=0;
    std::wstring model;
    void Add(const CallMetrics& other) {
        calls+=other.calls;sentTokens+=other.sentTokens;receivedTokens+=other.receivedTokens;
        sentReports+=other.sentReports;receivedReports+=other.receivedReports;llmMs+=other.llmMs;
    }
};
inline std::wstring MetricSeconds(uint64_t milliseconds) {
    std::wostringstream value;value<<std::fixed<<std::setprecision(1)<<milliseconds/1000.0;return value.str()+L" s";
}
inline std::wstring MetricTokens(uint64_t count,unsigned reports,unsigned calls) {
    if(!reports)return L"not reported";
    auto value=std::to_wstring(count);
    for(int i=static_cast<int>(value.size())-3;i>0;i-=3)value.insert(static_cast<size_t>(i),1,L',');
    value+=L" tokens";
    if(reports<calls)value+=L" ("+std::to_wstring(reports)+L"/"+std::to_wstring(calls)+L" calls reported)";
    return value;
}
inline std::wstring CallActivity(const CallMetrics& metrics,const std::wstring& state) {
    return (metrics.calls==1?L"LLM call":L"LLM calls")+std::wstring(L" \u00b7 ")+std::to_wstring(metrics.calls)+L" \u00b7 "+state+
        (metrics.model.empty()?L"":L"\nModel: "+metrics.model)+
        L"\nSent: "+MetricTokens(metrics.sentTokens,metrics.sentReports,metrics.calls)+L" \u00b7 Received: "+MetricTokens(metrics.receivedTokens,metrics.receivedReports,metrics.calls)+
        L"\nTime: "+MetricSeconds(metrics.llmMs)+L" LLM \u00b7 "+MetricSeconds(metrics.elapsedMs)+L" total";
}
}
