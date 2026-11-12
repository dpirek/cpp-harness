#define NOMINMAX
#include "tools/mcp_client.h"
#include "common/text_encoding.h"
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#pragma comment(lib,"crypt32.lib")

namespace chat {
using Json=nlohmann::json;
namespace {
constexpr size_t MaxBytes=4*1024*1024;
void Require(bool condition,const char* error){if(!condition)throw std::runtime_error(error);}
bool Clean(const std::string& value){return value.find('\0')==value.npos;}
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){Close();}
    void Close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=nullptr;}
};
std::wstring Quote(const std::wstring& value) {
    std::wstring result=L"\"";size_t slashes=0;
    for(auto c:value){if(c==L'\\'){++slashes;continue;}if(c==L'"'){result.append(slashes*2+1,L'\\');result+=c;}else{result.append(slashes,L'\\');result+=c;}slashes=0;}
    result.append(slashes*2,L'\\');return result+L'"';
}
struct EnvLess {bool operator()(const std::wstring& a,const std::wstring& b)const{return _wcsicmp(a.c_str(),b.c_str())<0;}};
std::vector<wchar_t> Environment(const Json& overrides) {
    std::map<std::wstring,std::wstring,EnvLess> entries;
    auto block=GetEnvironmentStringsW();if(!block)throw std::runtime_error("Could not read the process environment.");
    for(auto entry=block;*entry;entry+=wcslen(entry)+1){auto equal=wcschr(entry+(*entry==L'='?1:0),L'=');if(equal)entries[std::wstring(entry,equal)]=equal+1;}
    FreeEnvironmentStringsW(block);
    entries.erase(L"OPEN_ROUTER_KEY");entries.erase(L"OPENROUTER_API_KEY");
    for(const auto& item:overrides.items())entries[Wide(item.key())]=Wide(item.value().get<std::string>());
    std::vector<wchar_t> result;
    for(const auto& entry:entries){auto value=entry.first+L'='+entry.second;result.insert(result.end(),value.begin(),value.end());result.push_back(0);}
    result.push_back(0);if(result.size()==1)result.push_back(0);return result;
}
std::string Alias(const std::string& server,const std::string& tool) {
    // Stable names survive server ordering changes and fit provider function limits.
    uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:server+"/"+tool){hash^=c;hash*=1099511628211ULL;}
    std::string label="mcp_"+server+"_"+tool;
    for(char& c:label)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))c='_';
    label.resize((std::min)(label.size(),size_t(42)));
    return label+"_"+std::to_string(hash);
}
class Session {
    Json config_;
    const Cancellation& cancel_;
    Handle read_,write_,job_,process_;
    std::string pending_,session_,version_;
    int nextId_=0;
    bool failed_=false;
    void Check(ULONGLONG deadline) {
        if(cancel_.IsCancelled())throw std::runtime_error("MCP request cancelled.");
        if(GetTickCount64()>=deadline)throw std::runtime_error("MCP request timed out.");
    }
    void Send(const Json& message,ULONGLONG deadline) {
        auto bytes=message.dump()+"\n";Require(bytes.size()<=MaxBytes,"MCP request exceeded 4 MB.");
        Handle done;done.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);Require(done.value!=nullptr,"Could not create MCP write event.");
        std::atomic<bool> stop{false};bool success=false;
        std::thread writer([&] {
            size_t position=0;
            while(position<bytes.size()&&!stop){DWORD written=0;if(!WriteFile(write_.value,bytes.data()+position,static_cast<DWORD>((std::min)(bytes.size()-position,size_t(16384))),&written,nullptr)||!written)break;position+=written;}
            success=position==bytes.size();SetEvent(done.value);
        });
        auto now=GetTickCount64();HANDLE events[]{cancel_.Event(),done.value};
        auto wait=WaitForMultipleObjects(2,events,FALSE,now<deadline?static_cast<DWORD>(deadline-now):0);
        if(wait!=WAIT_OBJECT_0+1){stop=true;CancelSynchronousIo(writer.native_handle());if(job_.value)TerminateJobObject(job_.value,1);}
        writer.join();Check(deadline);Require(success,"Could not write to MCP server.");
    }
    Json Receive(ULONGLONG deadline) {
        for(;;) {
            Check(deadline);
            auto end=pending_.find('\n');
            if(end!=pending_.npos){auto line=pending_.substr(0,end);pending_.erase(0,end+1);if(line.empty()||line=="\r")continue;auto message=Json::parse(line,nullptr,false);Require(message.is_object(),"MCP server wrote invalid JSON to stdout.");return message;}
            DWORD available=0;Require(PeekNamedPipe(read_.value,nullptr,0,nullptr,&available,nullptr)!=FALSE,"MCP server closed stdout.");
            if(available){std::array<char,16384> bytes{};DWORD size=0;Require(ReadFile(read_.value,bytes.data(),(std::min)(available,static_cast<DWORD>(bytes.size())),&size,nullptr)&&size,"Could not read MCP response.");pending_.append(bytes.data(),size);Require(pending_.size()<=MaxBytes,"MCP response exceeded 4 MB.");}
            else {if(WaitForSingleObject(process_.value,0)==WAIT_OBJECT_0)throw std::runtime_error("MCP server exited before responding.");WaitForSingleObject(cancel_.Event(),20);}
        }
    }
    void Start(const std::filesystem::path& directory) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};Handle childIn,childOut,error,thread;
        Require(CreatePipe(&childIn.value,&write_.value,&inherit,65536)&&CreatePipe(&read_.value,&childOut.value,&inherit,65536),"Could not create MCP pipes.");
        Require(SetHandleInformation(write_.value,HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(read_.value,HANDLE_FLAG_INHERIT,0),"Could not protect MCP pipe handles.");
        error.value=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr);Require(error.value!=INVALID_HANDLE_VALUE,"Could not initialize MCP stderr.");
        job_.value=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(job_.value&&SetInformationJobObject(job_.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Could not initialize MCP process job.");
        auto executable=Wide(config_.at("command").get<std::string>());
        std::array<wchar_t,32768> path{};
        if(SearchPathW(nullptr,executable.c_str(),L".exe",static_cast<DWORD>(path.size()),path.data(),nullptr))executable=path.data();
        Require(std::filesystem::path(executable).extension()!=L".cmd"&&std::filesystem::path(executable).extension()!=L".bat","For .cmd/.bat servers, configure cmd.exe explicitly or use the runtime executable directly.");
        auto command=Quote(executable);for(const auto& arg:config_.value("args",Json::array()))command+=L" "+Quote(Wide(arg.get<std::string>()));
        Require(command.size()<32767,"MCP command line is too long.");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
        startup.StartupInfo.hStdInput=childIn.value;startup.StartupInfo.hStdOutput=childOut.value;startup.StartupInfo.hStdError=error.value;
        SIZE_T attributeSize=0;InitializeProcThreadAttributeList(nullptr,1,0,&attributeSize);std::vector<unsigned char> attributes(attributeSize);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        Require(InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attributeSize)!=FALSE,"Could not initialize MCP inherited handles.");
        HANDLE handles[]{childIn.value,childOut.value,error.value};
        bool valid=UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)!=FALSE;
        PROCESS_INFORMATION info{};auto environment=Environment(config_.value("env",Json::object()));
        auto cwd=config_.contains("cwd")?directory/Wide(config_["cwd"].get<std::string>()):directory;
        BOOL created=valid&&CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),cwd.c_str(),&startup.StartupInfo,&info);
        DeleteProcThreadAttributeList(startup.lpAttributeList);Require(created!=FALSE,"Could not launch MCP server. Check the command and working directory.");
        process_.value=info.hProcess;thread.value=info.hThread;
        if(!AssignProcessToJobObject(job_.value,process_.value)){TerminateProcess(process_.value,1);throw std::runtime_error("Could not attach MCP server to its process job.");}
        Require(ResumeThread(thread.value)!=static_cast<DWORD>(-1),"Could not start MCP server.");
    }
public:
    Session(Json config,const std::filesystem::path& directory,const Cancellation& cancel):config_(std::move(config)),cancel_(cancel) {
        if(config_.contains("command"))Start(directory);
    }
    ~Session(){write_.Close();if(process_.value)WaitForSingleObject(process_.value,100);job_.Close();}
    void Notify(const std::string& method) {
        Json message{{"jsonrpc","2.0"},{"method",method}};
        if(config_.contains("url"))Http(message);else Send(message,GetTickCount64()+30000);
    }
    std::string Http(const Json& message) {
        std::vector<std::pair<std::string,std::string>> headers;
        auto configuredHeaders=config_.value("headers",Json::object());
        for(const auto& item:configuredHeaders.items())headers.emplace_back(item.key(),item.value().get<std::string>());
        return McpHttpPost(config_["url"].get<std::string>(),headers,session_,version_,message.dump(),cancel_);
    }
    Json Request(const std::string& method,const Json& params=Json::object()) {
        Require(!failed_,"MCP connection is no longer available.");
        try {
            auto deadline=GetTickCount64()+30000;int id=++nextId_;
            Json message{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}},response;
            if(config_.contains("url"))response=Json::parse(Http(message));
            else {
                Send(message,deadline);size_t notifications=0;
                for(;;){response=Receive(deadline);Require(++notifications<=4096,"MCP server sent too many messages.");
                    if(response.contains("method")) {
                        if(response.contains("id")) {
                            Json answer{{"jsonrpc","2.0"},{"id",response["id"]}};
                            if(response["method"]=="ping")answer["result"]=Json::object();else answer["error"]={{"code",-32601},{"message","Client capability not supported"}};
                            Send(answer,deadline);
                        }continue;
                    }
                    if(response.contains("id")&&response["id"]==id)break;
                }
            }
            Require(response.is_object()&&response.value("jsonrpc",std::string{})=="2.0"&&response.contains("id")&&response["id"]==id,"Invalid MCP response ID or protocol.");
            if(response.contains("error"))throw std::runtime_error("MCP server returned a JSON-RPC error.");
            Require(response.contains("result")&&response["result"].is_object(),"Invalid MCP result.");return response["result"];
        }catch(...){failed_=true;job_.Close();throw;}
    }
    void Initialize() {
        auto result=Request("initialize",{{"protocolVersion","2025-11-25"},{"capabilities",Json::object()},{"clientInfo",{{"name","native-chat"},{"version","1.0"}}}});
        version_=result.value("protocolVersion",std::string{});
        Require(version_=="2025-11-25"||version_=="2025-06-18"||version_=="2025-03-26"||version_=="2024-11-05","MCP server selected an unsupported protocol version.");
        Require(result.contains("capabilities")&&result["capabilities"].is_object()&&result["capabilities"].contains("tools"),"MCP server does not expose tools.");
        Notify("notifications/initialized");
    }
};
}
void ValidateMcpConfig(const Json& config) {
    Require(config.is_object()&&config.contains("mcpServers")&&config["mcpServers"].is_object(),"Expected an object containing mcpServers.");
    if(config.contains("enabled"))Require(config["enabled"].is_boolean(),"The global enabled setting must be true or false.");
    Require(config["mcpServers"].size()<=16,"At most 16 MCP servers are supported.");
    for(const auto& item:config["mcpServers"].items()) {
        Require(!item.key().empty()&&item.key().size()<=128&&Clean(item.key()),"MCP server names must contain 1 to 128 characters.");Wide(item.key());
        const auto& server=item.value();Require(server.is_object(),"Each MCP server must be an object.");
        if(server.contains("enabled"))Require(server["enabled"].is_boolean(),"enabled must be true or false.");
        Require(server.contains("command")!=server.contains("url"),"Each MCP server needs either command or url.");
        for(const auto& field:server.items())Require(field.key()=="enabled"||field.key()=="command"||field.key()=="args"||field.key()=="env"||field.key()=="cwd"||field.key()=="url"||field.key()=="headers","Unknown MCP server field. Use command/args/env/cwd or url/headers, plus enabled.");
        if(server.contains("command")) {
            Require(!server.contains("headers"),"headers are only supported for remote MCP servers.");
            Require(server["command"].is_string()&&!server["command"].get<std::string>().empty()&&Clean(server["command"]),"command must be a nonempty string.");
            if(server.contains("cwd"))Require(server["cwd"].is_string()&&Clean(server["cwd"]),"cwd must be a string.");
            auto args=server.value("args",Json::array());Require(args.is_array()&&args.size()<=128,"args must be an array of at most 128 strings.");
            for(const auto& arg:args)Require(arg.is_string()&&Clean(arg),"Each command argument must be a string without NUL characters.");
            auto env=server.value("env",Json::object());Require(env.is_object(),"env must be an object of string values.");
            for(const auto& entry:env.items())Require(!entry.key().empty()&&entry.key().find('=')==entry.key().npos&&Clean(entry.key())&&entry.value().is_string()&&Clean(entry.value()),"Invalid MCP environment variable.");
        } else {
            Require(!server.contains("args")&&!server.contains("env")&&!server.contains("cwd"),"Remote MCP servers use url and headers, not command arguments or environment variables.");
            Require(server["url"].is_string()&&Clean(server["url"]),"url must be a string without NUL characters.");ProviderEndpoint(server["url"].get<std::string>());
            auto headers=server.value("headers",Json::object());Require(headers.is_object(),"headers must be an object of string values.");
            for(const auto& header:headers.items()) {
                auto name=header.key();std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                Require(!name.empty()&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_")==name.npos&&header.value().is_string()&&Clean(header.value())&&header.value().get<std::string>().find_first_of("\r\n")==std::string::npos,"Invalid MCP HTTP header.");
                Require(name!="host"&&name!="content-length"&&name!="content-type"&&name!="accept"&&name!="mcp-session-id"&&name!="mcp-protocol-version"&&name!="transfer-encoding"&&name!="connection","This MCP HTTP header is managed by the app.");
            }
        }
    }
    Require(config.dump().size()<=256*1024,"MCP configuration exceeds 256 KB.");
}
Json LoadMcpConfig(const std::filesystem::path& directory) {
    auto path=directory/L"mcp-servers.json";if(!std::filesystem::exists(path))return {{"mcpServers",Json::object()}};
    try {
        Require(std::filesystem::file_size(path)<=2*1024*1024,"MCP configuration file is too large.");
        std::ifstream file(path,std::ios::binary);auto stored=Json::parse(file);auto bytes=stored.at("protected_config").get<std::vector<unsigned char>>();
        DATA_BLOB input{static_cast<DWORD>(bytes.size()),bytes.data()},output{};
        Require(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)!=FALSE,"Could not decrypt MCP configuration.");
        std::string text(reinterpret_cast<char*>(output.pbData),output.cbData);SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);
        auto config=Json::parse(text);ValidateMcpConfig(config);return config;
    }catch(...){throw std::runtime_error("Could not load MCP server settings. Open Settings > MCP Servers to replace them.");}
}
void SaveMcpConfig(const std::filesystem::path& directory,const Json& config) {
    ValidateMcpConfig(config);auto text=config.dump();
    DATA_BLOB input{static_cast<DWORD>(text.size()),reinterpret_cast<BYTE*>(text.data())},output{};
    Require(CryptProtectData(&input,L"Chat MCP servers",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)!=FALSE,"Could not encrypt MCP settings.");
    std::vector<unsigned char> bytes(output.pbData,output.pbData+output.cbData);LocalFree(output.pbData);
    auto temporary=directory/L"mcp-servers.json.tmp",destination=directory/L"mcp-servers.json";
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<Json{{"protected_config",bytes}}.dump();file.flush();Require(static_cast<bool>(file),"Could not save MCP settings.");}
    Require(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Could not replace MCP settings.");
}
struct McpClient::State {
    struct Tool {std::string name,definition,label;Session* session;};
    std::vector<std::unique_ptr<Session>> sessions;
    std::map<std::string,Tool> tools;
};
McpClient::McpClient(const Json& config,const std::filesystem::path& directory,const Cancellation& cancel,const McpStatus& status):state_(std::make_unique<State>()) {
    ValidateMcpConfig(config);
    if(!config.value("enabled",true))return;
    for(const auto& server:config["mcpServers"].items()) {
        if(cancel.IsCancelled())break;if(!server.value().value("enabled",true))continue;
        try {
            if(status)status(L"MCP \u00b7 Connecting to "+Wide(server.key()));
            auto session=std::make_unique<Session>(server.value(),directory,cancel);session->Initialize();
            std::string cursor;std::set<std::string> cursors,names;std::map<std::string,State::Tool> discovered;
            for(int page=0;page<32;++page) {
                auto tools=session->Request("tools/list",cursor.empty()?Json::object():Json{{"cursor",cursor}});
                Require(tools.contains("tools")&&tools["tools"].is_array(),"Invalid MCP tools list.");
                for(const auto& tool:tools["tools"]) {
                    Require(tool.is_object()&&tool.contains("name")&&tool["name"].is_string()&&tool.contains("inputSchema")&&tool["inputSchema"].is_object(),"Invalid MCP tool definition.");
                    auto name=tool["name"].get<std::string>();Require(!name.empty()&&name.size()<=128&&Clean(name)&&names.insert(name).second,"Invalid or duplicate MCP tool name.");
                    Require(tool["inputSchema"].value("type",std::string("object"))=="object","MCP tool input schema must describe an object.");
                    if(tool.contains("execution")&&tool["execution"].is_object()&&tool["execution"].value("taskSupport",std::string{})=="required")continue;
                    auto alias=Alias(server.key(),name),description="MCP server "+server.key()+", tool "+name+". "+tool.value("description",std::string{});
                    auto definition=Json{{"type","function"},{"function",{{"name",alias},{"description",description},{"parameters",tool["inputSchema"]}}}}.dump();
                    Require(definition.size()<=128*1024&&discovered.size()+state_->tools.size()<127,"MCP tool definitions exceed the supported limit.");
                    discovered.emplace(alias,State::Tool{name,definition,server.key()+" / "+name,session.get()});
                }
                if(!tools.contains("nextCursor")||tools["nextCursor"].is_null())break;
                Require(tools["nextCursor"].is_string(),"Invalid MCP pagination cursor.");cursor=tools["nextCursor"].get<std::string>();
                if(cursor.empty())break;Require(page<31&&cursors.insert(cursor).second,"MCP tool pagination did not complete.");
            }
            auto count=discovered.size();state_->sessions.push_back(std::move(session));state_->tools.insert(discovered.begin(),discovered.end());
            if(status)status(L"MCP \u00b7 "+Wide(server.key())+L" connected ("+std::to_wstring(count)+L" tools)");
        }catch(const std::exception& error){if(!cancel.IsCancelled()&&status)status(L"MCP \u00b7 "+Wide(server.key())+L": "+Wide(error.what()));}
    }
}
McpClient::~McpClient()=default;
std::vector<std::string> McpClient::Definitions()const {std::vector<std::string> result;for(const auto& tool:state_->tools)result.push_back(tool.second.definition);return result;}
bool McpClient::HasTool(const std::string& name)const{return state_->tools.count(name)!=0;}
std::string McpClient::Label(const std::string& name)const {auto tool=state_->tools.find(name);return tool==state_->tools.end()?name:tool->second.label;}
Json McpClient::Call(const std::string& name,const Json& arguments) {
    auto found=state_->tools.find(name);if(found==state_->tools.end())return {{"error","Unknown MCP tool."}};
    if(!arguments.is_object())return {{"error","MCP tool arguments must be an object."}};
    try {
        auto result=found->second.session->Request("tools/call",{{"name",found->second.name},{"arguments",arguments}});
        // Binary content is not usable by this text-only chat; omit large base64 payloads.
        if(result.contains("content")&&result["content"].is_array())for(auto& content:result["content"]) {
            if(content.is_object()&&content.contains("data")){content.erase("data");content["note"]="Binary content omitted by text-only client.";}
            if(content.is_object()&&content.contains("resource")&&content["resource"].is_object())content["resource"].erase("blob");
        }
        if(result.dump().size()>64*1024)return {{"error","MCP tool result exceeded 64 KB; narrow the request."}};
        return result;
    }catch(const std::exception& error){return {{"error",error.what()}};}
}
}
