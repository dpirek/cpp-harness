#define NOMINMAX
#include "tools/computer_tool.h"
#include <gdiplus.h>
#include <objidl.h>
#include <algorithm>
#include <cmath>
#include <map>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")

namespace chat {
using Json=nlohmann::json;
std::string ComputerDefinition() {
    Json properties={{"action",{{"type","string"},{"enum",{"screenshot","move","click","double_click","drag","scroll","type","key","wait"}}}},
        {"x",{{"type","integer"}}},{"y",{{"type","integer"}}},{"to_x",{{"type","integer"}}},{"to_y",{{"type","integer"}}},
        {"button",{{"type","string"},{"enum",{"left","right","middle"}}}},
        {"text",{{"type","string"},{"maxLength",4096}}},{"keys",{{"type","array"},{"items",{{"type","string"}}},{"minItems",1},{"maxItems",8}}},
        {"delta",{{"type","integer"},{"minimum",-20},{"maximum",20}}},{"milliseconds",{{"type","integer"},{"minimum",0},{"maximum",5000}}}};
    Json definition;
    definition["type"]="function";
    auto& function=definition["function"];
    function["name"]="computer";
    function["description"]="Control the Windows desktop. Screenshot returns a PNG and physical desktop bounds; image (0,0) maps to (left,top). Move/click/double_click/drag require x,y; drag also requires to_x,to_y. Scroll requires delta in notches (positive up). Type requires Unicode text. Key requires a chord such as [CTRL,L] or [ENTER]; supports letters, digits, F1-F24, CTRL, ALT, SHIFT, WIN, TAB, ENTER, ESC, SPACE, BACKSPACE, DELETE, LEFT, RIGHT, UP, DOWN, HOME, END, PAGEUP, PAGEDOWN. Wait accepts milliseconds (default 300). Pointer actions move smoothly from the current cursor position and pause for hover, button presses, and desktop updates. Success means input was delivered, not that the target application completed its operation. Inspect screenshots before acting and after changes.";
    function["parameters"]={{"type","object"},{"properties",properties},{"required",Json::array({"action"})},{"additionalProperties",false}};
    return definition.dump();
}
namespace {
void Require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
void Send(INPUT input) { Require(SendInput(1,&input,sizeof(input))==1,"Windows rejected input (the target may be elevated or on a secure desktop)."); }
void Mouse(DWORD flags,DWORD data=0){INPUT i{};i.type=INPUT_MOUSE;i.mi.dwFlags=flags;i.mi.mouseData=data;Send(i);}
void Key(WORD vk,bool up,bool unicode=false){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=unicode?0:vk;i.ki.wScan=unicode?vk:0;i.ki.dwFlags=(up?KEYEVENTF_KEYUP:0)|(unicode?KEYEVENTF_UNICODE:0);if(!unicode&&(vk==VK_LWIN||vk==VK_DELETE||(vk>=VK_PRIOR&&vk<=VK_DOWN)))i.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY;Send(i);}
WORD KeyCode(std::string name) {
    for(auto& c:name)c=static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if(name.size()==1&&((name[0]>='A'&&name[0]<='Z')||(name[0]>='0'&&name[0]<='9')))return name[0];
    static const std::map<std::string,int> codes={{"CTRL",VK_CONTROL},{"ALT",VK_MENU},{"SHIFT",VK_SHIFT},{"WIN",VK_LWIN},{"TAB",VK_TAB},{"ENTER",VK_RETURN},{"ESC",VK_ESCAPE},{"SPACE",VK_SPACE},{"BACKSPACE",VK_BACK},{"DELETE",VK_DELETE},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},{"UP",VK_UP},{"DOWN",VK_DOWN},{"HOME",VK_HOME},{"END",VK_END},{"PAGEUP",VK_PRIOR},{"PAGEDOWN",VK_NEXT}};
    auto it=codes.find(name);if(it!=codes.end())return static_cast<WORD>(it->second);
    for(int n=1;n<=24;++n)if(name=="F"+std::to_string(n))return static_cast<WORD>(VK_F1+n-1);
    throw std::runtime_error("Unsupported key name.");
}
Attachment Capture(int x,int y,int w,int h) {
    Require(w>0&&h>0&&static_cast<long long>(w)*h<=64000000,"Desktop dimensions exceed the screenshot limit.");
    ULONG_PTR token=0;Gdiplus::GdiplusStartupInput startup;
    Require(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"Could not initialize screenshot encoder.");
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,w,h);
    HGDIOBJ old=bitmap&&memory?SelectObject(memory,bitmap):nullptr;
    bool copied=screen&&memory&&bitmap&&BitBlt(memory,0,0,w,h,screen,x,y,SRCCOPY|CAPTUREBLT);
    if(old)SelectObject(memory,old);
    Attachment result;
    try {
        Require(copied,"Could not capture the desktop.");
        Gdiplus::Bitmap image(bitmap,nullptr);
        CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
        IStream* stream=nullptr;Require(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)),"Could not allocate screenshot stream.");
        auto status=image.Save(stream,&png,nullptr);
        HGLOBAL buffer=nullptr;GetHGlobalFromStream(stream,&buffer);STATSTG stat{};auto statResult=stream->Stat(&stat,STATFLAG_NONAME);
        if(status!=Gdiplus::Ok||FAILED(statResult)||stat.cbSize.QuadPart>10*1024*1024){stream->Release();throw std::runtime_error("Screenshot encoding failed or exceeds 10 MiB.");}
        auto data=static_cast<const char*>(GlobalLock(buffer));
        if(!data){stream->Release();throw std::runtime_error("Could not read screenshot bytes.");}
        result.name=L"Desktop screenshot";result.mime="image/png";result.bytes=static_cast<size_t>(stat.cbSize.QuadPart);
        result.data="data:image/png;base64,"+AttachmentBase64(std::string(data,result.bytes));
        GlobalUnlock(buffer);stream->Release();
    }catch(...){if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(screen)ReleaseDC(nullptr,screen);Gdiplus::GdiplusShutdown(token);throw;}
    DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);Gdiplus::GdiplusShutdown(token);return result;
}
}
ComputerResult RunComputer(const Json& args,const Cancellation& cancel,bool smoothPointer) {
    ComputerResult result;
    auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        Require(!cancel.IsCancelled(),"Computer action cancelled.");
        Require(args.is_object()&&args.contains("action")&&args["action"].is_string(),"action must be a string.");
        auto definition=Json::parse(ComputerDefinition())["function"]["parameters"]["properties"];
        for(auto& item:args.items()) {
            Require(definition.contains(item.key()),"Unknown computer argument.");
            auto type=definition[item.key()]["type"];
            Require((type=="integer"&&item.value().is_number_integer())||(type=="string"&&item.value().is_string())||(type=="array"&&item.value().is_array()),"Invalid computer argument type.");
        }
        int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN),w=GetSystemMetrics(SM_CXVIRTUALSCREEN),h=GetSystemMetrics(SM_CYVIRTUALSCREEN);
        auto point=[&](const char* a,const char* b){Require(args.contains(a)&&args.contains(b),"Coordinates are required.");int px=args.at(a).get<int>(),py=args.at(b).get<int>();Require(px>=x&&py>=y&&px<x+w&&py<y+h,"Coordinates are outside the desktop.");return POINT{px,py};};
        auto pause=[&](DWORD ms){Require(WaitForSingleObject(cancel.Event(),ms)==WAIT_TIMEOUT,"Computer action cancelled.");};
        auto moveTo=[&](POINT p){
            INPUT input{};input.type=INPUT_MOUSE;
            input.mi.dx=static_cast<LONG>((static_cast<long long>(p.x-x)*65536+32768)/w);
            input.mi.dy=static_cast<LONG>((static_cast<long long>(p.y-y)*65536+32768)/h);
            input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;Send(input);
        };
        auto move=[&](POINT target){
            POINT origin{};Require(GetCursorPos(&origin)!=0,"Could not read the pointer position.");
            double dx=static_cast<double>(target.x-origin.x),dy=static_cast<double>(target.y-origin.y);
            double distance=std::hypot(dx,dy);
            if(distance>0&&smoothPointer) {
                int duration=std::clamp(static_cast<int>(distance/2),180,800),steps=(duration+15)/16;
                auto began=GetTickCount64();
                for(int step=1;step<=steps;++step) {
                    Require(!cancel.IsCancelled(),"Computer action cancelled.");
                    double t=static_cast<double>(step)/steps,eased=t*t*(3-2*t);
                    moveTo({origin.x+static_cast<LONG>(std::lround(dx*eased)),origin.y+static_cast<LONG>(std::lround(dy*eased))});
                    auto due=began+static_cast<ULONGLONG>(duration)*step/steps,now=GetTickCount64();
                    if(due>now)pause(static_cast<DWORD>(due-now));
                }
            }
            if(distance>0&&!smoothPointer)moveTo(target);
            // Confirm queued movement was processed before sending a button press.
            POINT actual{};bool arrived=false;
            for(int attempt=0;attempt<10;++attempt) {
                Require(!cancel.IsCancelled(),"Computer action cancelled.");
                if(GetCursorPos(&actual)&&actual.x==target.x&&actual.y==target.y){arrived=true;break;}
                pause(10);
            }
            Require(arrived,"Pointer did not reach the requested position. Another application may be confining or moving it.");
        };
        auto action=args["action"].get<std::string>();
        if(action=="screenshot") {result.screenshot=Capture(x,y,w,h);result.output={{"left",x},{"top",y},{"width",w},{"height",h},{"coordinates","physical desktop pixels"}};}
        else if(action=="wait") {int ms=args.value("milliseconds",300);Require(ms>=0&&ms<=5000,"milliseconds must be 0 to 5000.");WaitForSingleObject(cancel.Event(),ms);}
        else if(action=="type") {
            Require(args.contains("text"),"text is required.");auto text=Wide(args.at("text").get<std::string>());Require(text.size()<=4096,"text exceeds 4096 UTF-16 units.");
            for(auto c:text){
                if(cancel.IsCancelled())break;
                INPUT pair[2]{};
                for(auto& event:pair){event.type=INPUT_KEYBOARD;event.ki.wScan=c;event.ki.dwFlags=KEYEVENTF_UNICODE;}
                pair[1].ki.dwFlags|=KEYEVENTF_KEYUP;
                auto sent=SendInput(2,pair,sizeof(INPUT));
                if(sent==1)try{Key(c,true,true);}catch(...){}
                Require(sent==2,"Windows rejected text input. Keep the desktop unlocked and use a target running at the same privilege level as Chat.");
            }
        } else if(action=="key") {
            Require(args.contains("keys")&&args["keys"].size()>0&&args["keys"].size()<=8,"Provide 1 to 8 keys.");std::vector<WORD> codes;
            for(auto& name:args["keys"]){Require(name.is_string(),"Key names must be strings.");codes.push_back(KeyCode(name.get<std::string>()));}
            std::vector<INPUT> events;
            auto append=[&](WORD code,bool up){
                INPUT event{};event.type=INPUT_KEYBOARD;event.ki.wVk=code;
                event.ki.wScan=static_cast<WORD>(MapVirtualKeyW(code,MAPVK_VK_TO_VSC));
                event.ki.dwFlags=up?KEYEVENTF_KEYUP:0;
                if(code==VK_LWIN||code==VK_DELETE||(code>=VK_PRIOR&&code<=VK_DOWN))event.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY;
                events.push_back(event);
            };
            for(auto code:codes)append(code,false);
            for(auto it=codes.rbegin();it!=codes.rend();++it)append(*it,true);
            auto sent=SendInput(static_cast<UINT>(events.size()),events.data(),sizeof(INPUT));
            if(sent!=events.size()) {
                for(auto it=codes.rbegin();it!=codes.rend();++it)try{Key(*it,true);}catch(...){}
                throw std::runtime_error("Windows rejected the key chord. The target may be elevated or on a secure desktop.");
            }
        } else if(action=="scroll") {Require(args.contains("delta"),"delta is required.");int delta=args.at("delta").get<int>();Require(delta>=-20&&delta<=20,"delta must be -20 to 20.");Mouse(MOUSEEVENTF_WHEEL,static_cast<DWORD>(delta*WHEEL_DELTA));}
        else if(action=="move"||action=="click"||action=="double_click"||action=="drag") {
            POINT start=point("x","y"),end=start;if(action=="drag")end=point("to_x","to_y");
            auto button=args.value("button",std::string("left"));Require(button=="left"||button=="right"||button=="middle","Invalid mouse button.");
            DWORD down=button=="left"?MOUSEEVENTF_LEFTDOWN:button=="right"?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_MIDDLEDOWN;
            DWORD up=button=="left"?MOUSEEVENTF_LEFTUP:button=="right"?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_MIDDLEUP;
            move(start);
            pause(action=="move"?(smoothPointer?80:0):160); // Allow hover/activation handlers to run before pressing.
            if(action!="move")for(int click=0;click<(action=="double_click"?2:1);++click) {
                Require(!cancel.IsCancelled(),"Computer action cancelled.");Mouse(down);
                try {
                    pause(std::min<DWORD>(60,std::max<DWORD>(1,GetDoubleClickTime()/4)));
                    if(action=="drag"){move(end);pause(100);}
                }catch(...){try{Mouse(up);}catch(...){}throw;}
                Mouse(up);
                if(action=="double_click"&&click==0)pause(std::min<DWORD>(100,std::max<DWORD>(1,GetDoubleClickTime()/4)));
            }
            if(action!="move")pause(350); // Let the target process the release and resulting UI changes.
        } else throw std::runtime_error("Unknown computer action.");
        if(action=="type"||action=="key"||action=="scroll")pause(action=="scroll"?250:150);
        if(result.output.is_null())result.output={{"ok",!cancel.IsCancelled()},{"action",action}};
    }catch(const std::exception& error){result.output={{"error",error.what()}};}
    if(previous)SetThreadDpiAwarenessContext(previous);
    return result;
}
}
