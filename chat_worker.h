#pragma once
#include "chat_client.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include "agent.h"

namespace chat {
struct Result {
    unsigned generation;
    Reply reply;
    bool final = true;
    UpdateKind kind = UpdateKind::Delta;
    std::vector<Message> history;
};
class ChatWorker {
public:
    using Delivery = std::function<void(Result)>;
    ChatWorker(std::filesystem::path directory, Delivery delivery, Endpoint endpoint = {});
    ~ChatWorker();
    void Start(std::vector<Message> messages, unsigned generation);
    void Cancel();
    void Stop();
private:
    struct Job {
        std::vector<Message> messages;
        unsigned generation;
        std::shared_ptr<Cancellation> cancel;
    };
    void Loop();
    std::filesystem::path directory_;
    Delivery delivery_;
    Endpoint endpoint_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Job> pending_;
    std::shared_ptr<Cancellation> active_;
    bool stopping_ = false;
    std::thread thread_;
};
}
