#include "agent.h"
#include "computer_tool.h"
#include <algorithm>
#include "powershell_tool.h"
#include "text_encoding.h"
#include "third_party/nlohmann/json.hpp"
#include "mcp_client.h"
#include "tool_format.h"

namespace chat {
namespace {
void Redact(std::wstring& value,const std::wstring& key) {
    if(key.empty())return;
    size_t position=0;
    while((position=value.find(key,position))!=value.npos){value.replace(position,key.size(),L"[redacted]");position+=10;}
}
std::wstring McpDisplay(const nlohmann::json& result) {
    std::wstring text;
    if(result.contains("content")&&result["content"].is_array())for(const auto& item:result["content"]) {
        if(!item.is_object())continue;
        if(item.contains("text")&&item["text"].is_string())text+=ToolOutput(Wide(item["text"].get<std::string>()))+L"\n";
        else if(item.contains("resource")&&item["resource"].is_object()&&item["resource"].contains("text")&&item["resource"]["text"].is_string())text+=ToolOutput(Wide(item["resource"]["text"].get<std::string>()))+L"\n";
        else if(item.contains("uri")&&item["uri"].is_string())text+=Wide(item["uri"].get<std::string>())+L"\n";
        else if(item.contains("note")&&item["note"].is_string())text+=Wide(item["note"].get<std::string>())+L"\n";
    }
    if(result.contains("structuredContent")) {
        bool duplicate=false;
        if(result.contains("content")&&result["content"].is_array())for(const auto& item:result["content"])
            if(item.is_object()&&item.contains("text")&&item["text"].is_string()) {
                auto parsed=nlohmann::json::parse(item["text"].get<std::string>(),nullptr,false);
                if(!parsed.is_discarded()&&parsed==result["structuredContent"])duplicate=true;
            }
        if(!duplicate)text+=ToolOutput(Wide(result["structuredContent"].dump(2)))+L"\n";
    }
    if(text.empty())text=ToolOutput(Wide(result.dump(2)));
    return text;
}
}
AgentResult RunAgent(const Config& config,std::vector<Message> history,const std::filesystem::path& directory,
    const Cancellation& cancel,const Endpoint& endpoint,const OnAgentUpdate& onUpdate) {
    AgentResult result;int totalTools=0;
    CallMetrics metrics;metrics.model=Wide(config.model);auto started=GetTickCount64();
    auto key=Wide(config.key);
    auto emit=[&](UpdateKind kind,std::wstring text){if(onUpdate&&!cancel.IsCancelled()){Redact(text,key);onUpdate({kind,std::move(text)});}};
    auto status=[&](const std::wstring& state){auto current=metrics;current.elapsedMs=GetTickCount64()-started;emit(UpdateKind::Status,CallActivity(current,state));};
    std::unique_ptr<McpClient> mcp;
    Config requestConfig=config;
    try {
        mcp=std::make_unique<McpClient>(LoadMcpConfig(directory),directory,cancel,[&](const std::wstring& text){emit(UpdateKind::Tool,text);});
        requestConfig.extraTools=mcp->Definitions();
    }catch(const std::exception& error){emit(UpdateKind::Tool,Wide(error.what()));}
    for(int round=0;round<8;++round) {
        if(cancel.IsCancelled()){result.reply.cancelled=true;break;}
        auto active=metrics;++active.calls;active.elapsedMs=GetTickCount64()-started;emit(UpdateKind::Status,CallActivity(active,L"Waiting for response"));
        bool firstDelta=true;auto callStarted=GetTickCount64();
        auto reply=Complete(requestConfig,history,cancel,endpoint,[&](const std::wstring& delta){
            if(firstDelta){firstDelta=false;active.llmMs=metrics.llmMs+GetTickCount64()-callStarted;active.elapsedMs=GetTickCount64()-started;emit(UpdateKind::Status,CallActivity(active,L"Streaming"));}
            emit(UpdateKind::Delta,delta);
        });
        metrics.Add(reply.metrics);status(reply.ok?L"Response received":L"Failed");
        Redact(reply.text,key);
        if(!reply.ok){result.reply=std::move(reply);break;}
        Message assistant{L"assistant",reply.text};assistant.toolCalls=reply.toolCalls;
        history.push_back(std::move(assistant));
        if(reply.toolCalls.empty()){result.reply=std::move(reply);break;}
        // Close commentary before displaying tool activity and the next model turn.
        emit(UpdateKind::SegmentEnd,reply.text);
        std::vector<Attachment> screenshots;
        for(auto& call:reply.toolCalls) {
            if(cancel.IsCancelled()){result.reply.cancelled=true;break;}
            nlohmann::json output;
            if(round==7||totalTools>=16) {
                output={{"error","Agent step limit reached; this command was not run."}};
            } else if(mcp&&mcp->HasTool(call.name)) {
                auto args=nlohmann::json::parse(call.arguments,nullptr,false);
                if(!args.is_object())output={{"error","MCP tool arguments must be a JSON object."}};
                else {
                    auto label=Wide(mcp->Label(call.name));
                    ++totalTools;status(L"MCP running \u00b7 "+label);
                    emit(UpdateKind::Tool,L"MCP \u00b7 "+label);
                    output=mcp->Call(call.name,args);
                    if(cancel.IsCancelled()){result.reply.cancelled=true;break;}
                    auto text=Wide(output.dump());Redact(text,key);output=nlohmann::json::parse(Utf8(text));
                    auto display=McpDisplay(output);
                    emit(UpdateKind::Tool,display.substr(0,4000)+(display.size()>4000?L"\n[output truncated]":L""));
                }
            } else if(call.name=="computer") {
                ++totalTools;
                auto args=nlohmann::json::parse(call.arguments,nullptr,false);
                status(L"Computer running");
                emit(UpdateKind::Tool,L"Computer action");
                auto computer=RunComputer(args,cancel);
                output=std::move(computer.output);
                if(!computer.screenshot.data.empty())screenshots.push_back(std::move(computer.screenshot));
                emit(UpdateKind::Tool,ToolOutput(Wide(output.dump())));
            } else if(call.name!="powershell") {
                output={{"error","Unknown tool. Use one of the tools listed in this request."}};
                emit(UpdateKind::Tool,L"Tool rejected \u00b7 "+Wide(call.name));
            } else {
                auto args=nlohmann::json::parse(call.arguments,nullptr,false);
                bool valid=args.is_object()&&args.contains("command")&&args["command"].is_string();
                if(valid)for(auto& item:args.items())if(item.key()!="command"&&item.key()!="timeout_seconds")valid=false;
                if(valid&&args.contains("timeout_seconds")&&!args["timeout_seconds"].is_number_integer())valid=false;
                int timeout=valid?args.value("timeout_seconds",30):0;
                if(!valid||timeout<1||timeout>120) {
                    output={{"error","Invalid arguments: command must be a string and timeout_seconds an integer from 1 to 120."}};
                    emit(UpdateKind::Tool,L"PowerShell \u00b7 Invalid arguments; command not run");
                } else {
                    ++totalTools;
                    auto command=Wide(args["command"].get<std::string>());
                    status(L"PowerShell running \u00b7 Command "+std::to_wstring(totalTools));
                    emit(UpdateKind::Tool,L"```powershell\n"+command+L"\n```");
                    auto tool=RunPowerShell(command,directory,cancel,static_cast<unsigned>(timeout));
                    if(tool.cancelled){result.reply.cancelled=true;break;}
                    Redact(tool.output,key);
                    output={{"output",Utf8(tool.output)},{"exit_code",tool.exitCode},{"timed_out",tool.timedOut},{"truncated",tool.truncated}};
                    auto display=tool.output.substr(0,4000);
                    if(tool.output.size()>4000||tool.truncated)display+=L"\n[output truncated]";
                    if(display.empty())display=L"(no output)";
                    emit(UpdateKind::Tool,(tool.timedOut?L"PowerShell \u00b7 Timed out":L"PowerShell \u00b7 Exit "+std::to_wstring(tool.exitCode))+L"\n"+ToolOutput(display));
                }
            }
            Message toolMessage{L"tool",Wide(output.dump())};toolMessage.toolCallId=call.id;
            history.push_back(std::move(toolMessage));
        }
        if(!screenshots.empty()) {
            // Images follow all matching tool results for Chat Completions compatibility.
            for(auto& message:history)message.attachments.erase(std::remove_if(message.attachments.begin(),message.attachments.end(),[](const Attachment& a){return a.name==L"Desktop screenshot";}),message.attachments.end());
            Message image{L"user",L"Computer screenshot from the preceding tool call. Use the tool's desktop bounds for coordinates."};
            image.attachments=std::move(screenshots);history.push_back(std::move(image));
        }
        if(result.reply.cancelled)break;
        if(round==7||totalTools>=16){result.reply.text=L"Stopped at the agent step limit. Send a follow-up to continue.";break;}
    }
    metrics.elapsedMs=GetTickCount64()-started;result.reply.metrics=metrics;
    result.history=std::move(history);return result;
}
}
