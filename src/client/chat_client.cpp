#include "client/model_list.h"
#include "tools/computer_tool.h"
#include "client/chat_client.h"
#include "common/text_encoding.h"
#include "client/stream_decoder.h"
#include <nlohmann/json.hpp>
#include <winhttp.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <utility>

namespace chat {
using Json = nlohmann::json;
Cancellation::Cancellation() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!event_) throw std::runtime_error("Unable to create a cancellation event.");
}
Cancellation::~Cancellation() { CloseHandle(event_); }
void Cancellation::Cancel() const noexcept { SetEvent(event_); }
bool Cancellation::IsCancelled() const noexcept { return WaitForSingleObject(event_, 0) == WAIT_OBJECT_0; }
namespace {
struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET h) : value(h) { if (!h) throw std::runtime_error("Could not initialize the HTTP client."); }
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};
struct Cancelled {};
// Only the owning worker calls WinHTTP functions. Callbacks signal completion;
// cancellation wakes the worker, which closes the async request and waits for
// HANDLE_CLOSING before releasing callback state or request buffers.
class AsyncRequest {
public:
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> error{0};
    std::atomic<DWORD> bytes{0};
    HINTERNET handle = nullptr;
    bool callbackInstalled = false;
    std::array<char, 16384> buffer{};
    std::string payload;
    AsyncRequest() {
        if (!done || !closed) {
            if (done) CloseHandle(done);
            if (closed) CloseHandle(closed);
            throw std::runtime_error("Unable to create HTTP completion events.");
        }
    }
    ~AsyncRequest() {
        if (handle) {
            WinHttpCloseHandle(handle);
            if (callbackInstalled) WaitForSingleObject(closed, INFINITE);
        }
        CloseHandle(done);
        CloseHandle(closed);
    }
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length) noexcept {
        auto state = reinterpret_cast<AsyncRequest*>(context);
        if (!state) return;
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: SetEvent(state->closed); break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            state->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            SetEvent(state->done); break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            state->bytes = length;
            SetEvent(state->done); break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            state->bytes = *static_cast<DWORD*>(info);
            SetEvent(state->done); break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            SetEvent(state->done); break;
        }
    }
    void Begin(const Cancellation& cancel) {
        if (cancel.IsCancelled()) throw Cancelled{};
        ResetEvent(done); error = 0; bytes = 0;
    }
    void Wait(BOOL initiated, const Cancellation& cancel, ULONGLONG deadline) {
        if (!initiated && GetLastError() != ERROR_IO_PENDING) Fail(GetLastError());
        auto now = GetTickCount64();
        if (now >= deadline) throw std::runtime_error("AI request timed out. Please try again.");
        HANDLE events[]{cancel.Event(), done};
        DWORD result = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(deadline - now));
        if (result == WAIT_OBJECT_0) throw Cancelled{};
        if (result == WAIT_TIMEOUT) throw std::runtime_error("AI request timed out. Please try again.");
        if (result != WAIT_OBJECT_0 + 1) throw std::runtime_error("Unable to wait for the AI request.");
        if (error) Fail(error);
    }
    static void Fail(DWORD error) {
        if (error == ERROR_WINHTTP_TIMEOUT) throw std::runtime_error("AI request timed out. Please try again.");
        if (error == ERROR_WINHTTP_SECURE_FAILURE) throw std::runtime_error("The secure connection to the AI provider failed. Check Windows TLS and certificate settings.");
        throw std::runtime_error("Could not reach the AI service. Check your connection and try again.");
    }
};
}
std::string BuildPayload(const Config& config, const std::vector<Message>& messages) {
    Json history = Json::array();
    bool hasPdf=false;
    history.push_back({{"role", "system"}, {"content", config.systemPrompt}});
    for (auto& message : messages) {
        if (message.role == L"user" || message.role == L"assistant" || message.role == L"tool") {
            Json item{{"role", Utf8(message.role)}, {"content", Utf8(message.text)}};
            if(message.role==L"user"&&!message.attachments.empty()) {
                auto parts=Json::array();
                std::string text=Utf8(message.text);
                if(!message.text.empty())parts.push_back({{"type","text"},{"text",Utf8(message.text)}});
                for(const auto& attachment:message.attachments) {
                    if(attachment.mime=="text/plain") {
                        auto document="Attached document: "+Utf8(attachment.name)+"\n"+attachment.data;
                        parts.push_back({{"type","text"},{"text",document}});
                        if(!text.empty())text+="\n\n";
                        text+=document;
                    }
                    else if(attachment.mime=="application/pdf") {hasPdf=true;parts.push_back({{"type","file"},{"file",{{"filename",Utf8(attachment.name)},{"file_data",attachment.data}}}});}
                    else parts.push_back({{"type","image_url"},{"image_url",{{"url",attachment.data}}}});
                }
                bool textOnly=std::all_of(message.attachments.begin(),message.attachments.end(),[](const Attachment& attachment){return attachment.mime=="text/plain";});
                if(textOnly)item["content"]=std::move(text);
                else item["content"]=std::move(parts);
            }
            if (!message.toolCalls.empty()) {
                item["tool_calls"] = Json::array();
                for (auto& call : message.toolCalls) item["tool_calls"].push_back({{"id", call.id}, {"type", "function"}, {"function", {{"name", call.name}, {"arguments", call.arguments}}}});
            }
            if (message.role == L"tool") item["tool_call_id"] = message.toolCallId;
            history.push_back(std::move(item));
        }
    }
    Json tool{{"type", "function"}, {"function", {
        {"name", "powershell"}, {"description", "Run a noninteractive PowerShell command in the project directory. Returns combined stdout/stderr, exit code, timeout and truncation flags. Commands execute with the app's normal user permissions. Each call starts a fresh shell."},
        {"parameters", {{"type", "object"}, {"properties", {
            {"command", {{"type", "string"}, {"description", "PowerShell script to execute."}}},
            {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}, {"description", "Time limit; defaults to 30 seconds."}}}
        }}, {"required", Json::array({"command"})}, {"additionalProperties", false}}}
    }}};
    auto tools=Json::array({tool,Json::parse(ComputerDefinition())});
    for(const auto& definition:config.extraTools)tools.push_back(Json::parse(definition));
    Json payload{{"model", config.model}, {"messages", history}, {"max_tokens", 2048}, {"stream", true}, {"stream_options",{{"include_usage",true}}}, {"tools", tools}, {"tool_choice", "auto"}};
    if(hasPdf&&InferenceEndpoint(config.url).host==L"openrouter.ai")
        payload["plugins"]=Json::array({{{"id","file-parser"},{"pdf",{{"engine","cloudflare-ai"}}}}});
    return payload.dump();
}
Reply ParseReply(unsigned status, const std::string& body) {
    // Never display untrusted HTTP bodies: they may contain request details.
    if (status < 200 || status >= 300) return {L"AI request failed (HTTP " + std::to_wstring(status) + L"). Check your API key, credits, and model, then try again."};
    try {
        auto json = Json::parse(body);
        if (json.contains("error")) return {L"The AI provider reported an error. Check your model and credits, then try again."};
        auto& message=json.at("choices").at(0).at("message");
        Reply reply;reply.ok=true;
        if(message.contains("content")&&!message["content"].is_null())reply.text=Wide(message.at("content").get<std::string>());
        if(message.contains("tool_calls")) {
            if(!message["tool_calls"].is_array()||message["tool_calls"].size()>16)throw std::runtime_error("Invalid tool calls.");
            for(const auto& entry:message["tool_calls"]) {
                ToolCall call{entry.at("id").get<std::string>(),entry.at("function").at("name").get<std::string>(),entry.at("function").at("arguments").get<std::string>()};
                if(call.id.empty()||call.name.empty())throw std::runtime_error("Incomplete tool call.");
                for(const auto& previous:reply.toolCalls)if(previous.id==call.id)throw std::runtime_error("Duplicate tool call ID.");
                reply.toolCalls.push_back(std::move(call));
            }
        }
        if(reply.toolCalls.empty()&&reply.text.find_first_not_of(L" \r\n\t")==reply.text.npos)return {L"The model returned no text. Choose another model in Settings."};
        if(json.contains("usage")&&json["usage"].is_object()) {
            auto count=[&](const char* name,uint64_t& total,unsigned& reports){
                const auto& usage=json["usage"];if(!usage.contains(name)||!usage[name].is_number_integer())return;
                if(!usage[name].is_number_unsigned()&&usage[name].get<int64_t>()<0)return;
                auto value=usage[name].get<uint64_t>();if(value<=1000000000000ULL){total=value;reports=1;}
            };
            count("prompt_tokens",reply.metrics.sentTokens,reply.metrics.sentReports);count("completion_tokens",reply.metrics.receivedTokens,reply.metrics.receivedReports);
        }
        return reply;
    } catch (...) { return {L"The AI service returned an invalid response. Please try again."}; }
}
Endpoint ProviderEndpoint(const std::string& url) {
    Config config;config.url=url;ValidateConfig(config,false);
    auto wide=Wide(url);URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(wide.c_str(),0,0,&parts))throw std::runtime_error("Invalid provider URL.");
    Endpoint result;result.host.assign(parts.lpszHostName,parts.dwHostNameLength);result.port=parts.nPort;
    result.secure=parts.nScheme==INTERNET_SCHEME_HTTPS;
    result.path=parts.dwUrlPathLength?std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength):L"/";
    if(parts.dwExtraInfoLength)result.path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    return result;
}
Endpoint InferenceEndpoint(const std::string& url) {
    auto result=ProviderEndpoint(url);
    auto query=result.path.find(L'?');
    auto extra=query==std::wstring::npos?L"":result.path.substr(query);
    if(query!=std::wstring::npos)result.path.resize(query);
    while(!result.path.empty()&&result.path.back()==L'/')result.path.pop_back();
    const std::wstring suffix=L"/chat/completions";
    if(result.path.size()<suffix.size()||result.path.compare(result.path.size()-suffix.size(),suffix.size(),suffix)!=0)
        result.path+=suffix;
    result.path+=extra;
    return result;
}
std::string McpHttpPost(const std::string& url,const std::vector<std::pair<std::string,std::string>>& extraHeaders,
    std::string& sessionId,const std::string& version,const std::string& payload,const Cancellation& cancel) {
    static thread_local unsigned depth=0;
    if(depth>=4)throw std::runtime_error("MCP server sent too many nested requests.");
    struct DepthGuard {unsigned& value;explicit DepthGuard(unsigned& depth):value(depth){++value;}~DepthGuard(){--value;}} guard(depth);
    auto endpoint=ProviderEndpoint(url);
    HttpHandle session(WinHttpOpen(L"NativeCodexHarness/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts(session.value,10000,10000,30000,30000);
    HttpHandle connection(WinHttpConnect(session.value,endpoint.host.c_str(),endpoint.port,0));
    AsyncRequest request;request.payload=payload;
    request.handle=WinHttpOpenRequest(connection.value,L"POST",endpoint.path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,endpoint.secure?WINHTTP_FLAG_SECURE:0);
    if(!request.handle)AsyncRequest::Fail(GetLastError());
    DWORD_PTR context=reinterpret_cast<DWORD_PTR>(&request);
    if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)))AsyncRequest::Fail(GetLastError());
    if(WinHttpSetStatusCallback(request.handle,AsyncRequest::Callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)AsyncRequest::Fail(GetLastError());
    request.callbackInstalled=true;DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)))AsyncRequest::Fail(GetLastError());
    std::wstring headers=L"Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n";
    if(!sessionId.empty())headers+=L"Mcp-Session-Id: "+Wide(sessionId)+L"\r\n";
    if(!version.empty())headers+=L"MCP-Protocol-Version: "+Wide(version)+L"\r\n";
    for(const auto& header:extraHeaders)headers+=Wide(header.first)+L": "+Wide(header.second)+L"\r\n";
    auto deadline=GetTickCount64()+30000;
    try {
        request.Begin(cancel);request.Wait(WinHttpSendRequest(request.handle,headers.c_str(),static_cast<DWORD>(headers.size()),request.payload.data(),static_cast<DWORD>(request.payload.size()),static_cast<DWORD>(request.payload.size()),context),cancel,deadline);
        request.Begin(cancel);request.Wait(WinHttpReceiveResponse(request.handle,nullptr),cancel,deadline);
        DWORD status=0,size=sizeof(status);
        if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX))AsyncRequest::Fail(GetLastError());
        if(status<200||status>=300)throw std::runtime_error("MCP HTTP request failed (HTTP "+std::to_string(status)+").");
        auto header=[&](DWORD query,const wchar_t* name) {
            DWORD bytes=0;WinHttpQueryHeaders(request.handle,query,name,nullptr,&bytes,WINHTTP_NO_HEADER_INDEX);
            if(!bytes)return std::wstring{};std::wstring value(bytes/sizeof(wchar_t),L'\0');
            if(!WinHttpQueryHeaders(request.handle,query,name,value.data(),&bytes,WINHTTP_NO_HEADER_INDEX))return std::wstring{};
            value.resize(bytes/sizeof(wchar_t));while(!value.empty()&&value.back()==L'\0')value.pop_back();return value;
        };
        auto assigned=Utf8(header(WINHTTP_QUERY_CUSTOM,L"Mcp-Session-Id"));
        if(!assigned.empty()) {
            if(assigned.size()>1024||assigned.find_first_of("\r\n")!=assigned.npos)throw std::runtime_error("Invalid MCP session ID.");
            sessionId=assigned;
        }
        auto type=header(WINHTTP_QUERY_CONTENT_TYPE,WINHTTP_HEADER_NAME_BY_INDEX);
        bool sse=type.find(L"text/event-stream")!=std::wstring::npos;
        auto sent=Json::parse(payload);bool notification=!sent.contains("id");
        if(notification&&status==202)return {};
        std::string body,line,event;size_t received=0;
        auto eventResponse=[&]() -> std::string {
            if(event.empty())return {};auto message=Json::parse(event,nullptr,false);event.clear();
            if(message.is_discarded())throw std::runtime_error("Invalid MCP event JSON.");
            if(!notification&&message.is_object()&&message.contains("id")&&message["id"]==sent["id"]&&!message.contains("method"))return message.dump();
            if(message.is_object()&&message.contains("method")&&message.contains("id")) {
                auto response=Json{{"jsonrpc","2.0"},{"id",message["id"]}};
                if(message["method"]=="ping")response["result"]=Json::object();
                else response["error"]={{"code",-32601},{"message","Client capability not supported"}};
                McpHttpPost(url,extraHeaders,sessionId,version,response.dump(),cancel);
            }
            return {};
        };
        for(;;) {
            request.Begin(cancel);request.Wait(WinHttpReadData(request.handle,request.buffer.data(),static_cast<DWORD>(request.buffer.size()),nullptr),cancel,deadline);
            if(!request.bytes)break;
            received+=request.bytes;if(received>4*1024*1024)throw std::runtime_error("MCP response exceeded 4 MB.");
            if(!sse){body.append(request.buffer.data(),request.bytes);continue;}
            for(DWORD i=0;i<request.bytes;++i) {
                char c=request.buffer[i];if(c!='\n'){line+=c;continue;}
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                if(line.empty()){auto response=eventResponse();if(!response.empty())return response;}
                else if(line.compare(0,5,"data:")==0){if(!event.empty())event+='\n';event+=line.substr(line.size()>5&&line[5]==' '?6:5);}
                line.clear();
            }
        }
        if(sse){auto response=eventResponse();if(!response.empty())return response;throw std::runtime_error("MCP event stream ended before the response.");}
        return body;
    }catch(const Cancelled&){throw std::runtime_error("MCP request cancelled.");}
}
std::vector<std::string> FetchModels(const Config& config,const Cancellation& cancel) {
    ValidateConfig(config);
    auto endpoint=InferenceEndpoint(config.url);
    auto query=endpoint.path.find(L'?');
    auto queryString=query==std::wstring::npos?L"":endpoint.path.substr(query);
    if(query!=std::wstring::npos)endpoint.path.resize(query);
    const std::wstring suffix=L"/chat/completions";
    if(endpoint.path.size()>=suffix.size()&&endpoint.path.compare(endpoint.path.size()-suffix.size(),suffix.size(),suffix)==0)
        endpoint.path.resize(endpoint.path.size()-suffix.size());
    else throw std::runtime_error("Use a URL ending in /chat/completions to load models.");
    endpoint.path+=L"/models"+queryString;
    HttpHandle session(WinHttpOpen(L"NativeCodexHarness/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts(session.value,10000,10000,10000,15000);
    HttpHandle connection(WinHttpConnect(session.value,endpoint.host.c_str(),endpoint.port,0));
    AsyncRequest request;
    request.handle=WinHttpOpenRequest(connection.value,L"GET",endpoint.path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,endpoint.secure?WINHTTP_FLAG_SECURE:0);
    if(!request.handle)AsyncRequest::Fail(GetLastError());
    DWORD_PTR context=reinterpret_cast<DWORD_PTR>(&request);
    if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)))AsyncRequest::Fail(GetLastError());
    if(WinHttpSetStatusCallback(request.handle,AsyncRequest::Callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)AsyncRequest::Fail(GetLastError());
    request.callbackInstalled=true;
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)))AsyncRequest::Fail(GetLastError());
    std::wstring headers=L"Accept: application/json\r\n";
    if(!config.key.empty())headers+=L"Authorization: Bearer "+Wide(config.key)+L"\r\n";
    auto deadline=GetTickCount64()+15000;
    try {
        request.Begin(cancel);request.Wait(WinHttpSendRequest(request.handle,headers.c_str(),static_cast<DWORD>(headers.size()),WINHTTP_NO_REQUEST_DATA,0,0,context),cancel,deadline);
        request.Begin(cancel);request.Wait(WinHttpReceiveResponse(request.handle,nullptr),cancel,deadline);
        DWORD status=0,size=sizeof(status);
        if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX))AsyncRequest::Fail(GetLastError());

        std::string body;
        for(;;){request.Begin(cancel);request.Wait(WinHttpReadData(request.handle,request.buffer.data(),static_cast<DWORD>(request.buffer.size()),nullptr),cancel,deadline);if(!request.bytes)break;body.append(request.buffer.data(),request.bytes);if(body.size()>4*1024*1024)throw std::runtime_error("The model list is too large.");}
        if(status<200||status>=300){auto detail=ModelErrorDetail(body,config.key);throw std::runtime_error(ModelHttpError(status)+(detail.empty()?"":" Provider: "+detail));}
        return ParseModelList(body);
    }catch(const Cancelled&){throw std::runtime_error("Model loading cancelled.");}
}
Reply Complete(const Config& config, const std::vector<Message>& messages, const Cancellation& cancel, const Endpoint& overrideEndpoint, const OnDelta& onDelta) {
    auto started=GetTickCount64();auto complete=[&]() -> Reply {
    try {
        ValidateConfig(config);
        auto endpoint=overrideEndpoint;
        if(endpoint.host==L"openrouter.ai"&&endpoint.path==L"/api/v1/chat/completions"&&endpoint.port==443&&endpoint.secure) {
            auto timeout=endpoint.timeoutMs;endpoint=InferenceEndpoint(config.url);endpoint.timeoutMs=timeout;
        }
        if (cancel.IsCancelled()) throw Cancelled{};
        // Buffers are request members and remain alive until the final callback.
        HttpHandle session(WinHttpOpen(L"NativeCodexHarness/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (!WinHttpSetTimeouts(session.value, 10000, 15000, 30000, 120000)) AsyncRequest::Fail(GetLastError());
        HttpHandle connection(WinHttpConnect(session.value, endpoint.host.c_str(), endpoint.port, 0));
        AsyncRequest request;
        request.payload = BuildPayload(config, messages);
        request.handle = WinHttpOpenRequest(connection.value, L"POST", endpoint.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request.handle) AsyncRequest::Fail(GetLastError());
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&request);
        if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) AsyncRequest::Fail(GetLastError());
        if (WinHttpSetStatusCallback(request.handle, AsyncRequest::Callback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) AsyncRequest::Fail(GetLastError());
        request.callbackInstalled = true;
        // Never forward a bearer token to another host through a redirect.
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) AsyncRequest::Fail(GetLastError());
        std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\nAccept: text/event-stream\r\nX-Title: Native Codex Harness\r\n";
        if(!config.key.empty())headers+=L"Authorization: Bearer "+Wide(config.key)+L"\r\n";
        if (config.key.find_first_of("\r\n") != config.key.npos) return {L"The API key contains an invalid newline."};
        auto deadline = GetTickCount64() + endpoint.timeoutMs;
        request.Begin(cancel);
        request.Wait(WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(headers.size()),
            request.payload.data(), static_cast<DWORD>(request.payload.size()), static_cast<DWORD>(request.payload.size()), context), cancel, deadline);
        request.Begin(cancel);
        request.Wait(WinHttpReceiveResponse(request.handle, nullptr), cancel, deadline);
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) AsyncRequest::Fail(GetLastError());
        if (status < 200 || status >= 300) {
            auto failure=ParseReply(status, {});
            if(status==400||status==404||status==413||status==422) {
                bool image=false,pdf=false;
                for(const auto& message:messages)for(const auto& attachment:message.attachments) {
                    image=image||attachment.mime.compare(0,6,"image/")==0;
                    pdf=pdf||attachment.mime=="application/pdf";
                }
                if(image||pdf) {
                    failure.text=L"Attachment request rejected (HTTP "+std::to_wstring(status)+L") for model "+Wide(config.model)+L". ";
                    if(status==413)failure.text+=L"Reduce attachment sizes or start a new chat to clear attachment history.";
                    else {
                        if(image)failure.text+=L"Choose a model that accepts images in Settings. ";
                        if(pdf)failure.text+=L"PDFs require file support or provider PDF parsing; try exporting the document to text. ";
                        failure.text+=L"Start a new chat to remove earlier attachments before retrying without them.";
                    }
                }
            }
            return failure;
        }
        // Some compatible providers return a JSON completion despite stream=true.
        bool formatKnown=false,jsonResponse=false;
        std::string responseBody;
        StreamDecoder decoder(onDelta);
        size_t received = 0;
        for (;;) {
            request.Begin(cancel);
            request.Wait(WinHttpQueryDataAvailable(request.handle, nullptr), cancel, deadline);
            DWORD available = request.bytes;
            if (!available) break;
            request.Begin(cancel);
            request.Wait(WinHttpReadData(request.handle, request.buffer.data(), (std::min)(available, static_cast<DWORD>(request.buffer.size())), nullptr), cancel, deadline);
            DWORD count = request.bytes;
            if (!count) break;
            received += count;
            if (received > 4 * 1024 * 1024) return {L"The AI response exceeded the 4 MB limit."};
            if(!formatKnown) {
                // Detect from bytes: compatible services sometimes mislabel SSE as JSON.
                responseBody.append(request.buffer.data(),count);
                size_t first=0;
                if(static_cast<unsigned char>(responseBody[0])==0xef) {
                    if(responseBody.size()<3)continue;
                    if(responseBody.compare(0,3,"\xef\xbb\xbf")==0)first=3;
                }
                first=responseBody.find_first_not_of(" \r\n\t",first);
                if(first==responseBody.npos)continue;
                jsonResponse=responseBody[first]=='{'||responseBody[first]=='[';formatKnown=true;
                if(!jsonResponse){decoder.Feed(responseBody);responseBody.clear();}
            } else if(jsonResponse)responseBody.append(request.buffer.data(),count);
            else decoder.Feed(std::string_view(request.buffer.data(), count));
            if (!jsonResponse&&decoder.Done()) break;
        }
        if(jsonResponse) {
            auto reply=ParseReply(status,responseBody);
            if(reply.ok&&onDelta&&!reply.text.empty())onDelta(reply.text);
            return reply;
        }
        return decoder.Finish();
    } catch (const Cancelled&) { return {{}, false, true}; }
    catch (const std::runtime_error& error) { return {Wide(error.what())}; }
    catch (...) { return {L"Unable to complete the AI request. Please try again."}; }
    };
    auto reply=complete();reply.metrics.calls=1;reply.metrics.llmMs=GetTickCount64()-started;return reply;
}
}


