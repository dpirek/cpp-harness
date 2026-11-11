#include "chat_worker.h"
#include "text_encoding.h"

namespace chat {
ChatWorker::ChatWorker(std::filesystem::path directory, Delivery delivery, Endpoint endpoint)
    : directory_(std::move(directory)), delivery_(std::move(delivery)), endpoint_(std::move(endpoint)), thread_(&ChatWorker::Loop, this) {}
ChatWorker::~ChatWorker() { Stop(); }
void ChatWorker::Start(std::vector<Message> messages, unsigned generation) {
    auto cancel = std::make_shared<Cancellation>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (active_) active_->Cancel();
    pending_ = Job{std::move(messages), generation, std::move(cancel)};
    wake_.notify_one();
}
void ChatWorker::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
    if (active_) active_->Cancel();
}
void ChatWorker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.reset();
        if (active_) active_->Cancel();
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}
void ChatWorker::Loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            job = std::move(*pending_);
            pending_.reset();
            active_ = job.cancel;
        }
        Reply reply;
        auto started=GetTickCount64();
        std::vector<Message> history;
        try {
            auto config = LoadConfig(directory_);
            auto result = RunAgent(config, job.messages, directory_, *job.cancel, endpoint_, [&](AgentUpdate update) {
                if (!job.cancel->IsCancelled()) delivery_(Result{job.generation, Reply{std::move(update.text), true}, false, update.kind});
            });
            reply = std::move(result.reply);history = std::move(result.history);
        } catch (const std::runtime_error& error) {
            reply.text = Wide(error.what());
        } catch (...) { reply.text = L"Could not load AI configuration. Check the project .env file."; }
        reply.metrics.elapsedMs=GetTickCount64()-started;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.reset();
            if (stopping_) return;
        }
        if (!job.cancel->IsCancelled() && !reply.cancelled) {
            // Delivery only posts to the UI; no controls are accessed here.
            try { delivery_(Result{job.generation, std::move(reply), true, UpdateKind::Delta, std::move(history)}); } catch (...) {}
        }
    }
}
}
