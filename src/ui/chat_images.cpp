#define NOMINMAX
#include "ui/chat_images.h"
#include <gdiplus.h>
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cwctype>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"winhttp.lib")

namespace chat {
namespace {
constexpr size_t MaxBytes=20*1024*1024;
void Require(bool ok){if(!ok)throw std::runtime_error("Image unavailable");}
struct Http {
    HINTERNET handle;
    explicit Http(HINTERNET value):handle(value){Require(handle!=nullptr);}
    ~Http(){WinHttpCloseHandle(handle);}
};
std::vector<unsigned char> ReadImage(const std::wstring& source) {
    std::vector<unsigned char> bytes;
    if(!WebImageSource(source)) {
        auto size=std::filesystem::file_size(source);Require(size>0&&size<=MaxBytes);
        bytes.resize(static_cast<size_t>(size));std::ifstream file(std::filesystem::path(source),std::ios::binary);
        Require(file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())).good());return bytes;
    }
    URL_COMPONENTS url{};url.dwStructSize=sizeof(url);url.dwHostNameLength=url.dwUrlPathLength=url.dwExtraInfoLength=url.dwUserNameLength=url.dwPasswordLength=static_cast<DWORD>(-1);
    Require(WinHttpCrackUrl(source.c_str(),0,0,&url)!=FALSE);
    Require(url.dwUserNameLength==0&&url.dwPasswordLength==0);
    std::wstring host(url.lpszHostName,url.dwHostNameLength),path(url.lpszUrlPath,url.dwUrlPathLength);
    if(path.empty())path=L"/";
    if(url.dwExtraInfoLength)path.append(url.lpszExtraInfo,url.dwExtraInfoLength);
    auto fragment=path.find(L'#');if(fragment!=path.npos)path.resize(fragment);
    Http session(WinHttpOpen(L"ChatHarness/Image",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    Require(WinHttpSetTimeouts(session.handle,5000,5000,5000,5000)!=FALSE);
    Http connection(WinHttpConnect(session.handle,host.c_str(),url.nPort,0));
    Http request(WinHttpOpenRequest(connection.handle,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,url.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0));
    DWORD disable=WINHTTP_DISABLE_COOKIES;
    Require(WinHttpSetOption(request.handle,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable))!=FALSE);
    DWORD auth=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;WinHttpSetOption(request.handle,WINHTTP_OPTION_AUTOLOGON_POLICY,&auth,sizeof(auth));
    Require(WinHttpSendRequest(request.handle,L"Accept: image/*\r\n",static_cast<DWORD>(-1),nullptr,0,0,0)!=FALSE);
    Require(WinHttpReceiveResponse(request.handle,nullptr)!=FALSE);
    DWORD status=0,length=sizeof(status);
    Require(WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX)!=FALSE&&status>=200&&status<300);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    unsigned char chunk[16384];DWORD count=0;
    do {
        Require(std::chrono::steady_clock::now()<deadline);
        Require(WinHttpReadData(request.handle,chunk,sizeof(chunk),&count)!=FALSE);
        Require(bytes.size()+count<=MaxBytes);bytes.insert(bytes.end(),chunk,chunk+count);
    }while(count);
    Require(!bytes.empty());return bytes;
}
}
bool WebImageSource(const std::wstring& source) {
    return _wcsnicmp(source.c_str(),L"https://",8)==0||_wcsnicmp(source.c_str(),L"http://",7)==0;
}
bool ImageLink(const std::wstring& source) {
    auto end=source.find_first_of(L"?#");auto ext=std::filesystem::path(source.substr(0,end)).extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    return ext==L".png"||ext==L".jpg"||ext==L".jpeg"||ext==L".gif"||ext==L".bmp"||ext==L".tif"||ext==L".tiff";
}
std::wstring ResolveImageSource(const std::filesystem::path& workspace,const std::wstring& source) {
    if(source.empty()||source.find_first_of(L"\r\n\t")!=source.npos||source.find(L'\0')!=source.npos)return {};
    if(WebImageSource(source))return source;
    // Local previews belong to the current chat's workspace, including absolute references.
    if(workspace.empty()||source.find(L"://")!=source.npos||source.rfind(L"\\\\",0)==0||source.rfind(L"//",0)==0)return {};
    try {
        auto root=std::filesystem::weakly_canonical(workspace);
        auto path=std::filesystem::path(source);path=std::filesystem::weakly_canonical(path.is_absolute()?path:root/path);
        auto a=root.begin(),b=path.begin();
        for(;a!=root.end();++a,++b)if(b==path.end()||_wcsicmp(a->c_str(),b->c_str())!=0)return {};
        if(b==path.end())return {};
        return path.wstring();
    }catch(...){return {};}
}
SIZE FitChatImage(int width,int height,int availableWidth,int maximumHeight) {
    if(width<=0||height<=0||availableWidth<=0||maximumHeight<=0)return {};
    double scale=std::min({1.0,static_cast<double>(availableWidth)/width,static_cast<double>(maximumHeight)/height});
    return {std::max(1,static_cast<int>(width*scale)),std::max(1,static_cast<int>(height*scale))};
}
std::shared_ptr<ChatImage> LoadChatImage(const std::wstring& source) {
    auto result=std::make_shared<ChatImage>();
    try {
        Require(!source.empty());auto bytes=ReadImage(source);
        struct Gdi {ULONG_PTR token=0;Gdi(){Gdiplus::GdiplusStartupInput input;Require(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok);}~Gdi(){Gdiplus::GdiplusShutdown(token);}} gdi;
        auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes.size());Require(memory!=nullptr);
        auto buffer=GlobalLock(memory);if(!buffer){GlobalFree(memory);Require(false);}
        memcpy(buffer,bytes.data(),bytes.size());GlobalUnlock(memory);
        IStream* raw=nullptr;
        if(FAILED(CreateStreamOnHGlobal(memory,TRUE,&raw))){GlobalFree(memory);Require(false);}
        std::unique_ptr<IStream,void(*)(IStream*)> stream(raw,[](IStream* value){value->Release();});
        Gdiplus::Bitmap original(stream.get());Require(original.GetLastStatus()==Gdiplus::Ok);
        auto w=original.GetWidth(),h=original.GetHeight();Require(w&&h&&static_cast<unsigned long long>(w)*h<=32000000);
        auto size=FitChatImage(static_cast<int>(w),static_cast<int>(h),1600,1600);
        Gdiplus::Bitmap scaled(size.cx,size.cy,PixelFormat32bppARGB);
        {Gdiplus::Graphics graphics(&scaled);graphics.Clear(Gdiplus::Color(255,24,24,24));graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);Require(graphics.DrawImage(&original,0,0,size.cx,size.cy)==Gdiplus::Ok);}
        Require(scaled.GetHBITMAP(Gdiplus::Color(255,24,24,24),&result->bitmap)==Gdiplus::Ok);
        result->width=size.cx;result->height=size.cy;
    }catch(...){result->error=L"Image unavailable (missing, unsupported, or too large)";}
    return result;
}
std::shared_ptr<ChatImage> ChatImageCache::Get(const std::wstring& source) {
    auto& entry=entries[source];entry.used=++clock;
    if(!entry.image&&!entry.pending.valid()) {
        size_t active=0;for(const auto& pair:entries)active+=pair.second.pending.valid()?1:0;
        if(active<4) {
            try{entry.pending=std::async(std::launch::async,[source]{return LoadChatImage(source);});}
            catch(...){entry.image=std::make_shared<ChatImage>();entry.image->error=L"Could not load image";}
        }
    }
    return entry.image;
}
bool ChatImageCache::Poll() {
    bool changed=false;
    for(auto& pair:entries)if(pair.second.pending.valid()&&pair.second.pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try{pair.second.image=pair.second.pending.get();}catch(...){pair.second.image=std::make_shared<ChatImage>();pair.second.image->error=L"Could not load image";}
        changed=true;
    }
    // Bound decoded bitmap memory; in-flight jobs never capture the cache or UI.
    size_t bytes=0;for(const auto& pair:entries)if(pair.second.image)bytes+=static_cast<size_t>(pair.second.image->width)*pair.second.image->height*4;
    while(bytes>64*1024*1024||entries.size()>256) {
        auto oldest=entries.end();
        for(auto it=entries.begin();it!=entries.end();++it)if(!it->second.pending.valid()&&(oldest==entries.end()||it->second.used<oldest->second.used))oldest=it;
        if(oldest==entries.end())break;
        if(oldest->second.image)bytes-=static_cast<size_t>(oldest->second.image->width)*oldest->second.image->height*4;
        entries.erase(oldest);
    }
    return changed;
}
}
