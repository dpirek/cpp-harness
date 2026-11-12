#pragma once
#include "common/text_encoding.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>
#include <vector>

namespace chat {
struct Attachment {
    std::wstring name;
    std::string mime, data; // UTF-8 text or a data URL; contents are snapshotted on drop.
    size_t bytes=0;
};
inline std::string AttachmentBase64(const std::string& bytes) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;result.reserve((bytes.size()+2)/3*4);
    for(size_t i=0;i<bytes.size();i+=3) {
        unsigned value=static_cast<unsigned char>(bytes[i])<<16;
        if(i+1<bytes.size())value|=static_cast<unsigned char>(bytes[i+1])<<8;
        if(i+2<bytes.size())value|=static_cast<unsigned char>(bytes[i+2]);
        result+=alphabet[(value>>18)&63];result+=alphabet[(value>>12)&63];
        result+=i+1<bytes.size()?alphabet[(value>>6)&63]:'=';result+=i+2<bytes.size()?alphabet[value&63]:'=';
    }
    return result;
}
inline Attachment LoadAttachment(const std::filesystem::path& path) {
    if(!std::filesystem::is_regular_file(path))throw std::runtime_error("Drop individual files, not folders.");
    auto size=std::filesystem::file_size(path);
    if(size>10*1024*1024)throw std::runtime_error("Each attachment must be 10 MiB or smaller.");
    std::ifstream file(path,std::ios::binary);
    std::string bytes(static_cast<size_t>(size),'\0');
    if(!file||!file.read(bytes.data(),static_cast<std::streamsize>(size))||file.peek()!=EOF)throw std::runtime_error("Could not read the attachment, or it changed while reading.");
    if(bytes.empty())throw std::runtime_error("The attachment is empty.");
    Attachment result;result.name=path.filename().wstring();result.bytes=bytes.size();
    auto ext=path.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    if(ext==L".png"&&bytes.compare(0,8,"\x89PNG\r\n\x1a\n")==0)result.mime="image/png";
    else if((ext==L".jpg"||ext==L".jpeg")&&bytes.compare(0,3,"\xff\xd8\xff")==0)result.mime="image/jpeg";
    else if(ext==L".gif"&&(bytes.compare(0,6,"GIF87a")==0||bytes.compare(0,6,"GIF89a")==0))result.mime="image/gif";
    else if(ext==L".webp"&&bytes.compare(0,4,"RIFF")==0&&bytes.size()>=12&&bytes.compare(8,4,"WEBP")==0)result.mime="image/webp";
    else if(ext==L".pdf"&&bytes.compare(0,5,"%PDF-")==0)result.mime="application/pdf";
    else {
        const std::vector<std::wstring> textTypes{L".txt",L".md",L".markdown",L".csv",L".tsv",L".json",L".xml",L".yaml",L".yml",L".log",L".html",L".css",L".js",L".ts",L".jsx",L".tsx",L".py",L".cpp",L".h",L".c",L".cs",L".java",L".rs",L".go",L".sql",L".ps1",L".sh"};
        if(std::find(textTypes.begin(),textTypes.end(),ext)==textTypes.end())throw std::runtime_error("Unsupported file. Use PNG, JPEG, GIF, WebP, PDF, or a text/source document. Export Office documents to PDF or text first.");
        if(bytes.size()>1024*1024)throw std::runtime_error("Text documents must be 1 MiB or smaller.");
        if(bytes.compare(0,3,"\xef\xbb\xbf")==0)bytes.erase(0,3);
        if(bytes.size()>=2&&((static_cast<unsigned char>(bytes[0])==255&&static_cast<unsigned char>(bytes[1])==254)||(static_cast<unsigned char>(bytes[0])==254&&static_cast<unsigned char>(bytes[1])==255))) {
            if(bytes.size()%2)throw std::runtime_error("Invalid UTF-16 document.");
            bool little=static_cast<unsigned char>(bytes[0])==255;std::wstring text;
            for(size_t i=2;i<bytes.size();i+=2){unsigned a=static_cast<unsigned char>(bytes[i]),b=static_cast<unsigned char>(bytes[i+1]);text+=static_cast<wchar_t>(little?a|(b<<8):(a<<8)|b);}
            bytes=Utf8(text);
        }
        Wide(bytes);if(bytes.find('\0')!=bytes.npos)throw std::runtime_error("Use a UTF-8 or UTF-16 text document without binary data.");
        result.mime="text/plain";result.data=std::move(bytes);return result;
    }
    result.data="data:"+result.mime+";base64,"+AttachmentBase64(bytes);return result;
}
inline std::wstring AttachmentNames(const std::vector<Attachment>& attachments) {
    std::wstring result;
    for(const auto& item:attachments){if(!result.empty())result+=L", ";result+=item.name;}
    return result;
}
}
