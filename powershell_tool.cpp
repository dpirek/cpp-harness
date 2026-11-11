#include "powershell_tool.h"
#include <array>
#include <string_view>
#include <stdexcept>

namespace chat {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
std::wstring Base64(const std::wstring& script) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto data = reinterpret_cast<const unsigned char*>(script.data());
    size_t size = script.size() * sizeof(wchar_t);std::wstring result;
    for (size_t i = 0; i < size; i += 3) {
        unsigned bits = static_cast<unsigned>(data[i]) << 16;
        if (i + 1 < size) bits |= static_cast<unsigned>(data[i + 1]) << 8;
        if (i + 2 < size) bits |= data[i + 2];
        result += alphabet[(bits >> 18) & 63];result += alphabet[(bits >> 12) & 63];
        result += i + 1 < size ? alphabet[(bits >> 6) & 63] : '=';
        result += i + 2 < size ? alphabet[bits & 63] : '=';
    }
    return result;
}
std::vector<wchar_t> ChildEnvironment() {
    std::vector<wchar_t> block;
    auto environment = GetEnvironmentStringsW();
    if (!environment) throw std::runtime_error("Could not read the process environment.");
    for (auto entry = environment; *entry; entry += wcslen(entry) + 1) {
        auto equals = wcschr(entry, L'=');
        std::wstring name(entry, equals ? equals : entry);
        if (_wcsicmp(name.c_str(), L"OPEN_ROUTER_KEY") == 0 || _wcsicmp(name.c_str(), L"OPENROUTER_API_KEY") == 0 || _wcsicmp(name.c_str(), L"AI_API_KEY") == 0) continue;
        block.insert(block.end(), entry, entry + wcslen(entry) + 1);
    }
    FreeEnvironmentStringsW(environment);block.push_back(L'\0');if(block.size()==1)block.push_back(L'\0');return block;
}
std::wstring DecodeOutput(const std::string& bytes) {
    if (bytes.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring output(size, L'\0');MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), output.data(), size);return output;
}
}
ToolResult RunPowerShell(const std::wstring& command, const std::filesystem::path& directory, const Cancellation& cancel, unsigned timeoutSeconds) {
    ToolResult result;
    if (cancel.IsCancelled()) { result.cancelled = true;return result; }
    if (command.empty() || command.size() > 8000 || timeoutSeconds < 1 || timeoutSeconds > 120) { result.output = L"Invalid command or timeout (maximum 8,000 characters and 120 seconds).";return result; }
    Handle read,write,stdinHandle,job,process,thread;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};
    if (!CreatePipe(&read.value,&write.value,&attributes,0) || !SetHandleInformation(read.value,HANDLE_FLAG_INHERIT,0)) { result.output=L"Could not create the tool output pipe.";return result; }
    stdinHandle.value=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,OPEN_EXISTING,0,nullptr);
    if(stdinHandle.value==INVALID_HANDLE_VALUE){result.output=L"Could not initialize tool input.";return result;}
    job.value=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.value||!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){result.output=L"Could not create the tool process job.";return result;}
    // Encode the script as UTF-16, avoiding command-line quoting/interpolation.
    std::wstring script=L"$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false); $OutputEncoding=[Console]::OutputEncoding; try { & {\r\n"+command+L"\r\n}; if($LASTEXITCODE){exit $LASTEXITCODE}; exit 0 } catch { [Console]::Error.WriteLine($_.ToString()); exit 1 }";
    wchar_t systemDirectory[MAX_PATH];GetSystemDirectoryW(systemDirectory,MAX_PATH);
    auto shell=std::filesystem::path(systemDirectory)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    std::wstring arguments=L"\""+shell.wstring()+L"\" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand "+Base64(script);
    if(arguments.size()>=32767){result.output=L"Tool command exceeds the Windows command-line limit.";return result;}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    startup.hStdInput=stdinHandle.value;startup.hStdOutput=write.value;startup.hStdError=write.value;
    PROCESS_INFORMATION info{};auto environment=ChildEnvironment();
    if(!CreateProcessW(shell.c_str(),arguments.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT,environment.data(),directory.c_str(),&startup,&info)){result.output=L"Could not launch Windows PowerShell.";return result;}
    process.value=info.hProcess;thread.value=info.hThread;
    if(!AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);WaitForSingleObject(process.value,5000);result.output=L"Could not attach the tool to its cancellation job.";return result;}
    if(ResumeThread(thread.value)==static_cast<DWORD>(-1)){TerminateJobObject(job.value,1);WaitForSingleObject(process.value,5000);result.output=L"Could not start the tool process.";return result;}
    write.Close();stdinHandle.Close();thread.Close();
    std::string output;constexpr size_t MaxOutput=64*1024;
    auto drain=[&]{
        std::array<char,4096> buffer{};
        for(int batch=0;batch<32;++batch){DWORD available=0;if(!PeekNamedPipe(read.value,nullptr,0,nullptr,&available,nullptr)||!available)break;DWORD count=0;
            if(!ReadFile(read.value,buffer.data(),(std::min)(available,static_cast<DWORD>(buffer.size())),&count,nullptr)||!count)break;
            size_t keep=(std::min)(static_cast<size_t>(count),MaxOutput-output.size());output.append(buffer.data(),keep);if(keep<count)result.truncated=true;
        }
    };
    auto deadline=GetTickCount64()+timeoutSeconds*1000ULL;
    for(;;){
        drain();
        if(cancel.IsCancelled()){result.cancelled=true;TerminateJobObject(job.value,1);break;}
        if(GetTickCount64()>=deadline){result.timedOut=true;TerminateJobObject(job.value,1);break;}
        HANDLE handles[]{cancel.Event(),process.value};DWORD wait=WaitForMultipleObjects(2,handles,FALSE,25);
        if(wait==WAIT_OBJECT_0+1)break;
        if(wait==WAIT_FAILED){TerminateJobObject(job.value,1);break;}
    }
    // Kill descendants too, including commands that left a child holding stdout.
    job.Close();WaitForSingleObject(process.value,5000);drain();
    DWORD exitCode=1;GetExitCodeProcess(process.value,&exitCode);result.exitCode=exitCode;result.output=DecodeOutput(output);
    return result;
}
}
