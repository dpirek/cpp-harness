#include "chat_client.h"
#include "chat_worker.h"
#include "stream_decoder.h"
#include "agent.h"
#include "powershell_tool.h"
#include "text_encoding.h"
#include "code_format.h"
#include "mcp_client.h"
#include "markdown.h"
#include "tool_format.h"
#include "third_party/nlohmann/json.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace chat;
using namespace std::chrono_literals;
void Check(bool value, const char* name) { if (!value) throw std::runtime_error(name); }
struct SavedEnv {
    std::wstring name, value; bool existed;
    explicit SavedEnv(const wchar_t* n) : name(n) {
        DWORD size=GetEnvironmentVariableW(n,nullptr,0);existed=size!=0;
        if(size){value.resize(size);GetEnvironmentVariableW(n,value.data(),size);value.resize(size-1);}
        SetEnvironmentVariableW(n,nullptr);
    }
    ~SavedEnv(){SetEnvironmentVariableW(name.c_str(),existed?value.c_str():nullptr);}
};
int main(int argc,char** argv) {
    try {
        auto blocks=FormatBlocks(L"Response\n```XML\r\n<root name=\"value\"/>\r\n```\nDone");
        Check(blocks.size()==3&&blocks[1].code&&blocks[1].language==L"xml"&&blocks[1].text==L"<root name=\"value\"/>\n","XML fences and CRLF formatting");
        auto xmlColors=Highlight(blocks[1].text,L"xml");
        Check(xmlColors[0]==CodeColor::Tag&&xmlColors[11]==CodeColor::String,"XML tags and attribute strings");
        auto psColors=Highlight(L"Get-Item $env:TEMP # comment",L"ps");
        Check(psColors[0]==CodeColor::Keyword&&psColors[9]==CodeColor::Variable&&psColors[19]==CodeColor::Comment,"PowerShell commands variables and comments");
        auto partialBlock=FormatBlocks(L"```pwsh\n  Get-Item\n");
        Check(partialBlock.size()==1&&partialBlock[0].code&&partialBlock[0].text==L"  Get-Item\n","Streaming code preserves indentation");
        auto jsonOutput=ToolOutput(L" {\"name\":\"value\",\"enabled\":true,\"count\":42,\"empty\":null} ");
        auto jsonBlocks=FormatBlocks(jsonOutput);
        Check(jsonBlocks.size()==1&&jsonBlocks[0].language==L"json"&&jsonBlocks[0].text.find(L"\n  \"name\"")!=std::wstring::npos,"Tool JSON detected and pretty printed");
        const std::wstring jsonSample=L"{\"name\":\"value\",\"enabled\":true,\"count\":42,\"empty\":null}";
        auto jsonColors=Highlight(jsonSample,L"json");
        Check(jsonColors[jsonSample.find(L"\"name\"")]==CodeColor::Variable&&jsonColors[jsonSample.find(L"\"value\"")]==CodeColor::String&&jsonColors[jsonSample.find(L"true")]==CodeColor::Keyword&&jsonColors[jsonSample.find(L"42")]==CodeColor::Number&&jsonColors[jsonSample.find(L"null")]==CodeColor::Keyword,"JSON properties strings literals and numbers highlighted");
        Check(FormatBlocks(ToolOutput(L"<root name=\"value\">text</root>"))[0].language==L"xml","Tool XML auto detected");
        Check(ToolOutput(L"ordinary output [not JSON]")==L"ordinary output [not JSON]","Plain tool logs remain unmodified");
        auto quotedFence=FormatBlocks(ToolOutput(L"{\"text\":\"```\"}"));
        Check(quotedFence.size()==1&&quotedFence[0].language==L"json"&&quotedFence[0].text.find(L"```")!=std::wstring::npos,"Tool code containing fences does not break its code block");
        auto markdown=ParseMarkdown(L"# Heading\n\nA **bold** and *italic* `code` [link](https://example.com).\n\n- one\n  - nested\n\n> quoted\n\n```powershell\nGet-Item $x\n```\n");
        Check(markdown.front().kind==MdKind::Heading&&markdown.front().heading==1&&markdown.front().runs[0].text==L"Heading","Markdown headings remove source markers");
        bool boldRun=false,italicRun=false,codeRun=false,linkRun=false,nestedList=false,quoteBlock=false,fencedCode=false;
        for(const auto& block:markdown){nestedList|=block.indent==2&&!block.marker.empty();quoteBlock|=block.quote==1;fencedCode|=block.kind==MdKind::Code&&block.language==L"powershell"&&block.text==L"Get-Item $x\n";for(const auto& run:block.runs){boldRun|=(run.style&MdBold)!=0;italicRun|=(run.style&MdItalic)!=0;codeRun|=(run.style&MdCode)!=0;linkRun|=(run.style&MdLink)&&run.href==L"https://example.com";}}
        Check(boldRun&&italicRun&&codeRun&&linkRun&&nestedList&&quoteBlock&&fencedCode,"Markdown inline styles nested lists quotes links and fenced code");
        auto table=ParseMarkdown(L"| Name | Value |\n| :--- | ---: |\n| **one** | two |\n");
        Check(table.size()==1&&table[0].kind==MdKind::Table&&table[0].rows.size()==2&&table[0].rows[0][0].header&&table[0].rows[1][1].align==3,"Markdown table headers rows and alignment");
        auto entities=ParseMarkdown(L"&amp; &#x1f680; &copy; <script>alert(1)</script>");
        std::wstring entityText;for(const auto& run:entities[0].runs)entityText+=run.text;
        Check(entityText==L"& \U0001f680 \u00a9 <script>alert(1)</script>","Markdown entity decoding and literal HTML");
        auto incompleteMarkdown=ParseMarkdown(L"**unfinished\n\n```xml\n<root>");
        Check(incompleteMarkdown.size()==2&&incompleteMarkdown[0].runs[0].text==L"**unfinished"&&incompleteMarkdown[1].kind==MdKind::Code&&incompleteMarkdown[1].text.find(L"<root>")!=std::wstring::npos,"Incomplete streamed Markdown remains readable");
        if(argc==2&&std::string(argv[1])=="--live") {
            Cancellation cancel;
            auto reply=Complete(LoadConfig(std::filesystem::current_path()),{{L"user",L"Reply with exactly: Harness connected"}},cancel);
            std::cout<<(reply.ok?"Live request succeeded: ":"Live request failed: ")<<Utf8(reply.text)<<"\n";
            return reply.ok?0:1;
        }
        Check(argc==2,"Pass the local mock server port.");
        SavedEnv key(L"OPEN_ROUTER_KEY"),alias(L"OPENROUTER_API_KEY"),model(L"OPENROUTER_MODEL");
        SavedEnv genericKey(L"AI_API_KEY"),genericModel(L"AI_MODEL"),genericUrl(L"AI_BASE_URL"),legacyUrl(L"AI_PROVIDER_URL");
        auto directory=std::filesystem::current_path()/L"build"/L"test-config";
        std::filesystem::create_directories(directory);
        std::filesystem::remove(directory/L"provider-settings.json");
        std::filesystem::remove(directory/L"mcp-servers.json");
        auto envFile=directory/L".env";
        {std::ofstream file(envFile,std::ios::binary);file<<"\xef\xbb\xbf# test config\nexport OPEN_ROUTER_KEY = 'test-key'\r\nOPENROUTER_MODEL = \"test/model\"\n";}
        auto config=LoadConfig(directory);
        Check(config.key=="test-key"&&config.model=="test/model","Quoted BOM .env parsing");
        SetEnvironmentVariableW(L"OPENROUTER_API_KEY",L"environment-key");
        SetEnvironmentVariableW(L"OPENROUTER_MODEL",L"env/model");
        auto overridden=LoadConfig(directory);
        Check(overridden.key=="environment-key"&&overridden.model=="env/model","Environment overrides file aliases");
        SetEnvironmentVariableW(L"OPEN_ROUTER_KEY",L"primary");
        Check(LoadConfig(directory).key=="primary","Primary environment key precedence");
        SetEnvironmentVariableW(L"OPEN_ROUTER_KEY",nullptr);SetEnvironmentVariableW(L"OPENROUTER_API_KEY",nullptr);SetEnvironmentVariableW(L"OPENROUTER_MODEL",nullptr);
        {std::ofstream file(envFile);file<<"OPENROUTER_API_KEY=alias-key # comment\n";}
        auto fallback=LoadConfig(directory);
        Check(fallback.key=="alias-key"&&fallback.model=="openrouter/auto","Alias and model fallback");
        SetEnvironmentVariableW(L"AI_API_KEY",L"generic-key");
        SetEnvironmentVariableW(L"AI_MODEL",L"generic/model");
        SetEnvironmentVariableW(L"AI_BASE_URL",L"http://localhost:8000/v1");
        auto generic=LoadConfig(directory);
        Check(generic.key=="generic-key"&&generic.model=="generic/model"&&generic.url=="http://localhost:8000/v1","Generic provider environment configuration");
        SetEnvironmentVariableW(L"AI_API_KEY",nullptr);SetEnvironmentVariableW(L"AI_MODEL",nullptr);SetEnvironmentVariableW(L"AI_BASE_URL",nullptr);
        auto baseEndpoint=InferenceEndpoint("http://localhost:8000/v1/?version=1");
        Check(baseEndpoint.path==L"/v1/chat/completions?version=1","Base URL normalization preserves query");
        Config keyless;keyless.url="http://localhost:8000/v1";
        SaveConfig(directory,keyless);
        Check(LoadConfig(directory).key.empty(),"Keyless settings persist without borrowing environment credentials");
        Config saved;saved.key="settings-secret";saved.model="custom/model";saved.url="http://localhost:8080/v1/chat/completions?version=1";
        SaveConfig(directory,saved);
        auto restored=LoadConfig(directory);
        Check(restored.key==saved.key&&restored.model==saved.model&&restored.url==saved.url,"Provider settings persist with encrypted key");
        {std::ifstream file(directory/L"provider-settings.json");std::string body((std::istreambuf_iterator<char>(file)),{});Check(body.find(saved.key)==std::string::npos,"Saved key is not plaintext");}
        auto parsedProvider=ProviderEndpoint(saved.url);
        Check(parsedProvider.host==L"localhost"&&parsedProvider.port==8080&&!parsedProvider.secure&&parsedProvider.path==L"/v1/chat/completions?version=1","Custom provider URL routing");
        bool badUrl=false;try{ProviderEndpoint("https://user:password@example.com/path");}catch(...){badUrl=true;}Check(badUrl,"Provider URL credentials rejected");
        std::filesystem::remove(directory/L"provider-settings.json");
        {std::ofstream file(envFile);file<<"OPEN_ROUTER_KEY=\"bad\rkey\"\n";}
        bool rejected=false;try{LoadConfig(directory);}catch(...){rejected=true;}
        Check(rejected,"Header injection rejected");
        {std::ofstream file(envFile);file<<"OPEN_ROUTER_KEY=test-key\nOPENROUTER_MODEL=test/model\n";}
        std::vector<Message> history{{L"user",L"Quotes: \" \\ \nUnicode: \u65e5\U0001f680"},{L"assistant",L"Previous answer"},{L"error",L"Local error must be excluded"}};
        auto payload=nlohmann::json::parse(BuildPayload(config,history));
        Check(payload["messages"].size()==3,"Only system and valid history sent");
        Check(payload["stream"]==true,"Streaming requested from API");
        Check(payload["stream_options"]["include_usage"]==true,"Streaming token usage requested");
        Check(payload["tools"][0]["function"]["name"]=="powershell","PowerShell function schema advertised");
        {
            auto document=directory/L"context.txt",image=directory/L"context.png",pdf=directory/L"context.pdf";
            {std::ofstream file(document,std::ios::binary);file<<"\xef\xbb\xbf" "Document context \xe6\x97\xa5";}
            {std::ofstream file(image,std::ios::binary);file<<"\x89PNG\r\n\x1a\nfixture";}
            {std::ofstream file(pdf,std::ios::binary);file<<"%PDF-1.7 fixture";}
            Message attached{L"user",L"Use these attachments"};
            attached.attachments={LoadAttachment(document),LoadAttachment(image),LoadAttachment(pdf)};
            {std::ofstream file(document);file<<"changed after dropping";}
            auto content=nlohmann::json::parse(BuildPayload(config,{attached}))["messages"][1]["content"];
            Check(content.size()==4&&content[1]["text"].get<std::string>().find("Document context")!=std::string::npos,"Dropped document contents are snapshotted and sent as text");
            Check(content[2]["image_url"]["url"].get<std::string>().find("data:image/png;base64,iVBORw0KGg") == 0,"Images sent as base64 image parts");
            Check(content[3]["file"]["filename"]=="context.pdf"&&content[3]["file"]["file_data"].get<std::string>().find("data:application/pdf;base64,")==0,"PDF files sent as named base64 file parts");
            attached.text.clear();Check(nlohmann::json::parse(BuildPayload(config,{attached}))["messages"][1]["content"].size()==3,"Attachment-only messages supported");
            Config router=config;router.url="https://openrouter.ai/api/v1/chat/completions";
            auto routed=nlohmann::json::parse(BuildPayload(router,{attached}));
            Check(routed["plugins"][0]["pdf"]["engine"]=="cloudflare-ai","OpenRouter PDFs use text extraction for text-only models");
            router.url="https://example.com/v1";
            Check(!nlohmann::json::parse(BuildPayload(router,{attached})).contains("plugins"),"Provider-specific PDF plugins excluded from other endpoints");
            Message textDocument{L"user",L"Read this"};textDocument.attachments={attached.attachments[0]};
            auto plain=nlohmann::json::parse(BuildPayload(config,{textDocument}))["messages"][1]["content"];
            Check(plain.is_string()&&plain.get<std::string>().find("Read this\n\nAttached document: context.txt\nDocument context") == 0,"Text documents use ordinary string content for text-only providers");
            textDocument.text.clear();
            Check(nlohmann::json::parse(BuildPayload(config,{textDocument}))["messages"][1]["content"].get<std::string>().find("Attached document:")==0,"Document-only messages retain contents");
            Check(AttachmentBase64("f")=="Zg=="&&AttachmentBase64("fo")=="Zm8="&&AttachmentBase64("foo")=="Zm9v","Attachment base64 padding");
            {std::ofstream file(document,std::ios::binary);const unsigned char utf16[]{255,254,'H',0,'i',0};file.write(reinterpret_cast<const char*>(utf16),sizeof(utf16));}
            Check(LoadAttachment(document).data=="Hi","UTF-16 documents normalized to UTF-8");
            auto refuses=[&](const std::filesystem::path& path){try{LoadAttachment(path);return false;}catch(...){return true;}};
            {std::ofstream file(document,std::ios::binary);file.write("a\0b",3);}
            Check(refuses(document),"Binary text documents rejected");
            {std::ofstream file(document,std::ios::binary);file<<std::string(1024*1024+1,'x');}
            Check(refuses(document),"Oversized text documents rejected");
            Check(refuses(directory),"Folder drops rejected");
            auto unsupported=directory/L"context.docx";{std::ofstream file(unsupported);file<<"binary";}
            Check(refuses(unsupported),"Unsupported Office documents rejected explicitly");
            std::filesystem::remove(document);std::filesystem::remove(image);std::filesystem::remove(pdf);std::filesystem::remove(unsupported);
        }
        std::wstring decoded;
        StreamDecoder decoder([&](const std::wstring& delta){decoded+=delta;});
        auto sse=std::string("\xef\xbb\xbf: keepalive\r\n\r\n")+
            "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":null}}]}\r\n\r\n"+
            "data: {\"choices\":[{\"delta\":{\"content\":\""+Utf8(L"Hello \U0001f680")+"\"}}]}\r\n\r\n"+
            "data: {\"choices\":[],\"usage\":{}}\n\n"+
            "data: [DONE]\n\n";
        for(char byte:sse)decoder.Feed(std::string_view(&byte,1));
        Check(decoder.Finish().ok&&decoded==L"Hello \U0001f680","Bytewise SSE framing, comments, usage and UTF-8");
        Check(decoder.Finish().metrics.sentReports==0&&decoder.Finish().metrics.receivedReports==0,"Missing usage is not fabricated");
        StreamDecoder usageDecoder;
        usageDecoder.Feed("data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}\n\ndata: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3}}\n\ndata: [DONE]\n\n");
        auto usageReply=usageDecoder.Finish();Check(usageReply.metrics.sentTokens==12&&usageReply.metrics.receivedTokens==3&&usageReply.metrics.sentReports==1,"Usage snapshots are replaced instead of double counted");
        CallMetrics partialMetrics;partialMetrics.calls=2;partialMetrics.sentReports=1;partialMetrics.sentTokens=1234;
        partialMetrics.model=L"example/model";
        Check(CallActivity(partialMetrics,L"Streaming").find(L"Model: example/model")!=std::wstring::npos,"LLM details show the configured model");
        Check(CallActivity(partialMetrics,L"Complete").find(L"1,234 tokens (1/2 calls reported)")!=std::wstring::npos&&CallActivity(partialMetrics,L"Complete").find(L"Received: not reported")!=std::wstring::npos,"Partial and missing usage labelled accurately");
        StreamDecoder incomplete;incomplete.Feed("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n");
        Check(!incomplete.Finish().ok,"Truncated stream is not a successful reply");
        StreamDecoder multiline;multiline.Feed("data: {\"choices\":\rdata: [{\"delta\":{\"content\":\"ok\"}}]}\r\rdata: [DONE]\r\r");
        Check(multiline.Finish().ok,"Multiline SSE data and CR framing");
        Check(Wide(payload["messages"][1]["content"].get<std::string>())==history[0].text,"Unicode and escaping roundtrip");
        Check(ParseReply(200,R"({"choices":[{"message":{"content":"Hello \ud83d\ude80\nnext"}}]})").text==L"Hello \U0001f680\nnext","Response Unicode decoding");
        Check(!ParseReply(200,"invalid").ok,"Malformed JSON rejected");
        Check(!ParseReply(200,R"({"choices":[]})").ok,"Missing choice rejected");
        Check(!ParseReply(200,R"({"choices":[{"message":{"content":null}}]})").ok,"Null content rejected");
        Check(!ParseReply(200,R"({"choices":[{"message":{"content":"  "}}]})").ok,"Blank content rejected");
        Check(ParseReply(401,"secret server details").text.find(L"secret")==std::wstring::npos,"Error body never exposed");
        Endpoint endpoint;endpoint.host=L"127.0.0.1";endpoint.port=static_cast<unsigned short>(std::stoi(argv[1]));endpoint.secure=false;endpoint.timeoutMs=3000;
        {
            using Json=nlohmann::json;
            auto mcpDirectory=directory/L"mcp-test";std::filesystem::create_directories(mcpDirectory);
            Json remote={{"mcpServers",{{"remote",{{"url","http://127.0.0.1:"+std::string(argv[1])+"/mcp"},{"headers",{{"Authorization","Bearer mcp-token"}}}}}}}};
            SaveMcpConfig(mcpDirectory,remote);Check(LoadMcpConfig(mcpDirectory)==remote,"MCP encrypted settings round trip");
            {std::ifstream file(mcpDirectory/L"mcp-servers.json");std::string bytes((std::istreambuf_iterator<char>(file)),{});Check(bytes.find("mcp-token")==bytes.npos,"MCP credentials encrypted at rest");}
            Cancellation mcpCancel;std::vector<std::wstring> progress;
            McpClient remoteClient(remote,mcpDirectory,mcpCancel,[&](const std::wstring& text){progress.push_back(text);});
            auto definitions=remoteClient.Definitions();Check(definitions.size()==1,"HTTP MCP initialization session and SSE tool discovery");
            auto name=Json::parse(definitions[0])["function"]["name"].get<std::string>();
            Check(remoteClient.Call(name,{{"text","remote result"}})["content"][0]["text"]=="remote result","HTTP MCP tool result routed through SSE");
            auto multiple=remote;multiple["mcpServers"]["second"]=multiple["mcpServers"]["remote"];
            multiple["mcpServers"]["bad-key"]=multiple["mcpServers"]["remote"];multiple["mcpServers"]["bad-key"]["headers"]["Authorization"]="Bearer wrong";
            McpClient multipleClient(multiple,mcpDirectory,mcpCancel);
            auto multiTools=multipleClient.Definitions();
            Check(multiTools.size()==2&&Json::parse(multiTools[0])["function"]["name"]!=Json::parse(multiTools[1])["function"]["name"],"MCP namespacing avoids collisions and failed servers do not block healthy servers");
            auto agentEndpoint=endpoint;agentEndpoint.path=L"/agent-mcp";
            auto loop=RunAgent(config,{{L"user",L"Use the MCP echo tool"}},mcpDirectory,mcpCancel,agentEndpoint);
            Check(loop.reply.ok&&loop.reply.text==L"MCP completed","Agent discovers advertises invokes MCP tools and sends matching result IDs");
            Check(loop.reply.metrics.calls==2&&loop.reply.metrics.sentTokens==240&&loop.reply.metrics.receivedTokens==60&&loop.reply.metrics.sentReports==2&&loop.reply.metrics.receivedReports==2&&loop.reply.metrics.elapsedMs>=loop.reply.metrics.llmMs,"Agent sums provider token usage and measures total versus LLM time across tool rounds");
            std::wstring formattedTools;agentEndpoint.path=L"/agent-formats";
            auto formatted=RunAgent(config,{{L"user",L"Get structured output"}},mcpDirectory,mcpCancel,agentEndpoint,[&](AgentUpdate update){if(update.kind==UpdateKind::Tool)formattedTools+=update.text+L"\n";});
            Check(formatted.reply.ok&&formatted.reply.text==L"Formats complete"&&formattedTools.find(L"```json")!=formattedTools.npos&&formattedTools.find(L"```xml")!=formattedTools.npos&&formattedTools.find(L"\"kind\": \"mcp\"")!=formattedTools.npos,"PowerShell JSON MCP XML and structured MCP JSON are formatted without changing protocol tool results");
            Json local={{"mcpServers",{{"local",{{"command","node.exe"},{"args",Json::array({Utf8((std::filesystem::current_path()/L"tests"/L"mock_mcp.cjs").wstring())})},{"env",{{"MCP_FIXTURE_VALUE","env-ok"}}}}}}}};
            McpClient localClient(local,mcpDirectory,mcpCancel);
            definitions=localClient.Definitions();Check(definitions.size()==2,"Stdio MCP initialization and paginated discovery");
            std::string echo,slow;
            for(const auto& definition:definitions){auto tool=Json::parse(definition);auto toolAlias=tool["function"]["name"].get<std::string>();if(toolAlias.find("_echo_")!=toolAlias.npos)echo=toolAlias;else slow=toolAlias;}
            Check(localClient.Call(echo,{{"text","stdio result"}})["content"][0]["text"]=="stdio result env-ok","Stdio tools arguments environment notifications and ping");
            Check(localClient.Call(echo,Json::array()).contains("error"),"MCP rejects nonobject tool arguments");
            Cancellation callCancel;McpClient cancellable(local,mcpDirectory,callCancel);
            std::thread stopMcp([&]{std::this_thread::sleep_for(100ms);callCancel.Cancel();});
            auto cancelStart=std::chrono::steady_clock::now();auto cancelledCall=cancellable.Call(slow,Json::object());stopMcp.join();
            Check(cancelledCall.contains("error")&&std::chrono::steady_clock::now()-cancelStart<3s,"MCP cancellation terminates stdio process tree promptly");
            auto disabled=local;disabled["mcpServers"]["local"]["enabled"]=false;McpClient none(disabled,mcpDirectory,mcpCancel);Check(none.Definitions().empty(),"Disabled MCP servers are not launched");
            auto allDisabled=remote;allDisabled["enabled"]=false;McpClient globallyDisabled(allDisabled,mcpDirectory,mcpCancel);Check(globallyDisabled.Definitions().empty()&&allDisabled["mcpServers"].size()==1,"Global MCP disable retains saved servers without connecting");
            auto invalid=remote;invalid["mcpServers"]["remote"]["headers"]["Authorization"]="Bearer x\r\nInjected: value";
            bool refused=false;try{ValidateMcpConfig(invalid);}catch(...){refused=true;}Check(refused,"MCP HTTP header injection rejected");
            std::filesystem::remove(mcpDirectory/L"mcp-servers.json");
        }
        std::filesystem::remove(directory/L"mcp-test");
        {
            Config provider=config;provider.url="http://127.0.0.1:"+std::string(argv[1])+"/v1/chat/completions";
            Cancellation modelCancel;auto models=FetchModels(provider,modelCancel);
            Check(models==std::vector<std::string>({"a/model","z/model"}),"Authenticated provider model discovery sorted and deduplicated");
            auto local=provider;local.key.clear();local.url="http://127.0.0.1:"+std::string(argv[1])+"/keyless/";
            Check(FetchModels(local,modelCancel)==std::vector<std::string>({"local-model"}),"Keyless base URL discovery omits authorization");
            Check(Complete(local,history,modelCancel).ok,"Keyless base URL inference omits authorization");
            auto rejects=[&](Config candidate){try{FetchModels(candidate,modelCancel);return false;}catch(...){return true;}};
            auto invalid=provider;invalid.key="wrong-key";Check(rejects(invalid),"Model discovery rejects invalid key");
            invalid=provider;invalid.url="http://127.0.0.1:"+std::string(argv[1])+"/invalid/chat/completions";Check(rejects(invalid),"Malformed model list rejected");
            invalid.url="http://127.0.0.1:"+std::string(argv[1])+"/empty/chat/completions";Check(rejects(invalid),"Empty model list reported");
            Cancellation stopped;stopped.Cancel();bool stoppedModels=false;try{FetchModels(provider,stopped);}catch(...){stoppedModels=true;}Check(stoppedModels,"Model discovery cancellation");
        }
        {Config custom=config;custom.url="http://127.0.0.1:"+std::string(argv[1])+"/streaming";Cancellation providerCancel;Check(Complete(custom,history,providerCancel).ok,"Configured provider URL used for requests");}
        Cancellation cancel;
        endpoint.path=L"/success";
        auto reply=Complete(config,history,cancel,endpoint);
        Check(reply.ok&&reply.text==L"Native reply \U0001f680","HTTP success and UTF-8");
        endpoint.path=L"/streaming";
        std::vector<std::wstring> deltas;
        reply=Complete(config,history,cancel,endpoint,[&](const std::wstring& delta){deltas.push_back(delta);});
        Check(reply.ok&&deltas==std::vector<std::wstring>{L"Native ",L"reply \U0001f680"},"Incremental transport emits ordered deltas");
        endpoint.path=L"/wait-after-first";
        Cancellation duringStream;std::wstring firstToken;
        reply=Complete(config,history,duringStream,endpoint,[&](const std::wstring& delta){firstToken+=delta;duringStream.Cancel();});
        Check(reply.cancelled&&firstToken==L"First token","First token delivered before EOF, cancellation while streaming");
        for(auto path:{L"/interrupted",L"/streamerror"}) {
            endpoint.path=path;std::wstring partial;
            reply=Complete(config,history,cancel,endpoint,[&](const std::wstring& delta){partial+=delta;});
            Check(!reply.ok&&partial==L"Partial text","Partial output preserved on stream failure");
            Check(reply.text.find(L"private details")==std::wstring::npos,"Stream errors never expose provider bodies");
        }
        endpoint.path=L"/chunked";
        Check(Complete(config,history,cancel,endpoint).ok,"Chunked response reads");
        endpoint.path=L"/unauthorized";
        Check(Complete(config,history,cancel,endpoint).text.find(L"401")!=std::wstring::npos,"HTTP status error");
        {
            Message image{L"user",L"Describe"};image.attachments.push_back({L"photo.png","image/png","data:image/png;base64,fixture",7});
            endpoint.path=L"/attachment-rejected";
            auto imageFailure=Complete(config,{image},cancel,endpoint);
            Check(!imageFailure.ok&&imageFailure.text.find(L"accepts images")!=std::wstring::npos&&imageFailure.text.find(Wide(config.model))!=std::wstring::npos,"Rejected image requests explain model compatibility");
            Check(imageFailure.text.find(L"private details")==std::wstring::npos,"Attachment errors never expose provider bodies");
            endpoint.path=L"/attachment-too-large";
            Check(Complete(config,{image},cancel,endpoint).text.find(L"Reduce attachment sizes")!=std::wstring::npos,"Oversized attachment requests explain size recovery");
        }
        for(auto path:{L"/malformed",L"/empty",L"/apierror",L"/oversized",L"/redirect"}) {
            endpoint.path=path;Check(!Complete(config,history,cancel,endpoint).ok,"Invalid HTTP response rejected");
        }
        endpoint.path=L"/slow";endpoint.timeoutMs=100;
        auto start=std::chrono::steady_clock::now();
        Check(Complete(config,history,cancel,endpoint).text.find(L"timed out")!=std::wstring::npos,"Overall timeout");
        Check(std::chrono::steady_clock::now()-start<2s,"Timeout releases pending HTTP callback");
        Cancellation cancelled;endpoint.timeoutMs=3000;
        std::thread canceller([&]{Sleep(100);cancelled.Cancel();});
        start=std::chrono::steady_clock::now();
        auto aborted=Complete(config,history,cancelled,endpoint);canceller.join();
        Check(aborted.cancelled&&std::chrono::steady_clock::now()-start<2s,"In-flight request cancellation");
        Check(Complete(config,history,cancelled,endpoint).cancelled,"Pre-cancelled request");
        std::mutex mutex;std::condition_variable ready;std::vector<Result> results;
        endpoint.path=L"/worker";
        ChatWorker worker(directory,[&](Result r){std::lock_guard<std::mutex> lock(mutex);results.push_back(std::move(r));ready.notify_one();},endpoint);
        worker.Start({{L"user",L"slow"}},1);Sleep(100);worker.Cancel();
        {std::lock_guard<std::mutex> lock(mutex);results.clear();}
        worker.Start({{L"user",L"fast"}},2);
        {std::unique_lock<std::mutex> lock(mutex);Check(ready.wait_for(lock,3s,[&]{return !results.empty()&&results.back().final;}),"Replacement request completes");Check(results.size()>=2&&results.front().generation==2&&!results.front().final&&results.back().generation==2&&results.back().reply.ok,"Worker delivers deltas then final reply");for(auto& result:results)Check(result.generation==2,"Stale reply discarded");}
        worker.Start({{L"user",L"slow"}},3);Sleep(100);start=std::chrono::steady_clock::now();worker.Stop();
        Check(std::chrono::steady_clock::now()-start<2s,"Shutdown cancels and joins worker");
        SetEnvironmentVariableW(L"OPEN_ROUTER_KEY",L"must-not-reach-child");
        auto tool=RunPowerShell(L"if($env:OPEN_ROUTER_KEY -or $env:OPENROUTER_API_KEY){throw 'credential leak'}; Write-Output 'tool-ok'; Write-Output '\u65e5\U0001f680'",directory,cancel,10);
        SetEnvironmentVariableW(L"OPEN_ROUTER_KEY",nullptr);
        Check(tool.exitCode==0&&tool.output.find(L"tool-ok")!=tool.output.npos&&tool.output.find(L"\u65e5\U0001f680")!=tool.output.npos,"PowerShell UTF-8 output, quoting and credential isolation");
        tool=RunPowerShell(L"Write-Error 'expected tool failure'",directory,cancel,10);
        Check(tool.exitCode!=0&&tool.output.find(L"expected tool failure")!=tool.output.npos,"PowerShell errors returned with exit code");
        tool=RunPowerShell(L"Write-Output ('x'*100000)",directory,cancel,10);
        Check(tool.exitCode==0&&tool.truncated&&tool.output.size()<=65536,"Tool output bounded and drained without deadlock");
        start=std::chrono::steady_clock::now();tool=RunPowerShell(L"Start-Sleep -Seconds 10",directory,cancel,1);
        Check(tool.timedOut&&std::chrono::steady_clock::now()-start<4s,"PowerShell timeout terminates process job");
        Cancellation cancelTool;std::thread killTool([&]{Sleep(250);cancelTool.Cancel();});
        tool=RunPowerShell(L"Start-Sleep -Seconds 10",directory,cancelTool,10);killTool.join();
        Check(tool.cancelled,"PowerShell cancellation terminates process job");
        endpoint.path=L"/agent";endpoint.timeoutMs=10000;
        std::vector<AgentUpdate> updates;
        auto agent=RunAgent(config,{{L"user",L"Run the test tools"}},directory,cancel,endpoint,[&](AgentUpdate update){updates.push_back(std::move(update));});
        Check(agent.reply.ok&&agent.reply.text==L"Agent finished","Agent executes tools and continues to a final streamed answer");
        Check(agent.history.size()==5&&agent.history[1].toolCalls.size()==2&&agent.history[2].toolCallId=="call_one"&&agent.history[3].toolCallId=="call_two","Tool call IDs and ordered history preserved");
        Check(std::filesystem::exists(directory/L"agent-output.txt"),"PowerShell executed in the selected project directory");
        bool sawStatus=false,sawTool=false,sawDelta=false;for(auto& update:updates){sawStatus|=update.kind==UpdateKind::Status;sawTool|=update.kind==UpdateKind::Tool;sawDelta|=update.kind==UpdateKind::Delta;}
        Check(sawStatus&&sawTool&&sawDelta,"Agent exposes progress, commands, output and response deltas");
        for(auto path:{L"/badtool",L"/badargs"}){endpoint.path=path;agent=RunAgent(config,{{L"user",L"Recover"}},directory,cancel,endpoint);Check(agent.reply.ok&&agent.reply.text==L"Recovered","Tool validation failures returned to model for recovery");}
        endpoint.path=L"/loop";agent=RunAgent(config,{{L"user",L"Loop"}},directory,cancel,endpoint);
        Check(!agent.reply.ok&&agent.history.size()==17&&agent.reply.text.find(L"step limit")!=agent.reply.text.npos,"Agent loop stops after eight model turns");
        std::filesystem::remove(directory/L"agent-output.txt");
        std::filesystem::remove(envFile);std::filesystem::remove(directory);
        std::cout<<"All configuration, streaming, PowerShell, agent-loop, cancellation, and worker tests passed.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}

