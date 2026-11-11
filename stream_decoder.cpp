#include "stream_decoder.h"
#include "text_encoding.h"
#include "third_party/nlohmann/json.hpp"

namespace chat {
void StreamDecoder::Feed(std::string_view bytes) {
    // Retain bytes until an entire SSE event arrives, including split UTF-8.
    for (char c : bytes) {
        if (done_) return;
        if (skipLf_) { skipLf_ = false; if (c == '\n') continue; }
        if (c == '\r' || c == '\n') {
            Line(); line_.clear(); skipLf_ = c == '\r';
        } else line_ += c;
    }
}
void StreamDecoder::Line() {
    if (firstLine_) {
        firstLine_ = false;
        if (line_.compare(0, 3, "\xef\xbb\xbf") == 0) line_.erase(0, 3);
    }
    if (line_.empty()) { Event(); return; }
    if (line_.front() == ':') return;
    auto colon = line_.find(':');
    if (line_.substr(0, colon) != "data") return;
    auto value = colon == line_.npos ? std::string{} : line_.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.erase(0, 1);
    data_ += value + '\n';
}
void StreamDecoder::Event() {
    if (data_.empty()) return;
    data_.pop_back();
    auto data = std::move(data_); data_.clear();
    if (data == "[DONE]") { done_ = true; return; }
    auto json = nlohmann::json::parse(data, nullptr, false);
    if (json.is_discarded() || !json.is_object()) throw std::runtime_error("The AI service returned an invalid stream. Please try again.");
    if (json.contains("error")) throw std::runtime_error("The AI stream failed. Check your model and credits, then try again.");
    if(json.contains("usage")&&json["usage"].is_object()) {
        auto count=[&](const char* name,uint64_t& total,unsigned& reports) {
            auto entry=json["usage"].find(name);if(entry==json["usage"].end()||!entry->is_number_integer())return;
            if(!entry->is_number_unsigned()&&entry->get<int64_t>()<0)return;
            auto value=entry->get<uint64_t>();if(value>1000000000000ULL)return;
            total=value;reports=1;
        };
        count("prompt_tokens",metrics_.sentTokens,metrics_.sentReports);count("completion_tokens",metrics_.receivedTokens,metrics_.receivedReports);
        if(!json.contains("choices"))return;
    }
    if (!json.contains("choices") || !json["choices"].is_array()) throw std::runtime_error("The AI service returned an invalid stream. Please try again.");
    for (auto& choice : json["choices"]) {
        if (!choice.is_object()) throw std::runtime_error("The AI service returned an invalid stream. Please try again.");
        if (choice.value("index", 0) != 0) continue;
        if (choice.value("finish_reason", nlohmann::json{}) == "error") throw std::runtime_error("The AI stream was interrupted by the provider. Please try again.");
        if (!choice.contains("delta") || !choice["delta"].is_object()) continue;
        auto& delta = choice["delta"];
        if (delta.contains("tool_calls")) {
            if (!delta["tool_calls"].is_array()) throw std::runtime_error("Invalid streamed tool calls.");
            for (auto& fragment : delta["tool_calls"]) {
                int index = fragment.at("index").get<int>();
                if (index < 0 || index >= 16) throw std::runtime_error("The model requested too many tools at once.");
                auto& call = calls_[index];
                if (fragment.contains("id") && fragment["id"].is_string()) call.id += fragment["id"].get<std::string>();
                if (fragment.contains("function")) {
                    auto& function = fragment["function"];
                    if (function.contains("name") && function["name"].is_string()) call.name += function["name"].get<std::string>();
                    if (function.contains("arguments") && function["arguments"].is_string()) call.arguments += function["arguments"].get<std::string>();
                }
            }
        }
        if (!delta.contains("content") || delta["content"].is_null()) continue;
        if (!delta["content"].is_string()) throw std::runtime_error("The AI service returned an invalid text stream. Please try again.");
        auto text = Wide(delta["content"].get<std::string>());
        if (text.empty()) continue;
        text_ += text;
        if (onDelta_) onDelta_(text);
    }
}
Reply StreamDecoder::Finish() const {
    auto finish=[&]() -> Reply {
    if (!done_) return {L"The AI stream ended before the response completed. Please try again."};
    if (text_.find_first_not_of(L" \r\n\t") == text_.npos && calls_.empty()) return {L"The model returned no text. Choose another model in Settings."};
    Reply reply{text_, true};
    for (auto& entry : calls_) {
        auto& call = entry.second;
        if (call.id.empty() || call.name.empty()) return {L"The model returned an incomplete tool call."};
        for (auto& previous : reply.toolCalls) if (previous.id == call.id) return {L"The model returned duplicate tool call IDs."};
        reply.toolCalls.push_back(call);
    }
    return reply;
    };
    auto reply=finish();reply.metrics=metrics_;return reply;
}
}

