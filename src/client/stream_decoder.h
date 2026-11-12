#pragma once
#include "client/chat_client.h"
#include <string_view>
#include <map>

namespace chat {
class StreamDecoder {
public:
    explicit StreamDecoder(OnDelta onDelta = {},std::string key = {}) : onDelta_(std::move(onDelta)),key_(std::move(key)) {}
    void Feed(std::string_view bytes);
    Reply Finish() const;
    bool Done() const { return done_; }
private:
    void Line();
    void Event();
    OnDelta onDelta_;
    std::string key_;
    std::string line_, data_;
    std::wstring text_;
    std::map<int, ToolCall> calls_;
    CallMetrics metrics_;
    bool done_ = false, skipLf_ = false, firstLine_ = true;
};
}
