#pragma once
#include <windows.h>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>

namespace chat {
struct ChatImage {
    HBITMAP bitmap=nullptr;
    int width=0,height=0;
    std::wstring error;
    ~ChatImage(){if(bitmap)DeleteObject(bitmap);}
};
bool WebImageSource(const std::wstring& source);
bool ImageLink(const std::wstring& source);
std::wstring ResolveImageSource(const std::filesystem::path& workspace,const std::wstring& source);
SIZE FitChatImage(int width,int height,int availableWidth,int maximumHeight);
std::shared_ptr<ChatImage> LoadChatImage(const std::wstring& source);
class ChatImageCache {
    struct Entry {
        std::shared_ptr<ChatImage> image;
        std::future<std::shared_ptr<ChatImage>> pending;
        unsigned long long used=0;
    };
    std::map<std::wstring,Entry> entries;
    unsigned long long clock=0;
public:
    std::shared_ptr<ChatImage> Get(const std::wstring& source);
    bool Poll();
};
}
