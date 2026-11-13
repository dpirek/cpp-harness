#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(lib,"uxtheme.lib")
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <future>
#include <map>
#include <shellapi.h>
#include "ui/markdown.h"
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"comctl32.lib")
#include "app/chat_worker.h"
#include "ui/code_format.h"
#include "common/text_encoding.h"
#include "tools/mcp_client.h"
#include "tools/computer_tool.h"
#include "config/chat_history.h"
#include "config/window_state.h"

namespace {
constexpr COLORREF Background=RGB(24,24,24), Panel=RGB(31,31,31), Surface=RGB(36,36,36);
constexpr COLORREF ModalHeader=RGB(44,44,44), ModalBody=RGB(40,40,40);
constexpr COLORREF Ink=RGB(240,240,240), Muted=RGB(148,148,148), Border=RGB(48,48,48);
constexpr int Header=48, Space=24, MaxPromptLines=6;
HWND conversationScroll,historyList,workspaceButton,historyButton;
HWND app,input,newButton,settingsButton,sidebarToggle,sendButton,clearAttachmentsButton,navTooltips,minButton,maxButton,closeButton;
HFONT bodyFont,labelFont,headingFont,smallFont,codeFont,navIconFont,captionIconFont;
HANDLE iconFontResource=nullptr;
std::map<std::pair<int,unsigned>,HFONT> markdownFonts;
struct ResponseLink {RECT rect;std::wstring href;};
std::vector<ResponseLink> responseLinks;
using TextPosition=std::pair<size_t,size_t>;
struct TextGlyph {RECT rect;std::wstring text;TextPosition position;};
std::vector<TextGlyph> textGlyphs;
TextPosition textAnchor{},textEnd{};
size_t textMessage=0,textOffset=0;
bool captureText=false,dragText=false;
HBRUSH surfaceBrush,composerBackdropBrush;
WNDPROC oldInputProc,oldButtonProc;
UINT dpi=96;
bool busy=false;
bool sidebarExpanded=true,sidebarWide=true;
std::optional<size_t> streamingMessage;
std::optional<size_t> currentPrompt;
std::optional<size_t> recalledPrompt;
std::wstring promptRecallDraft;
DWORD promptDraftSelection=0;
bool recallingPrompt=false;
int scroll=0,sideWidth=260;
int conversationExtent=0,conversationPage=1;
bool scrollDragging=false,scrollHovered=false;
int scrollDragOffset=0,scrollRepeatDirection=0;
unsigned generation=0;
std::filesystem::path configDirectory;
std::vector<chat::Message> messages;
std::vector<chat::Message> apiHistory;
std::vector<chat::Attachment> pendingAttachments;
std::unique_ptr<chat::ChatWorker> worker;
chat::ChatHistory savedHistory;
int hoveredChat=-1;
std::string pressedDeleteChat;
bool historyWritable=true,restoringChat=false;
std::wstring historyError;
void StopAgent();
int ShowModalMessage(HWND,const wchar_t*,const wchar_t*,UINT);
struct LayoutRects { RECT client{},sidebar{},column{},conversation{},composer{}; } layout;
int Px(int value) { return MulDiv(value,static_cast<int>(dpi),96); }
void Fill(HDC dc,RECT rect,COLORREF color) { auto brush=CreateSolidBrush(color);FillRect(dc,&rect,brush);DeleteObject(brush); }
void Line(HDC dc,int x,int y,int endX,int endY,COLORREF color=Border,int width=1) {
    auto pen=CreatePen(PS_SOLID,width,color);auto previous=SelectObject(dc,pen);
    MoveToEx(dc,x,y,nullptr);LineTo(dc,endX,endY);SelectObject(dc,previous);DeleteObject(pen);
}
bool HasTextSelection(){return textAnchor!=textEnd;}
void RecordGlyphs(HDC dc,const std::wstring& value,int x,int y) {
    if(!captureText)return;
    TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);int previous=0;
    for(size_t i=0;i<value.size();) {
        size_t count=(value[i]>=0xd800&&value[i]<=0xdbff&&i+1<value.size())?2:1;
        SIZE size{};GetTextExtentPoint32W(dc,value.data(),static_cast<int>(i+count),&size);
        textGlyphs.push_back({{x+previous,y,x+size.cx,y+tm.tmHeight},value.substr(i,count),{textMessage,textOffset++}});
        previous=size.cx;i+=count;
    }
}
TextPosition HitChatText(POINT point) {
    long long best=0x7fffffffffffffffLL;TextPosition result{};
    for(const auto& glyph:textGlyphs) {
        const auto& r=glyph.rect;
        if(r.bottom<layout.conversation.top||r.top>=layout.conversation.bottom)continue;
        long long dy=point.y<r.top?r.top-point.y:point.y>=r.bottom?point.y-r.bottom+1:0;
        long long dx=point.x<r.left?r.left-point.x:point.x>r.right?point.x-r.right:0;
        auto distance=dy*dy*1000000+dx*dx;
        if(distance<best){best=distance;result=glyph.position;if(point.x>(r.left+r.right)/2)++result.second;}
    }
    return result;
}
void CopyChatSelection() {
    std::wstring value;const TextGlyph* previous=nullptr;
    auto first=std::min(textAnchor,textEnd),last=std::max(textAnchor,textEnd);
    for(const auto& glyph:textGlyphs)if(glyph.position>=first&&glyph.position<last) {
        if(previous) {
            if(previous->position.first!=glyph.position.first)value+=L"\r\n\r\n";
            else if(glyph.rect.top>=previous->rect.bottom)value+=L"\r\n";
        }
        value+=glyph.text;previous=&glyph;
    }
    if(value.empty()||!OpenClipboard(app))return;
    auto memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
    if(memory) {
        auto data=GlobalLock(memory);
        if(data){memcpy(data,value.c_str(),(value.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}
        else GlobalFree(memory);
    }
    CloseClipboard();
}
void Text(HDC dc,const std::wstring& value,RECT rect,HFONT font,COLORREF color=Ink,UINT flags=DT_LEFT|DT_WORDBREAK|DT_NOPREFIX) {
    SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,value.c_str(),-1,&rect,flags);
    if(captureText) {
        TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);int y=rect.top;size_t start=0;
        while(start<value.size()) {
            auto end=value.find_first_of(L"\r\n",start);if(end==value.npos)end=value.size();
            int fit=0;SIZE size{};GetTextExtentExPointW(dc,value.data()+start,static_cast<int>(end-start),std::max(1L,rect.right-rect.left),&fit,nullptr,&size);
            size_t take=(flags&DT_SINGLELINE)?end-start:std::min(end-start,static_cast<size_t>(std::max(1,fit)));
            if(start+take<end){auto space=value.find_last_of(L" \t",start+take);if(space!=value.npos&&space>start)take=space-start+1;}
            RecordGlyphs(dc,value.substr(start,take),rect.left,y);y+=tm.tmHeight;start+=take;
            if(start==end&&end<value.size()){++start;if(value[end]==L'\r'&&start<value.size()&&value[start]==L'\n')++start;}
        }
    }
}
void Round(HDC dc,RECT rect,COLORREF color,int radius) {
    auto brush=CreateSolidBrush(color);auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,radius,radius);
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
}
void CreateFonts() {
    for(const auto& font:markdownFonts)DeleteObject(font.second);markdownFonts.clear();
    for(auto font:{bodyFont,labelFont,headingFont,smallFont,codeFont,navIconFont,captionIconFont})if(font)DeleteObject(font);
    auto make=[](int size,int weight){return CreateFontW(-Px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");};
    bodyFont=make(16,FW_NORMAL);labelFont=make(14,FW_SEMIBOLD);headingFont=make(28,FW_SEMIBOLD);smallFont=make(13,FW_NORMAL);
    codeFont=CreateFontW(-Px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    auto icon=[](int size){return CreateFontW(-Px(size),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"bootstrap-icons");};
    navIconFont=icon(18);captionIconFont=icon(14);
    if(input)SendMessageW(input,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);
}
int PromptLines(HDC dc,int width) {
    int length=GetWindowTextLengthW(input);std::wstring value(length+1,L'\0');GetWindowTextW(input,value.data(),length+1);value.resize(length);
    int lines=0;size_t paragraph=0;
    do {
        auto end=value.find_first_of(L"\r\n",paragraph);if(end==value.npos)end=value.size();
        size_t position=paragraph;
        if(position==end)++lines;
        while(position<end&&lines<MaxPromptLines) {
            int fit=0;SIZE extent{};
            GetTextExtentExPointW(dc,value.data()+position,static_cast<int>(end-position),width,&fit,nullptr,&extent);
            ++lines;if(fit>=static_cast<int>(end-position))break;
            size_t take=static_cast<size_t>(std::max(1,fit));
            auto space=value.find_last_of(L" \t",position+take-1);
            if(space!=value.npos&&space>=position)take=space-position+1;
            position+=take;
            while(position<end&&(value[position]==L' '||value[position]==L'\t'))++position;
        }
        if(end==value.size()||lines>=MaxPromptLines)break;
        paragraph=end+1;if(value[end]==L'\r'&&paragraph<value.size()&&value[paragraph]==L'\n')++paragraph;
    }while(paragraph<=value.size());
    return std::max(1,lines);
}
void Layout(bool promptOnly=false) {
    auto previousComposer=layout.composer;auto previousColumn=layout.column;
    GetClientRect(app,&layout.client);
    int width=layout.client.right,height=layout.client.bottom;
    if(width<=0||height<=0)return;
    sidebarWide=sidebarExpanded&&width>=Px(720);
    sideWidth=sidebarWide?Px(width<Px(1000)?228:260):Px(72);
    layout.sidebar={0,Px(Header),sideWidth,height};
    int available=width-sideWidth-2*Px(Space);
    int columnWidth=std::min(Px(880),available);
    int left=sideWidth+(width-sideWidth-columnWidth)/2;
    // Measure the text independently of the edit's current one-line viewport.
    int inputWidth=columnWidth-Px(88);
    RECT previousInput{};GetClientRect(input,&previousInput);
    HDC dc=GetDC(input);auto oldFont=SelectObject(dc,bodyFont);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);
    int measuredLines=PromptLines(dc,inputWidth);SelectObject(dc,oldFont);ReleaseDC(input,dc);
    int lineHeight=std::max(1L,metrics.tmHeight);
    int maxLines=std::clamp((height-Px(Header+Space*4+80))/lineHeight,1,MaxPromptLines);
    int lines=std::clamp(measuredLines,1,maxLines);
    int inputHeight=lines*lineHeight;
    int attachmentHeight=pendingAttachments.empty()?0:Px(40);
    int bodyHeight=std::max(inputHeight+Px(32),Px(64));
    int composerHeight=bodyHeight+attachmentHeight;
    layout.column={left,Px(Header),left+columnWidth,height-Px(Space)};
    layout.composer={left,height-Px(Space)-composerHeight,left+columnWidth,height-Px(Space)};
    layout.conversation={left,Px(Header),left+columnWidth,height};
    if(promptOnly&&EqualRect(&previousComposer,&layout.composer)&&EqualRect(&previousColumn,&layout.column))return;
    if(!promptOnly) {
    // A 40 DIP icon button has 16 DIP clearance around its hover rectangle.
    int iconLeft=sidebarWide?sideWidth-Px(56):(sideWidth-Px(40))/2;
    MoveWindow(historyButton,0,Px(Header+72+56),Px(72),Px(56),TRUE);ShowWindow(historyButton,sidebarWide?SW_HIDE:SW_SHOW);
    MoveWindow(historyList,Px(12),Px(Header+72+56+60),std::max(1,sideWidth-Px(24)),std::max(1,height-Px(Header+72+56+60+120)),TRUE);
    ShowWindow(historyList,sidebarWide?SW_SHOW:SW_HIDE);
    MoveWindow(workspaceButton,0,sidebarWide?height-Px(120):Px(Header+72+56*2),sideWidth,Px(56),TRUE);
    ShowWindow(workspaceButton,SW_SHOW);
    MoveWindow(newButton,0,Px(Header+72),sideWidth,Px(56),TRUE);
    MoveWindow(settingsButton,iconLeft,height-Px(56),Px(40),Px(40),TRUE);
    MoveWindow(sidebarToggle,iconLeft,Px(Header+16),Px(40),Px(40),TRUE);
    SetWindowTextW(sidebarToggle,sidebarWide?L"Collapse sidebar":L"Expand sidebar");
    }
    MoveWindow(conversationScroll,layout.client.right-Px(18),layout.conversation.top,Px(16),std::max(1L,layout.conversation.bottom-layout.conversation.top),TRUE);
    int inputTop=layout.composer.top+attachmentHeight+(bodyHeight-inputHeight)/2;
    SetWindowPos(input,nullptr,left+Px(24),inputTop,inputWidth,inputHeight,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
    MoveWindow(clearAttachmentsButton,layout.composer.right-Px(48),layout.composer.top+Px(8),Px(32),Px(32),TRUE);
    ShowWindow(clearAttachmentsButton,pendingAttachments.empty()?SW_HIDE:SW_SHOW);
    MoveWindow(sendButton,layout.composer.right-Px(48),layout.composer.bottom-Px(48),Px(32),Px(32),TRUE);
    if(previousInput.right!=inputWidth||previousInput.bottom!=inputHeight) {
        RECT formatting{0,0,inputWidth,inputHeight};SendMessageW(input,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&formatting));
        SendMessageW(input,EM_SCROLLCARET,0,0);
    }
    RedrawWindow(input,nullptr,nullptr,RDW_INVALIDATE|RDW_NOERASE);
    if(!promptOnly) {
    int size=Px(Header);
    auto positions=BeginDeferWindowPos(3);
    for(auto control:{minButton,maxButton,closeButton}) {
        int offset=control==minButton?3:control==maxButton?2:1;
        if(positions)positions=DeferWindowPos(positions,control,nullptr,width-size*offset,0,size,size,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
    }
    if(positions)EndDeferWindowPos(positions);
    else for(auto control:{minButton,maxButton,closeButton}){int offset=control==minButton?3:control==maxButton?2:1;SetWindowPos(control,nullptr,width-size*offset,0,size,size,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);}
    const wchar_t* caption=IsZoomed(app)?L"Restore":L"Maximize";wchar_t previousCaption[32]{};GetWindowTextW(maxButton,previousCaption,32);
    if(std::wstring(previousCaption)!=caption)SetWindowTextW(maxButton,caption);
    for(auto control:{minButton,maxButton,closeButton})RedrawWindow(control,nullptr,nullptr,RDW_INVALIDATE|RDW_NOERASE|RDW_UPDATENOW);
    }
    InvalidateRect(app,nullptr,FALSE);
}
void RefreshChats() {
    if(!historyList)return;
    hoveredChat=-1;pressedDeleteChat.clear();
    SendMessageW(historyList,LB_RESETCONTENT,0,0);
    for(size_t i=0;i<savedHistory.chats.size();++i) {
        SendMessageW(historyList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(savedHistory.chats[i].title.c_str()));
        if(savedHistory.chats[i].id==savedHistory.activeId)SendMessageW(historyList,LB_SETCURSEL,i,0);
    }
    InvalidateRect(historyList,nullptr,TRUE);
}
void PersistChat() {
    try {
        if(savedHistory.activeId.empty()) {
            auto item=chat::NewSavedChat(configDirectory,savedHistory);savedHistory.activeId=item.id;savedHistory.chats.insert(savedHistory.chats.begin(),std::move(item));
        }
        for(auto& item:savedHistory.chats)if(item.id==savedHistory.activeId) {
            item.messages=messages;item.context=apiHistory;
            int length=GetWindowTextLengthW(input);std::wstring draft(length+1,L'\0');GetWindowTextW(input,draft.data(),length+1);draft.resize(length);item.draft=std::move(draft);
            for(const auto& m:messages)if(m.role==L"user"){item.title=m.text.substr(0,80);break;}
            std::replace(item.title.begin(),item.title.end(),L'\n',L' ');std::replace(item.title.begin(),item.title.end(),L'\r',L' ');
        }
        if(historyWritable)chat::SaveChatHistory(configDirectory,savedHistory);
        RefreshChats();
    }catch(const std::exception& error){historyError=chat::Wide(error.what());PostMessageW(app,WM_APP+2,0,0);}
}
void OpenChat(size_t index) {
    if(index>=savedHistory.chats.size())return;
    auto id=savedHistory.chats[index].id;
    if(id==savedHistory.activeId)return;
    if(busy)StopAgent();else PersistChat();
    ++generation;streamingMessage.reset();currentPrompt.reset();pendingAttachments.clear();
    for(const auto& item:savedHistory.chats)if(item.id==id) {
        messages=item.messages;apiHistory=item.context;restoringChat=true;SetWindowTextW(input,item.draft.c_str());restoringChat=false;break;
    }
    textAnchor=textEnd={};savedHistory.activeId=id;scroll=1000000;PersistChat();Layout();SetFocus(input);
}
RECT ChatDeleteRect(RECT row) {return {row.right-Px(36),row.top+Px(2),row.right-Px(4),row.bottom-Px(2)};}
int HistoryRowAt(HWND window,POINT point) {
    RECT client{};GetClientRect(window,&client);if(!PtInRect(&client,point))return -1;
    auto hit=SendMessageW(window,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
    int index=LOWORD(hit);RECT row{};
    if(HIWORD(hit)||index>=static_cast<int>(savedHistory.chats.size())||SendMessageW(window,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&row))==LB_ERR||!PtInRect(&row,point))return -1;
    return index;
}
void DeleteChat(size_t index) {
    if(index>=savedHistory.chats.size())return;
    if(!historyWritable){ShowModalMessage(app,L"History could not be loaded. Repair chat-history.json and restart before deleting saved chats.",L"Chat history",MB_OK|MB_ICONERROR);return;}
    auto id=savedHistory.chats[index].id;bool active=id==savedHistory.activeId;
    if(active&&busy)StopAgent();else PersistChat();
    try {
        auto updated=savedHistory;
        updated.chats.erase(std::remove_if(updated.chats.begin(),updated.chats.end(),[&](const auto& item){return item.id==id;}),updated.chats.end());
        if(active) {
            if(updated.chats.empty())updated.chats.push_back(chat::NewSavedChat(configDirectory,updated));
            updated.activeId=updated.chats.front().id;
        }
        chat::SaveChatHistory(configDirectory,updated);
        savedHistory=std::move(updated);
        if(active) {
            ++generation;streamingMessage.reset();currentPrompt.reset();pendingAttachments.clear();
            for(const auto& item:savedHistory.chats)if(item.id==savedHistory.activeId){messages=item.messages;apiHistory=item.context;restoringChat=true;SetWindowTextW(input,item.draft.c_str());restoringChat=false;break;}
            textAnchor=textEnd={};scroll=1000000;SetFocus(input);
        }
        RefreshChats();Layout();
    }catch(const std::exception& error){ShowModalMessage(app,chat::Wide(error.what()).c_str(),L"Could not delete chat",MB_OK|MB_ICONERROR);}
}
LRESULT CALLBACK HistoryListProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
    if(message==WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window,0};TrackMouseEvent(&tracking);
        hoveredChat=HistoryRowAt(window,point);InvalidateRect(window,nullptr,FALSE);
    }
    if(message==WM_MOUSELEAVE){hoveredChat=-1;InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK) {
        int index=HistoryRowAt(window,point);RECT row{};
        if(index>=0){SendMessageW(window,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&row));auto button=ChatDeleteRect(row);
            if(PtInRect(&button,point)){pressedDeleteChat=savedHistory.chats[index].id;SetCapture(window);return 0;}
        }
    }
    if(message==WM_LBUTTONUP&&!pressedDeleteChat.empty()) {
        auto id=pressedDeleteChat;pressedDeleteChat.clear();ReleaseCapture();
        int index=HistoryRowAt(window,point);RECT row{};
        if(index>=0&&savedHistory.chats[index].id==id){SendMessageW(window,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&row));auto button=ChatDeleteRect(row);if(PtInRect(&button,point))DeleteChat(static_cast<size_t>(index));}
        return 0;
    }
    if(message==WM_CAPTURECHANGED)pressedDeleteChat.clear();
    if(message==WM_SETCURSOR) {
        POINT cursor{};GetCursorPos(&cursor);ScreenToClient(window,&cursor);int index=HistoryRowAt(window,cursor);RECT row{};
        if(index>=0){SendMessageW(window,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&row));auto button=ChatDeleteRect(row);if(PtInRect(&button,cursor)){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}}
    }
    auto result=DefSubclassProc(window,message,wParam,lParam);
    if(message==WM_MOUSEWHEEL||message==WM_VSCROLL){POINT cursor{};GetCursorPos(&cursor);ScreenToClient(window,&cursor);hoveredChat=HistoryRowAt(window,cursor);InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,HistoryListProc,1);
    return result;
}
void Reset() {
    if(busy)StopAgent();else PersistChat();
    worker->Cancel();++generation;busy=false;streamingMessage.reset();currentPrompt.reset();messages.clear();apiHistory.clear();textAnchor=textEnd={};scroll=0;
    pendingAttachments.clear();
    SetWindowTextW(sendButton,L"Send message");InvalidateRect(sendButton,nullptr,TRUE);
    restoringChat=true;SetWindowTextW(input,L"");restoringChat=false;savedHistory.activeId.clear();PersistChat();InvalidateRect(app,nullptr,TRUE);SetFocus(input);
}
void InitModalChrome(HWND window,const wchar_t* title,int contentWidth,int contentHeight);
int ShowModalMessage(HWND owner,const wchar_t* text,const wchar_t* title,UINT type);
LRESULT CALLBACK ButtonProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam);
std::wstring ControlText(HWND window,int id) {
    HWND control=GetDlgItem(window,id);int length=GetWindowTextLengthW(control);
    std::wstring value(length+1,L'\0');GetWindowTextW(control,value.data(),length+1);value.resize(length);return value;
}
std::wstring EditorJson(const nlohmann::json& value) {
    auto text=chat::Wide(value.dump(2));std::wstring result;
    for(auto c:text){if(c==L'\n')result+=L'\r';result+=c;}return result;
}
struct McpEditorState {
    std::future<std::wstring> check;
    std::shared_ptr<chat::Cancellation> cancel;
    ~McpEditorState(){if(cancel)cancel->Cancel();}
};
INT_PTR CALLBACK McpSettingsProc(HWND window,UINT message,WPARAM wParam,LPARAM) {
    auto state=reinterpret_cast<McpEditorState*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        SetWindowTextW(window,L"MCP");
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        auto control=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id,int x,int y,int w,int h) {
            auto child=CreateWindowExW((style&WS_BORDER)?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(w),scale(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(id==201?codeFont:bodyFont),TRUE);return child;
        };
        control(L"STATIC",L"Configure local commands or remote MCP URLs as JSON.\nEnabled servers connect on your next prompt. Save to keep changes.",0,0,24,16,592,52);
        auto editor=control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_WANTRETURN,201,24,80,592,204);
        SendMessageW(editor,EM_SETLIMITTEXT,256*1024,0);
        control(L"EDIT",L"Use Test connections to discover available tools.\nServer credentials are encrypted with your Windows account.",WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,202,24,296,592,64);
        control(L"BUTTON",L"Test connections",WS_TABSTOP,203,24,378,156,32);
        control(L"BUTTON",L"Example",WS_TABSTOP,204,192,378,100,32);
        control(L"BUTTON",L"Disable",WS_TABSTOP,205,300,378,84,32);
        control(L"BUTTON",L"Save",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,396,378,100,32);
        control(L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,506,378,110,32);
        nlohmann::json config={{"mcpServers",nlohmann::json::object()}};
        try{config=chat::LoadMcpConfig(configDirectory);}catch(const std::exception& error){SetDlgItemTextW(window,202,chat::Wide(error.what()).c_str());}
        SetDlgItemTextW(window,201,EditorJson(config).c_str());
        SetDlgItemTextW(window,205,config.value("enabled",true)?L"Disable":L"Enable");
        state=new McpEditorState;SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
        SetFocus(editor);return FALSE;
    }
    if(message==WM_COMMAND) {
        auto id=LOWORD(wParam);
        if(id==201&&HIWORD(wParam)==EN_CHANGE) {
            auto config=nlohmann::json::parse(chat::Utf8(ControlText(window,201)),nullptr,false);
            if(config.is_object()&&(!config.contains("enabled")||config["enabled"].is_boolean()))SetDlgItemTextW(window,205,config.value("enabled",true)?L"Disable":L"Enable");
            return TRUE;
        }
        if(id==IDCANCEL||id==IDOK){SendMessageW(GetParent(window),WM_COMMAND,id,0);return TRUE;}
        if(id==204) {
            if(ControlText(window,201)!=L"{\r\n  \"mcpServers\": {}\r\n}"&&ControlText(window,201)!=L"{\n  \"mcpServers\": {}\n}"&&ShowModalMessage(window,L"Replace the current editor text with disabled example servers?",L"MCP example",MB_YESNO|MB_ICONQUESTION)!=IDYES)return TRUE;
            SetDlgItemTextW(window,201,LR"({
  "mcpServers": {
    "local-example": {
      "enabled": false,
      "command": "node.exe",
      "args": ["C:\\path\\to\\server.js"],
      "env": {},
      "cwd": "."
    },
    "remote-example": {
      "enabled": false,
      "url": "https://example.com/mcp",
      "headers": {"Authorization": "Bearer YOUR_TOKEN"}
    }
  }
})");return TRUE;
        }
        if(id==203||id==205) {
            try {
                auto config=nlohmann::json::parse(chat::Utf8(ControlText(window,201)),nullptr,false);
                if(config.is_discarded())throw std::runtime_error("Invalid JSON. Check quotes, commas, and braces.");
                chat::ValidateMcpConfig(config);
                if(id==205){config["enabled"]=!config.value("enabled",true);chat::SaveMcpConfig(configDirectory,config);SetDlgItemTextW(window,201,EditorJson(config).c_str());SetDlgItemTextW(window,205,config["enabled"].get<bool>()?L"Disable":L"Enable");SetDlgItemTextW(window,202,config["enabled"].get<bool>()?L"MCP enabled for new prompts. Saved servers retained.":L"MCP disabled for new prompts. Saved servers retained.");return TRUE;}
                if(state->check.valid())return TRUE;
                state->cancel=std::make_shared<chat::Cancellation>();auto cancel=state->cancel;
                state->check=std::async(std::launch::async,[config,cancel] {
                    std::wstring result;
                    try {
                        chat::McpClient client(config,configDirectory,*cancel,[&](const std::wstring& text){result+=text+L"\r\n";});
                        for(const auto& definition:client.Definitions()) {
                            auto tool=nlohmann::json::parse(definition);result+=L"  "+chat::Wide(client.Label(tool["function"]["name"].get<std::string>()))+L"\r\n";
                        }
                        if(result.empty())result=L"No enabled MCP servers configured.";
                    }catch(const std::exception& error){result+=chat::Wide(error.what());}
                    return result;
                });
                EnableWindow(GetDlgItem(window,203),FALSE);EnableWindow(GetDlgItem(window,IDOK),FALSE);EnableWindow(GetDlgItem(window,201),FALSE);EnableWindow(GetDlgItem(window,204),FALSE);EnableWindow(GetDlgItem(window,205),FALSE);
                SetDlgItemTextW(window,202,L"Connecting to MCP servers...");SetTimer(window,1,100,nullptr);
            }catch(const std::exception& error){SetDlgItemTextW(window,202,chat::Wide(error.what()).c_str());}
            return TRUE;
        }
    }
    if(message==WM_TIMER&&state&&state->check.valid()&&state->check.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=state->check.get();SetDlgItemTextW(window,202,result.c_str());KillTimer(window,1);
        for(int id:{203,IDOK,201,204,205})EnableWindow(GetDlgItem(window,id),TRUE);return TRUE;
    }
    if(message==WM_CLOSE){SendMessageW(GetParent(window),WM_COMMAND,IDCANCEL,0);return TRUE;}
    if(message==WM_DESTROY){KillTimer(window,1);SetWindowLongPtrW(window,DWLP_USER,0);delete state;return TRUE;}
    if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT) {
        HDC dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);return reinterpret_cast<INT_PTR>(surfaceBrush);
    }
    return FALSE;
}
struct ModelResult {std::vector<std::string> models;std::wstring error;};
void ShowProviderError(HWND owner,const std::wstring& error);
struct SettingsState {
    std::future<ModelResult> request;
    std::shared_ptr<chat::Cancellation> cancel;
    unsigned revision=0,issued=0;
    ULONGLONG due=0;
    bool pending=false;
    bool applyingPreset=false;
    std::wstring error;
    ~SettingsState(){if(cancel)cancel->Cancel();}
};
void QueueModels(HWND window,SettingsState& state) {
    if(state.applyingPreset)return;
    state.error.clear();EnableWindow(GetDlgItem(window,116),FALSE);
    ++state.revision;if(state.cancel)state.cancel->Cancel();
    state.pending=true;state.due=GetTickCount64()+600;
    auto selected=ControlText(window,103);
    SendDlgItemMessageW(window,103,CB_RESETCONTENT,0,0);SetDlgItemTextW(window,103,selected.c_str());
    SetDlgItemTextW(window,104,L"Loading models...");SetTimer(window,1,100,nullptr);
}
INT_PTR CALLBACK ProviderSettingsProc(HWND window,UINT message,WPARAM wParam,LPARAM) {
    auto state=reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        SetWindowTextW(window,L"Provider");
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        auto control=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height) {
            HWND child=CreateWindowExW((style&WS_BORDER)?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(width),scale(height),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);return child;
        };
        control(L"STATIC",L"Chat completions URL",0,0,24,20,400,24);
        control(L"BUTTON",L"Provider type",WS_TABSTOP,115,440,14,176,28);
        control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,101,24,48,592,30);
        control(L"STATIC",L"API key (optional for local endpoints)",0,0,24,92,592,24);
        auto key=control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_PASSWORD,102,24,120,400,30);
        SendMessageW(key,EM_SETPASSWORDCHAR,0x25cf,0);
        control(L"STATIC",L"Auth header",0,0,440,92,176,24);
        auto auth=control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL,111,440,120,176,180);
        for(auto name:{L"auto",L"Authorization",L"api-key",L"Ocp-Apim-Subscription-Key",L"none"})SendMessageW(auth,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        control(L"STATIC",L"Models URL (optional; leave blank for automatic discovery)",0,0,24,164,592,24);
        control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,112,24,192,592,30);
        control(L"STATIC",L"Model or Azure deployment name",0,0,24,236,592,24);
        auto model=control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL,103,24,264,400,180);
        (void)model;
        control(L"STATIC",L"Token limit field",0,0,440,236,176,24);
        auto tokenField=control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,113,440,264,176,180);
        for(auto name:{L"max_tokens",L"max_completion_tokens",L"none"})SendMessageW(tokenField,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        control(L"BUTTON",L"Request token usage in stream (disable if unsupported)",WS_TABSTOP|BS_AUTOCHECKBOX,114,24,422,592,32);
        control(L"EDIT",L"Enter a provider URL to load available models.",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,104,24,306,456,52);
        auto details=control(L"BUTTON",L"Error details",WS_TABSTOP,116,496,306,120,30);EnableWindow(details,FALSE);
        control(L"STATIC",L"Max model turns (1-100)",0,0,24,360,280,24);
        control(L"EDIT",L"8",WS_TABSTOP|WS_BORDER|ES_NUMBER,108,24,388,280,30);
        control(L"STATIC",L"Max tool calls (1-1000)",0,0,336,360,280,24);
        control(L"EDIT",L"16",WS_TABSTOP|WS_BORDER|ES_NUMBER,109,336,388,280,30);
        control(L"BUTTON",L"Save",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,396,444,100,32);
        control(L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,506,444,110,32);
        chat::Config config;
        try{config=chat::LoadConfig(configDirectory,false);}catch(...){ShowModalMessage(window,L"Saved settings could not be loaded. Enter the provider details to replace them.",L"Settings",MB_OK|MB_ICONINFORMATION);}
        SetDlgItemTextW(window,101,chat::Wide(config.url).c_str());SetDlgItemTextW(window,102,chat::Wide(config.key).c_str());SetDlgItemTextW(window,103,chat::Wide(config.model).c_str());
        SetDlgItemTextW(window,111,chat::Wide(config.authHeader).c_str());SetDlgItemTextW(window,112,chat::Wide(config.modelsUrl).c_str());
        SendDlgItemMessageW(window,113,CB_SETCURSEL,config.tokenLimitField=="max_completion_tokens"?1:config.tokenLimitField=="none"?2:0,0);CheckDlgButton(window,114,config.streamUsage?BST_CHECKED:BST_UNCHECKED);
        SetDlgItemInt(window,108,config.maxModelTurns,FALSE);SetDlgItemInt(window,109,config.maxToolCalls,FALSE);
        state=new SettingsState;SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
        if(!config.url.empty())QueueModels(window,*state);
        SetFocus(GetDlgItem(window,101));return FALSE;
    }
    if(message==WM_TIMER&&wParam==1&&state) {
        if(state->request.valid()) {
            if(state->request.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return TRUE;
            auto result=state->request.get();
            if(state->issued==state->revision) {
                auto selected=ControlText(window,103);
                SendDlgItemMessageW(window,103,CB_RESETCONTENT,0,0);
                for(const auto& model:result.models){auto wide=chat::Wide(model);SendDlgItemMessageW(window,103,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(wide.c_str()));}
                if(selected.empty()&&!result.models.empty())selected=chat::Wide(result.models.front());
                auto index=SendDlgItemMessageW(window,103,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(selected.c_str()));
                if(index!=CB_ERR)SendDlgItemMessageW(window,103,CB_SETCURSEL,index,0);else SetDlgItemTextW(window,103,selected.c_str());
                SetDlgItemTextW(window,104,result.error.empty()?L"Select an available model. Click Save to keep your selection.":result.error.c_str());
                state->error=result.error;EnableWindow(GetDlgItem(window,116),!state->error.empty());
            }
        }
        if(state->pending&&GetTickCount64()>=state->due) {
            state->pending=false;
            chat::Config config;config.url=chat::Utf8(ControlText(window,101));config.key=chat::Utf8(ControlText(window,102));config.authHeader=chat::Utf8(ControlText(window,111));config.modelsUrl=chat::Utf8(ControlText(window,112));
            state->issued=state->revision;state->cancel=std::make_shared<chat::Cancellation>();auto cancel=state->cancel;
            try {
                state->request=std::async(std::launch::async,[config,cancel] {
                    ModelResult result;try{result.models=chat::FetchModels(config,*cancel);}catch(const std::exception& error){result.error=chat::Wide(error.what());}catch(...){result.error=L"Could not load models. You can enter a model ID manually.";}return result;
                });
            }catch(...){SetDlgItemTextW(window,104,L"Could not start loading models. You can enter a model ID manually.");}
        }
        if(!state->pending&&!state->request.valid())KillTimer(window,1);
        return TRUE;
    }
    if(message==WM_COMMAND) {
        if(state&&LOWORD(wParam)==116&&HIWORD(wParam)==BN_CLICKED){ShowProviderError(window,state->error);return TRUE;}
        if(state&&LOWORD(wParam)==115&&HIWORD(wParam)==BN_CLICKED) {
            struct Preset {const wchar_t* name;const wchar_t* url;const wchar_t* example;const wchar_t* auth;const wchar_t* model;const wchar_t* hint;bool usage;};
            const Preset presets[]{
                {L"OpenRouter",L"https://openrouter.ai/api/v1/chat/completions",L"",L"Authorization",L"openrouter/auto",L"Enter your OpenRouter key to load models.",true},
                {L"OpenAI",L"https://api.openai.com/v1/chat/completions",L"",L"Authorization",L"",L"Enter your OpenAI key, then choose a model.",true},
                {L"Azure deployment",L"",L"https://RESOURCE.openai.azure.com/openai/deployments/DEPLOYMENT/chat/completions?api-version=VERSION",L"api-key",L"",L"Enter your full deployment URL, API version and deployment name. Use Authorization for an Entra token.",false},
                {L"Azure / Foundry v1",L"",L"https://RESOURCE.openai.azure.com/openai/v1",L"api-key",L"",L"Enter your Azure or Foundry v1 URL and deployment name. Use Authorization for an Entra token.",true},
                {L"Internal gateway",L"",L"https://YOUR-GATEWAY/v1",L"Authorization",L"",L"Enter your gateway URL and authentication header. Set Models URL if your catalog uses a separate endpoint.",false},
                {L"Local / compatible",L"http://localhost:8000/v1",L"",L"none",L"",L"Enter your local server URL and model ID. Choose an authentication header if the server requires a key.",false}
            };
            auto menu=CreatePopupMenu();for(unsigned i=0;i<6;++i)AppendMenuW(menu,MF_STRING,i+1,presets[i].name);
            RECT button{};GetWindowRect(GetDlgItem(window,115),&button);
            auto choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_TOPALIGN,button.left,button.bottom,0,window,nullptr);DestroyMenu(menu);
            if(choice>=1&&choice<=6) {
                const auto& preset=presets[choice-1];state->applyingPreset=true;
                ++state->revision;if(state->cancel)state->cancel->Cancel();state->pending=false;state->error.clear();
                // A credential for the old endpoint must not be sent to a newly selected service.
                SetDlgItemTextW(window,102,L"");SetDlgItemTextW(window,101,preset.url);SetDlgItemTextW(window,112,L"");
                SendDlgItemMessageW(window,101,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(preset.example));
                SetDlgItemTextW(window,111,preset.auth);SendDlgItemMessageW(window,103,CB_RESETCONTENT,0,0);SetDlgItemTextW(window,103,preset.model);
                SendDlgItemMessageW(window,113,CB_SETCURSEL,0,0);CheckDlgButton(window,114,preset.usage?BST_CHECKED:BST_UNCHECKED);
                SetDlgItemTextW(window,115,preset.name);SetDlgItemTextW(window,104,preset.hint);EnableWindow(GetDlgItem(window,116),FALSE);
                state->applyingPreset=false;SetFocus(GetDlgItem(window,*preset.url?102:101));
            }
            return TRUE;
        }
        if(state&&state->applyingPreset)return TRUE;
        if(state&&LOWORD(wParam)==111&&(HIWORD(wParam)==CBN_EDITCHANGE||HIWORD(wParam)==CBN_SELCHANGE)){QueueModels(window,*state);return TRUE;}
        if(state&&HIWORD(wParam)==EN_CHANGE&&(LOWORD(wParam)==101||LOWORD(wParam)==102||LOWORD(wParam)==112)) {
            auto selected=ControlText(window,103);QueueModels(window,*state);SetDlgItemTextW(window,103,selected.c_str());return TRUE;
        }
        if(LOWORD(wParam)==IDCANCEL||LOWORD(wParam)==IDOK){SendMessageW(GetParent(window),WM_COMMAND,LOWORD(wParam),0);return TRUE;}
    }
    if(message==WM_CLOSE){SendMessageW(GetParent(window),WM_COMMAND,IDCANCEL,0);return TRUE;}
    if(message==WM_DESTROY){KillTimer(window,1);SetWindowLongPtrW(window,DWLP_USER,0);delete state;return TRUE;}
    if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX) {
        HDC dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);return reinterpret_cast<INT_PTR>(surfaceBrush);
    }
    return FALSE;
}
struct MouseCircleTest { POINT origin{},center{}; int radius=0,step=0; };
// Exercise the production tool against a local field and verify input delivery.
INT_PTR CALLBACK ComputerSettingsProc(HWND window,UINT message,WPARAM wParam,LPARAM) {
    if(message==WM_INITDIALOG) {
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        auto control=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){
            auto child=CreateWindowW(type,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(width),scale(height),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);return child;
        };
        control(L"STATIC",L"Computer control",0,0,24,20,592,28);
        control(L"STATIC",L"Test mouse clicks, keyboard input, and screenshot access. The test clicks and types in the field below.",0,0,24,60,592,52);
        control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,402,24,128,592,32);
        control(L"BUTTON",L"Test access",WS_TABSTOP,401,24,180,160,32);
        control(L"BUTTON",L"Test mouse circle",WS_TABSTOP,404,200,180,220,32);
        control(L"STATIC",L"Not tested. Keep the desktop unlocked. Elevated apps and secure Windows screens can block control.",0,403,24,230,592,120);
        control(L"BUTTON",L"Save",WS_TABSTOP,IDOK,396,378,100,32);
        control(L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,506,378,110,32);
        return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wParam)==404&&HIWORD(wParam)==BN_CLICKED) {
        if(GetForegroundWindow()!=GetAncestor(window,GA_ROOT))return TRUE;
        auto state=new MouseCircleTest;
        if(!GetCursorPos(&state->origin)){delete state;SetDlgItemTextW(window,403,L"Could not read the mouse position.");return TRUE;}
        RECT rect{};GetWindowRect(window,&rect);
        state->center={(rect.left+rect.right)/2,(rect.top+rect.bottom)/2};
        state->radius=MulDiv(80,GetDpiForWindow(window),96);
        SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
        EnableWindow(GetDlgItem(window,401),FALSE);EnableWindow(GetDlgItem(window,404),FALSE);
        SetDlgItemTextW(window,403,L"Moving the mouse in a circle for three seconds. No clicks are sent.");
        SetTimer(window,5,25,nullptr);return TRUE;
    }
    if(message==WM_TIMER&&wParam==5) {
        auto state=reinterpret_cast<MouseCircleTest*>(GetWindowLongPtrW(window,DWLP_USER));
        if(!state)return TRUE;
        bool active=IsWindowVisible(window)&&GetForegroundWindow()==GetAncestor(window,GA_ROOT);
        double angle=state->step*6.283185307179586/120;
        POINT target{state->center.x+static_cast<LONG>(std::lround(state->radius*std::cos(angle))),state->center.y+static_cast<LONG>(std::lround(state->radius*std::sin(angle)))};
        chat::Cancellation cancel;
        auto result=active?chat::RunComputer({{"action","move"},{"x",target.x},{"y",target.y}},cancel,false):chat::ComputerResult{};
        bool failed=result.output.contains("error");
        if(!active||failed||++state->step>120) {
            KillTimer(window,5);
            auto status=!active?std::wstring(L"Mouse circle stopped because Settings lost focus."):failed?L"Mouse movement failed: "+chat::Wide(result.output["error"].get<std::string>()):std::wstring(L"Mouse circle passed: pointer positions verified throughout the circle.");
            if(active)chat::RunComputer({{"action","move"},{"x",state->origin.x},{"y",state->origin.y}},cancel,false);
            SetDlgItemTextW(window,403,status.c_str());
            SetWindowLongPtrW(window,DWLP_USER,0);delete state;
            EnableWindow(GetDlgItem(window,401),TRUE);EnableWindow(GetDlgItem(window,404),TRUE);
        }
        return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wParam)==401&&HIWORD(wParam)==BN_CLICKED) {
        if(GetForegroundWindow()!=GetAncestor(window,GA_ROOT)) {
            SetDlgItemTextW(window,403,L"Keep Settings in the foreground, then test again.");return TRUE;
        }
        SetDlgItemTextW(window,402,L"");EnableWindow(GetDlgItem(window,401),FALSE);EnableWindow(GetDlgItem(window,404),FALSE);
        SetDlgItemTextW(window,403,L"Testing mouse and keyboard delivery...");
        RECT rect{};GetWindowRect(GetDlgItem(window,402),&rect);
        chat::Cancellation cancel;
        auto result=chat::RunComputer({{"action","click"},{"x",(rect.left+rect.right)/2},{"y",(rect.top+rect.bottom)/2}},cancel);
        if(result.output.contains("error")) {
            SetDlgItemTextW(window,403,chat::Wide(result.output["error"].get<std::string>()).c_str());EnableWindow(GetDlgItem(window,401),TRUE);EnableWindow(GetDlgItem(window,404),TRUE);
        } else SetTimer(window,3,150,nullptr);
        return TRUE;
    }
    if(message==WM_TIMER&&wParam==3) {
        KillTimer(window,3);
        if(!IsWindowVisible(window)||GetForegroundWindow()!=GetAncestor(window,GA_ROOT)||GetFocus()!=GetDlgItem(window,402)) {
            SetDlgItemTextW(window,403,L"Mouse test failed: the test field did not receive the click. Keep Settings in the foreground and retry.");EnableWindow(GetDlgItem(window,401),TRUE);EnableWindow(GetDlgItem(window,404),TRUE);return TRUE;
        }
        chat::Cancellation cancel;auto result=chat::RunComputer({{"action","type"},{"text","Computer control OK"}},cancel);
        if(result.output.contains("error")) {
            SetDlgItemTextW(window,403,chat::Wide(result.output["error"].get<std::string>()).c_str());EnableWindow(GetDlgItem(window,401),TRUE);EnableWindow(GetDlgItem(window,404),TRUE);
        } else SetTimer(window,4,150,nullptr);
        return TRUE;
    }
    if(message==WM_TIMER&&wParam==4) {
        KillTimer(window,4);
        if(ControlText(window,402)!=L"Computer control OK")SetDlgItemTextW(window,403,L"Mouse passed. Keyboard test failed: expected text was not received. Release modifier keys and retry.");
        else {
            chat::Cancellation cancel;auto capture=chat::RunComputer({{"action","screenshot"}},cancel);
            auto text=capture.output.contains("error")?L"Mouse and keyboard passed. Screenshot failed: "+chat::Wide(capture.output["error"].get<std::string>()):std::wstring(L"Access works: mouse click, keyboard input, and screenshot capture passed. Elevated apps may still block input.");
            SetDlgItemTextW(window,403,text.c_str());
        }
        EnableWindow(GetDlgItem(window,401),TRUE);EnableWindow(GetDlgItem(window,404),TRUE);return TRUE;
    }
    if(message==WM_COMMAND&&(LOWORD(wParam)==IDOK||LOWORD(wParam)==IDCANCEL)){SendMessageW(GetParent(window),WM_COMMAND,LOWORD(wParam),0);return TRUE;}
    if(message==WM_DESTROY){KillTimer(window,3);KillTimer(window,4);KillTimer(window,5);delete reinterpret_cast<MouseCircleTest*>(GetWindowLongPtrW(window,DWLP_USER));SetWindowLongPtrW(window,DWLP_USER,0);}
    if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT) {
        auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);return reinterpret_cast<INT_PTR>(surfaceBrush);
    }
    return FALSE;
}
INT_PTR CALLBACK SystemPromptSettingsProc(HWND window,UINT message,WPARAM wParam,LPARAM) {
    if(message==WM_INITDIALOG) {
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        auto control=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){
            auto child=CreateWindowW(type,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(width),scale(height),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);return child;
        };
        control(L"STATIC",L"Edit the system instructions sent to the model. Saved changes apply to the next run.",0,0,24,20,592,40);
        auto editor=control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,501,24,68,592,288);
        SendMessageW(editor,EM_SETLIMITTEXT,32768,0);
        chat::Config config;try{config=chat::LoadConfig(configDirectory,false);}catch(...){}
        auto prompt=chat::Wide(config.systemPrompt);std::wstring display;
        for(size_t i=0;i<prompt.size();++i){if(prompt[i]==L'\n'&&(i==0||prompt[i-1]!=L'\r'))display+=L'\r';display+=prompt[i];}
        SetWindowTextW(editor,display.c_str());
        control(L"BUTTON",L"Restore default",WS_TABSTOP,502,24,378,180,32);
        control(L"BUTTON",L"Save",WS_TABSTOP,IDOK,396,378,100,32);
        control(L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,506,378,110,32);
        return TRUE;
    }
    if(message==WM_COMMAND) {
        if(LOWORD(wParam)==502){SetDlgItemTextW(window,501,chat::Wide(chat::DefaultSystemPrompt()).c_str());return TRUE;}
        if(LOWORD(wParam)==IDOK||LOWORD(wParam)==IDCANCEL){SendMessageW(GetParent(window),WM_COMMAND,LOWORD(wParam),0);return TRUE;}
    }
    if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT) {
        auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);return reinterpret_cast<INT_PTR>(surfaceBrush);
    }
    return FALSE;
}
void DrawButton(const DRAWITEMSTRUCT& item);
// Keep native scrollbar input and accessibility, and paint its nonclient area
// with the same charcoal track and muted thumb used throughout the app.
LRESULT CALLBACK SettingsScrollProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    auto result=DefSubclassProc(window,message,wParam,lParam);
    if(message==WM_NCPAINT||message==WM_PAINT||message==WM_SIZE||message==WM_VSCROLL||message==WM_HSCROLL||message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL||message==WM_NCMOUSEMOVE||message==WM_NCMOUSELEAVE||message==WM_NCLBUTTONDOWN||message==WM_LBUTTONUP||message==WM_MOUSEMOVE||message==WM_TIMER||message==WM_KEYUP||message==WM_SETTEXT) {
        auto dc=GetWindowDC(window);
        if(dc) {
            RECT bounds{};GetWindowRect(window,&bounds);
            for(auto object:{OBJID_VSCROLL,OBJID_HSCROLL}) {
                SCROLLBARINFO info{sizeof(info)};
                if(!GetScrollBarInfo(window,object,&info)||(info.rgstate[0]&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN)))continue;
                auto track=info.rcScrollBar;OffsetRect(&track,-bounds.left,-bounds.top);Fill(dc,track,Surface);
                bool vertical=object==OBJID_VSCROLL;
                auto thumb=track;
                if(vertical){thumb.top=track.top+info.xyThumbTop;thumb.bottom=track.top+info.xyThumbBottom;InflateRect(&thumb,-Px(4),0);}
                else{thumb.left=track.left+info.xyThumbTop;thumb.right=track.left+info.xyThumbBottom;InflateRect(&thumb,0,-Px(4));}
                if(!(info.rgstate[0]&STATE_SYSTEM_UNAVAILABLE)&&info.xyThumbBottom>info.xyThumbTop)Round(dc,thumb,RGB(100,100,100),Px(6));
                for(int end=0;end<2;++end) {
                    int x=vertical?(track.left+track.right)/2:end?track.right-info.dxyLineButton/2:track.left+info.dxyLineButton/2;
                    int y=vertical?(end?track.bottom-info.dxyLineButton/2:track.top+info.dxyLineButton/2):(track.top+track.bottom)/2;
                    int direction=end?1:-1,d=Px(3);
                    if(vertical){Line(dc,x-d,y-direction*d/2,x,y+direction*d/2,Muted);Line(dc,x,y+direction*d/2,x+d,y-direction*d/2,Muted);}
                    else{Line(dc,x-direction*d/2,y-d,x+direction*d/2,y,Muted);Line(dc,x+direction*d/2,y,x-direction*d/2,y+d,Muted);}
                }
            }
            ReleaseDC(window,dc);
        }
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,SettingsScrollProc,3);
    return result;
}
LRESULT CALLBACK SettingsComboProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORLISTBOX) {
        auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);SetDCBrushColor(dc,Surface);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    auto result=DefSubclassProc(window,message,wParam,lParam);
    if(message==WM_NCPAINT||message==WM_PAINT||message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_ENABLE||message==WM_MOUSEMOVE||message==WM_MOUSELEAVE||message==WM_LBUTTONUP) {
        auto dc=GetWindowDC(window);
        if(dc) {
            RECT rect{};GetWindowRect(window,&rect);OffsetRect(&rect,-rect.left,-rect.top);
            auto brush=CreateSolidBrush(Surface);
            // Cover the native recessed frame with the field's background.
            for(int i=0;i<3;++i){FrameRect(dc,&rect,brush);InflateRect(&rect,-1,-1);}
            COMBOBOXINFO info{sizeof(info)};
            if(GetComboBoxInfo(window,&info)) {
                FillRect(dc,&info.rcButton,brush);
                int x=(info.rcButton.left+info.rcButton.right)/2,y=(info.rcButton.top+info.rcButton.bottom)/2;
                Line(dc,x-4,y-2,x,y+2,Muted);Line(dc,x,y+2,x+4,y-2,Muted);
            }
            DeleteObject(brush);ReleaseDC(window,dc);
        }
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,SettingsComboProc,2);
    return result;
}
LRESULT CALLBACK SettingsEditProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_KEYDOWN&&(GetKeyState(VK_CONTROL)&0x8000)&&!(GetKeyState(VK_MENU)&0x8000)) {
        UINT action=wParam=='A'?EM_SETSEL:wParam=='C'?WM_COPY:wParam=='X'?WM_CUT:wParam=='V'?WM_PASTE:wParam=='Z'?WM_UNDO:0;
        if(action){SendMessageW(window,action,0,wParam=='A'?-1:0);return 0;}
    }
    if(message==WM_CHAR&&(GetKeyState(VK_CONTROL)&0x8000)&&(wParam==1||wParam==3||wParam==22||wParam==24||wParam==26))return 0;
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,SettingsEditProc,1);
    return DefSubclassProc(window,message,wParam,lParam);
}
LRESULT CALLBACK SettingsCheckboxProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_PAINT||message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};auto dc=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);
        RECT rect{};GetClientRect(window,&rect);Fill(dc,rect,ModalBody);
        POINT pointer{};GetCursorPos(&pointer);ScreenToClient(window,&pointer);
        bool checked=SendMessageW(window,BM_GETCHECK,0,0)==BST_CHECKED;
        bool enabled=IsWindowEnabled(window)!=FALSE;
        int top=(rect.bottom-Px(20))/2;
        RECT box{0,top,Px(20),top+Px(20)};
        Round(dc,box,checked&&enabled?Ink:PtInRect(&rect,pointer)?Muted:RGB(85,85,85),Px(5));
        if(checked) {
            Line(dc,Px(5),top+Px(10),Px(9),top+Px(14),Background,Px(2));
            Line(dc,Px(9),top+Px(14),Px(15),top+Px(6),Background,Px(2));
        }else{InflateRect(&box,-Px(1),-Px(1));Round(dc,box,Surface,Px(4));}
        rect.left=Px(32);
        Text(dc,ControlText(GetParent(window),GetDlgCtrlID(window)),rect,bodyFont,enabled?Ink:Muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if(GetFocus()==window){InflateRect(&rect,-1,-Px(3));DrawFocusRect(dc,&rect);}
        if(message==WM_PAINT)EndPaint(window,&paint);return 0;
    }
    if(message==WM_MOUSEMOVE){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window,0};TrackMouseEvent(&tracking);}
    auto result=DefSubclassProc(window,message,wParam,lParam);
    if(message==BM_SETCHECK||message==WM_LBUTTONUP||message==WM_KEYUP||message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_ENABLE||message==WM_MOUSEMOVE||message==WM_MOUSELEAVE)InvalidateRect(window,nullptr,FALSE);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,SettingsCheckboxProc,1);
    return result;
}
LRESULT CALLBACK SettingsPageProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND) {
        auto dc=reinterpret_cast<HDC>(wParam);RECT client{};GetClientRect(window,&client);Fill(dc,client,ModalBody);
        for(auto child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            wchar_t type[32]{};GetClassNameW(child,type,32);
            if(lstrcmpiW(type,L"EDIT")&&lstrcmpiW(type,L"COMBOBOX"))continue;
            RECT rect{};GetWindowRect(child,&rect);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&rect),2);
            InflateRect(&rect,2,2);Round(dc,rect,Border,Px(6));InflateRect(&rect,-1,-1);Round(dc,rect,Surface,Px(6));
        }
        return TRUE;
    }
    if(message==WM_CTLCOLORDLG) {SetDCBrushColor(reinterpret_cast<HDC>(wParam),ModalBody);return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));}
    if(message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORLISTBOX) {
        wchar_t type[32]{};GetClassNameW(reinterpret_cast<HWND>(lParam),type,32);
        if(message!=WM_CTLCOLORSTATIC||lstrcmpiW(type,L"EDIT")==0||lstrcmpiW(type,L"COMBOBOX")==0) {
            auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,IsWindowEnabled(reinterpret_cast<HWND>(lParam))?Ink:Muted);SetBkColor(dc,Surface);SetDCBrushColor(dc,Surface);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,ModalBody);SetDCBrushColor(dc,ModalBody);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    if(message==WM_DRAWITEM){
        auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        Fill(item.hDC,item.rcItem,ModalBody);
        POINT pointer{};GetCursorPos(&pointer);bool hover=WindowFromPoint(pointer)==item.hwndItem;
        bool primary=item.CtlID==IDOK;
        Round(item.hDC,item.rcItem,primary?(hover?RGB(215,215,215):Ink):(item.itemState&ODS_SELECTED)?RGB(62,62,62):hover?RGB(47,47,47):Surface,Px(6));
        auto label=item.rcItem;
        bool dropdown=item.CtlID==115;
        if(dropdown){label.left+=Px(12);label.right-=Px(30);}
        Text(item.hDC,ControlText(window,item.CtlID),label,labelFont,(item.itemState&ODS_DISABLED)?Muted:primary?Background:Ink,(dropdown?DT_LEFT:DT_CENTER)|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        if(dropdown){int x=item.rcItem.right-Px(16),y=(item.rcItem.top+item.rcItem.bottom)/2;Line(item.hDC,x-Px(4),y-Px(2),x,y+Px(2),Muted);Line(item.hDC,x,y+Px(2),x+Px(4),y-Px(2),Muted);}
        if(item.itemState&ODS_FOCUS){auto focus=item.rcItem;InflateRect(&focus,-3,-3);DrawFocusRect(item.hDC,&focus);}
        return TRUE;
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,SettingsPageProc,1);
    return DefSubclassProc(window,message,wParam,lParam);
}
struct SettingsTabsState {
    HWND provider=nullptr,mcp=nullptr,computer=nullptr,prompt=nullptr;
    int selected=105;
    std::wstring url,key,model,mcpText,modelTurns,toolCalls,systemPrompt,authHeader,modelsUrl,tokenLimitField;
    bool streamUsage=true;
};
INT_PTR CALLBACK SettingsTabsProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto state=reinterpret_cast<SettingsTabsState*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        // Match the 24 DIP side gutters above the tabs and below the footer.
        InitModalChrome(window,L"Settings",640,596);
        state=new SettingsTabsState;SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        for(int id:{105,106,107,110}) {
            auto button=CreateWindowW(L"BUTTON",id==105?L"Provider":id==106?L"MCP":id==107?L"Computer Control":L"System Prompt",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,scale(24+(id==110?3:id-105)*148),scale(Header+24),scale(148),scale(40),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SetWindowLongPtrW(button,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ButtonProc));
        }
        auto page=[&](DLGPROC proc) {
            struct DialogTemplate {DLGTEMPLATE dialog;WORD menu,windowClass,title;} dialog{};
            dialog.dialog.style=WS_CHILD|WS_CLIPCHILDREN|DS_CONTROL;dialog.dialog.dwExtendedStyle=WS_EX_CONTROLPARENT;dialog.dialog.cx=400;dialog.dialog.cy=270;
            auto child=CreateDialogIndirectParamW(GetModuleHandleW(nullptr),&dialog.dialog,window,proc,0);
            if(!child)return child;
            SetWindowSubclass(child,SettingsPageProc,1,0);
            // Keep controls at their designed DIP size, using the app's shared typography.
            EnumChildWindows(child,[](HWND control,LPARAM parent)->BOOL {
                auto page=reinterpret_cast<HWND>(parent);if(GetParent(control)!=page)return TRUE;
                RECT rect{};GetWindowRect(control,&rect);
                wchar_t type[32]{};GetClassNameW(control,type,32);
                if(lstrcmpiW(type,L"COMBOBOX")==0)SendMessageW(control,CB_GETDROPPEDCONTROLRECT,0,reinterpret_cast<LPARAM>(&rect));
                if(lstrcmpiW(type,L"BUTTON")==0&&(GetWindowLongPtrW(control,GWL_STYLE)&BS_TYPEMASK)!=BS_AUTOCHECKBOX) {
                    auto style=GetWindowLongPtrW(control,GWL_STYLE);
                    SetWindowLongPtrW(control,GWL_STYLE,(style&~BS_TYPEMASK)|BS_OWNERDRAW);
                    SetWindowLongPtrW(control,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ButtonProc));
                }
                if(lstrcmpiW(type,L"BUTTON")==0&&(GetWindowLongPtrW(control,GWL_STYLE)&BS_TYPEMASK)==BS_AUTOCHECKBOX)SetWindowSubclass(control,SettingsCheckboxProc,1,0);
                if(lstrcmpiW(type,L"EDIT")==0) {
                    SetWindowTheme(control,L"DarkMode_Explorer",nullptr);
                    SetWindowSubclass(control,SettingsScrollProc,3,0);
                    SetWindowSubclass(control,SettingsEditProc,1,0);
                    SetWindowLongPtrW(control,GWL_EXSTYLE,GetWindowLongPtrW(control,GWL_EXSTYLE)&~WS_EX_CLIENTEDGE);
                    SetWindowLongPtrW(control,GWL_STYLE,(GetWindowLongPtrW(control,GWL_STYLE)&~WS_BORDER)|ES_NOHIDESEL);
                    SetWindowPos(control,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
                    SendMessageW(control,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(Px(8),Px(8)));
                }
                if(lstrcmpiW(type,L"COMBOBOX")==0) {
                    SetWindowTheme(control,L"DarkMode_CFD",nullptr);
                    SetWindowSubclass(control,SettingsComboProc,2,0);
                    COMBOBOXINFO info{sizeof(info)};
                    if(GetComboBoxInfo(control,&info)) {
                        if(info.hwndItem){SetWindowTheme(info.hwndItem,L"DarkMode_Explorer",nullptr);SetWindowSubclass(info.hwndItem,SettingsEditProc,1,0);}
                        if(info.hwndList){SetWindowTheme(info.hwndList,L"DarkMode_Explorer",nullptr);SetWindowSubclass(info.hwndList,SettingsScrollProc,3,0);}
                    }
                }
                MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&rect),2);
                int id=GetDlgCtrlID(control);
                if(id==IDOK||id==IDCANCEL) {
                    auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(page),96);};
                    MoveWindow(control,scale(id==IDOK?516:404),scale(464),scale(id==IDOK?100:96),scale(36),TRUE);
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(child));
            MoveWindow(child,0,scale(Header+72),scale(640),scale(524),TRUE);return child;
        };
        state->provider=page(ProviderSettingsProc);state->mcp=page(McpSettingsProc);state->computer=page(ComputerSettingsProc);state->prompt=page(SystemPromptSettingsProc);
        if(!state->provider||!state->mcp||!state->computer||!state->prompt){EndDialog(window,IDCANCEL);return TRUE;}
        state->url=ControlText(state->provider,101);state->key=ControlText(state->provider,102);state->model=ControlText(state->provider,103);state->mcpText=ControlText(state->mcp,201);
        state->tokenLimitField=ControlText(state->provider,113);state->streamUsage=IsDlgButtonChecked(state->provider,114)==BST_CHECKED;
        state->authHeader=ControlText(state->provider,111);state->modelsUrl=ControlText(state->provider,112);
        state->systemPrompt=ControlText(state->prompt,501);
        state->modelTurns=ControlText(state->provider,108);state->toolCalls=ControlText(state->provider,109);
        ShowWindow(state->provider,SW_SHOW);ShowWindow(state->mcp,SW_HIDE);SetFocus(GetDlgItem(state->provider,101));return FALSE;
    }
    if(message==WM_COMMAND&&state) {
        int id=LOWORD(wParam);
        if(id==105||id==106||id==107||id==110) {
            state->selected=id;ShowWindow(state->provider,id==105?SW_SHOW:SW_HIDE);ShowWindow(state->mcp,id==106?SW_SHOW:SW_HIDE);ShowWindow(state->computer,id==107?SW_SHOW:SW_HIDE);ShowWindow(state->prompt,id==110?SW_SHOW:SW_HIDE);
            InvalidateRect(GetDlgItem(window,105),nullptr,TRUE);InvalidateRect(GetDlgItem(window,106),nullptr,TRUE);InvalidateRect(GetDlgItem(window,107),nullptr,TRUE);InvalidateRect(GetDlgItem(window,110),nullptr,TRUE);
            SetFocus(GetDlgItem(id==105?state->provider:id==106?state->mcp:id==107?state->computer:state->prompt,id==105?101:id==106?201:id==107?401:501));return TRUE;
        }
        if(id==IDCANCEL){EndDialog(window,IDCANCEL);return TRUE;}
        if(id==IDOK) {
            try {
                auto url=ControlText(state->provider,101),key=ControlText(state->provider,102),model=ControlText(state->provider,103),mcpText=ControlText(state->mcp,201);
                auto modelTurns=ControlText(state->provider,108),toolCalls=ControlText(state->provider,109);
                auto limit=[&](const std::wstring& text,int maximum,const char* error){
                    if(text.empty()||text.size()>4||text.find_first_not_of(L"0123456789")!=text.npos)throw std::runtime_error(error);
                    int value=std::stoi(text);if(value<1||value>maximum)throw std::runtime_error(error);return value;
                };
                int turns=limit(modelTurns,100,"Maximum model turns must be between 1 and 100.");
                int calls=limit(toolCalls,1000,"Maximum tool calls must be between 1 and 1000.");
                auto systemPrompt=ControlText(state->prompt,501);
                auto authHeader=ControlText(state->provider,111),modelsUrl=ControlText(state->provider,112);
                auto tokenLimitField=ControlText(state->provider,113);bool streamUsage=IsDlgButtonChecked(state->provider,114)==BST_CHECKED;
                bool providerChanged=tokenLimitField!=state->tokenLimitField||streamUsage!=state->streamUsage||authHeader!=state->authHeader||modelsUrl!=state->modelsUrl||url!=state->url||key!=state->key||model!=state->model||modelTurns!=state->modelTurns||toolCalls!=state->toolCalls||systemPrompt!=state->systemPrompt;
                auto mcp=nlohmann::json::parse(chat::Utf8(mcpText),nullptr,false);
                if(mcp.is_discarded())throw std::runtime_error("Invalid MCP JSON. Check quotes, commas, and braces.");
                chat::ValidateMcpConfig(mcp);
                chat::Config config;config.url=chat::Utf8(url);config.key=chat::Utf8(key);config.model=chat::Utf8(model);
                config.tokenLimitField=chat::Utf8(tokenLimitField);config.streamUsage=streamUsage;
                config.authHeader=chat::Utf8(authHeader);config.modelsUrl=chat::Utf8(modelsUrl);
                config.maxModelTurns=turns;config.maxToolCalls=calls;config.systemPrompt=chat::Utf8(systemPrompt);
                if(providerChanged||state->selected==105)chat::ValidateConfig(config);
                if(providerChanged||state->selected==105)chat::SaveConfig(configDirectory,config);
                if(mcpText!=state->mcpText||state->selected==106)chat::SaveMcpConfig(configDirectory,mcp);
                EndDialog(window,IDOK);
            }catch(const std::exception& error){ShowModalMessage(window,chat::Wide(error.what()).c_str(),L"Could not save settings",MB_OK|MB_ICONERROR);}
            return TRUE;
        }
    }
    if(message==WM_DRAWITEM&&state&&(wParam==105||wParam==106||wParam==107||wParam==110)) {
        auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);bool selected=state->selected==static_cast<int>(item.CtlID);
        POINT point{};GetCursorPos(&point);ScreenToClient(item.hwndItem,&point);
        Fill(item.hDC,item.rcItem,ModalBody);
        if(selected||PtInRect(&item.rcItem,point))Round(item.hDC,item.rcItem,selected?Surface:Panel,Px(6));
        Text(item.hDC,item.CtlID==105?L"Provider":item.CtlID==106?L"MCP":item.CtlID==107?L"Computer Control":L"System Prompt",item.rcItem,labelFont,selected?Ink:Muted,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
        if(item.itemState&ODS_FOCUS){auto focus=item.rcItem;InflateRect(&focus,-3,-3);DrawFocusRect(item.hDC,&focus);}
        return TRUE;
    }
    if(message==WM_CLOSE){EndDialog(window,IDCANCEL);return TRUE;}
    if(message==WM_DESTROY){SetWindowLongPtrW(window,DWLP_USER,0);delete state;return TRUE;}
    return FALSE;
}
void OpenSettings() {
    struct DialogTemplate {DLGTEMPLATE dialog;WORD menu,windowClass,title;} dialog{};
    // InitModalChrome centers the final sized dialog over its owner.
    dialog.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
    dialog.dialog.cx=326;dialog.dialog.cy=238;
    if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&dialog.dialog,app,SettingsTabsProc,0)==-1)
        ShowModalMessage(app,L"Could not open the settings window.",L"Settings",MB_OK|MB_ICONERROR);
}
void Send() {
    if(busy)return;
    int length=GetWindowTextLengthW(input);std::wstring value(length+1,L'\0');
    GetWindowTextW(input,value.data(),length+1);value.resize(length);
    auto first=value.find_first_not_of(L" \t\r\n");if(first==value.npos&&pendingAttachments.empty())return;
    value=first==value.npos?L"":value.substr(first,value.find_last_not_of(L" \t\r\n")-first+1);
    chat::Message request{L"user",value};request.attachments=std::move(pendingAttachments);pendingAttachments.clear();
    auto display=value;
    if(!request.attachments.empty())display+=(display.empty()?L"":L"\n\n")+std::wstring(L"Attached: ")+chat::AttachmentNames(request.attachments);
    currentPrompt=messages.size();messages.push_back({L"user",display});messages.back().activity=L"LLM call \u00b7 Preparing request";
    apiHistory.push_back(std::move(request));SetWindowTextW(input,L"");busy=true;streamingMessage.reset();scroll=1000000;
    PersistChat();
    try {worker->Start(apiHistory,generation,chat::ChatWorkspace(configDirectory,savedHistory.activeId));}catch(...){busy=false;messages[*currentPrompt].activity=L"LLM call \u00b7 Failed";messages.push_back({L"error",L"Unable to start the AI request. Please try again."});}
    SetWindowTextW(sendButton,busy?L"Stop agent":L"Send message");InvalidateRect(sendButton,nullptr,TRUE);
    InvalidateRect(app,nullptr,TRUE);
}
void StopAgent() {
    if(!busy)return;
    worker->Cancel();++generation;busy=false;
    if(streamingMessage)messages[*streamingMessage].role=L"partial";
    if(currentPrompt)messages[*currentPrompt].activity=L"Stopped";
    streamingMessage.reset();currentPrompt.reset();
    SetWindowTextW(sendButton,L"Send message");InvalidateRect(sendButton,nullptr,TRUE);
    PersistChat();InvalidateRect(app,nullptr,TRUE);SetFocus(input);
}
void RecallPrompt(bool previous) {
    std::vector<std::wstring> prompts;
    for(const auto& message:apiHistory) {
        bool screenshot=std::any_of(message.attachments.begin(),message.attachments.end(),[](const auto& a){return a.name==L"Desktop screenshot";});
        if(message.role==L"user"&&!message.text.empty()&&!screenshot)prompts.push_back(message.text);
    }
    if(prompts.empty())return;
    if(!recalledPrompt) {
        if(!previous)return;
        promptRecallDraft=ControlText(app,3);promptDraftSelection=static_cast<DWORD>(SendMessageW(input,EM_GETSEL,0,0));recalledPrompt=prompts.size();
    }
    auto position=std::min(*recalledPrompt,prompts.size());
    if(previous){if(position>0)--position;}else if(position<prompts.size())++position;
    recallingPrompt=true;SetWindowTextW(input,position<prompts.size()?prompts[position].c_str():promptRecallDraft.c_str());recallingPrompt=false;
    if(position==prompts.size()) {
        SendMessageW(input,EM_SETSEL,LOWORD(promptDraftSelection),HIWORD(promptDraftSelection));recalledPrompt.reset();promptRecallDraft.clear();
    } else {recalledPrompt=position;auto end=GetWindowTextLengthW(input);SendMessageW(input,EM_SETSEL,end,end);}
    SendMessageW(input,EM_SCROLLCARET,0,0);
}
void AlignComposerBrush(HDC dc,HWND control);
LRESULT CALLBACK InputProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND&&composerBackdropBrush) {
        auto dc=reinterpret_cast<HDC>(wParam);RECT rect{};GetClientRect(window,&rect);
        AlignComposerBrush(dc,window);FillRect(dc,&rect,composerBackdropBrush);return 1;
    }
    if(message==WM_DROPFILES)return SendMessageW(app,message,wParam,lParam);
    if(message==WM_KEYDOWN) {
        bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        if(control&&!alt) {
            if(wParam=='A'){SendMessageW(window,EM_SETSEL,0,-1);return 0;}
            if(wParam=='C'){SendMessageW(window,WM_COPY,0,0);return 0;}
            if(wParam=='X'){SendMessageW(window,WM_CUT,0,0);return 0;}
            if(wParam=='V'){SendMessageW(window,WM_PASTE,0,0);return 0;}
            if(wParam=='Z'){SendMessageW(window,WM_UNDO,0,0);return 0;}
        }
        if(!control&&!shift&&!alt&&(wParam==VK_UP||wParam==VK_DOWN)){RecallPrompt(wParam==VK_UP);return 0;}
    }
    if(message==WM_CHAR&&(GetKeyState(VK_CONTROL)&0x8000)&&(wParam==1||wParam==3||wParam==22||wParam==24||wParam==26))return 0;
    if(message==WM_CONTEXTMENU) {
        SetFocus(window);auto selection=static_cast<DWORD>(SendMessageW(window,EM_GETSEL,0,0));bool selected=LOWORD(selection)!=HIWORD(selection);
        auto menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING|(SendMessageW(window,EM_CANUNDO,0,0)?0:MF_GRAYED),1,L"Undo\tCtrl+Z");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(selected?0:MF_GRAYED),2,L"Cut\tCtrl+X");AppendMenuW(menu,MF_STRING|(selected?0:MF_GRAYED),3,L"Copy\tCtrl+C");
        AppendMenuW(menu,MF_STRING|((IsClipboardFormatAvailable(CF_UNICODETEXT)||IsClipboardFormatAvailable(CF_TEXT))?0:MF_GRAYED),4,L"Paste\tCtrl+V");
        AppendMenuW(menu,MF_STRING|(selected?0:MF_GRAYED),5,L"Delete");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(GetWindowTextLengthW(window)>0?0:MF_GRAYED),6,L"Select all\tCtrl+A");
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if(point.x==-1&&point.y==-1){RECT rect{};GetWindowRect(window,&rect);point={rect.left+Px(12),rect.top+Px(12)};}
        auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);
        if(command==1)SendMessageW(window,WM_UNDO,0,0);else if(command==2)SendMessageW(window,WM_CUT,0,0);else if(command==3)SendMessageW(window,WM_COPY,0,0);else if(command==4)SendMessageW(window,WM_PASTE,0,0);else if(command==5)SendMessageW(window,WM_CLEAR,0,0);else if(command==6)SendMessageW(window,EM_SETSEL,0,-1);
        return 0;
    }
    if(message==WM_SETTEXT&&!recallingPrompt){recalledPrompt.reset();promptRecallDraft.clear();}
    if(message==WM_KEYDOWN&&wParam==VK_RETURN&&!(GetKeyState(VK_SHIFT)&0x8000)){Send();return 0;}
    if(message==WM_CHAR&&wParam==L'\r'&&!(GetKeyState(VK_SHIFT)&0x8000))return 0;
    LRESULT result=CallWindowProcW(oldInputProc,window,message,wParam,lParam);
    if(message==WM_SETTEXT) {
        Layout(true); // Multiline edits do not notify EN_CHANGE here.
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE);
    }
    if(message==WM_PAINT&&GetWindowTextLengthW(window)==0) {
        HDC dc=GetDC(window);RECT rect;GetClientRect(window,&rect);
        Text(dc,L"Ask anything or drop files...",rect,bodyFont,Muted);ReleaseDC(window,dc);
    }
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS)InvalidateRect(window,nullptr,FALSE);
    return result;
}
LRESULT CALLBACK ButtonProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_MOUSEMOVE){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window,0};TrackMouseEvent(&tracking);InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_MOUSELEAVE)InvalidateRect(window,nullptr,FALSE);
    return CallWindowProcW(oldButtonProc,window,message,wParam,lParam);
}
int ActivityHeight(HDC dc,const chat::Message& message,int width) {
    if(message.activity.empty())return 0;
    RECT rect{0,0,width*3/4-Px(32),0};SelectObject(dc,smallFont);
    DrawTextW(dc,message.activity.c_str(),-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    return rect.bottom+Px(8);
}
int CodeText(HDC dc,const chat::CodeBlock& block,RECT rect,bool paint) {
    int y=rect.top;
        SelectObject(dc,codeFont);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);
        int cell=std::max(1L,metrics.tmAveCharWidth),line=std::max(1L,metrics.tmHeight)+Px(3);
        int columns=std::max(1,(static_cast<int>(rect.right-rect.left)-Px(24))/cell);
        int row=0,column=0;
        struct Glyph {wchar_t value;int row,column;chat::CodeColor color;};std::vector<Glyph> glyphs;
        auto colors=chat::Highlight(block.text,block.language);
        for(size_t i=0;i<block.text.size();++i) {
            wchar_t c=block.text[i];if(c==L'\r')continue;
            if(c==L'\n'){++row;column=0;continue;}
            if(c==L'\t'){int spaces=4-column%4;while(spaces--){if(column>=columns){++row;column=0;}if(paint)glyphs.push_back({L' ',row,column,colors[i]});++column;}continue;}
            if(column>=columns){++row;column=0;}
            if(paint)glyphs.push_back({c,row,column,colors[i]});++column;
        }
        int rows=row+1;if(!block.text.empty()&&block.text.back()==L'\n'&&row>0)--rows;
        int height=Px(36)+rows*line+Px(12);
        if(paint) {
            Round(dc,{rect.left,y,rect.right,y+height},Panel,Px(8));
            Text(dc,block.language.empty()?L"code":block.language,{rect.left+Px(12),y+Px(8),rect.right-Px(12),y+Px(28)},smallFont,Muted,DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,codeFont);SetBkMode(dc,TRANSPARENT);
            for(const auto& glyph:glyphs) {
                COLORREF ink=Ink;
                switch(glyph.color){case chat::CodeColor::String:ink=RGB(206,145,120);break;case chat::CodeColor::Comment:ink=RGB(106,153,85);break;case chat::CodeColor::Keyword:ink=RGB(197,134,192);break;case chat::CodeColor::Variable:ink=RGB(156,220,254);break;case chat::CodeColor::Tag:ink=RGB(86,156,214);break;case chat::CodeColor::Number:ink=RGB(181,206,168);break;default:break;}
                RecordGlyphs(dc,std::wstring(1,glyph.value),rect.left+Px(12)+glyph.column*cell,y+Px(36)+glyph.row*line);
                SetTextColor(dc,ink);TextOutW(dc,rect.left+Px(12)+glyph.column*cell,y+Px(36)+glyph.row*line,&glyph.value,1);
            }
        }
    return height+Px(8);
}
int PlainResponseText(HDC dc,const std::wstring& value,RECT rect,bool paint,COLORREF color) {
    int y=rect.top;
    for(const auto& block:chat::FormatBlocks(value)) {
        if(block.code){y+=CodeText(dc,block,{rect.left,y,rect.right,y},paint);continue;}
        RECT measured{rect.left,y,rect.right,y};SelectObject(dc,bodyFont);
        DrawTextW(dc,block.text.c_str(),-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        if(paint)Text(dc,block.text,measured,bodyFont,color);y=measured.bottom;
    }
    return y-rect.top;
}
HFONT MarkdownFont(int heading,unsigned style) {
    auto key=std::make_pair(heading,style);auto existing=markdownFonts.find(key);if(existing!=markdownFonts.end())return existing->second;
    int sizes[]{16,26,23,20,18,16,16};int size=sizes[std::clamp(heading,0,6)];
    if((style&chat::MdCode)&&!heading)size=14;
    auto font=CreateFontW(-Px(size),0,0,0,(heading||style&chat::MdBold)?FW_SEMIBOLD:FW_NORMAL,style&chat::MdItalic,style&chat::MdLink,style&chat::MdStrike,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,style&chat::MdCode?L"Consolas":L"Segoe UI");
    markdownFonts.emplace(key,font);return font;
}
bool SafeResponseLink(const std::wstring& href) {
    if(href.find_first_of(L"\r\n\t")!=href.npos||href.find(L'\0')!=href.npos)return false;
    return _wcsnicmp(href.c_str(),L"https://",8)==0||_wcsnicmp(href.c_str(),L"http://",7)==0||_wcsnicmp(href.c_str(),L"mailto:",7)==0;
}
int RichText(HDC dc,const std::vector<chat::MdRun>& runs,RECT rect,bool paint,COLORREF color,int heading=0,int align=0,bool bold=false) {
    struct Piece {std::wstring text,href;HFONT font;unsigned style;int x,width,ascent,height;};
    std::vector<Piece> line;int y=rect.top,x=0,ascent=0,descent=0;
    int width=std::max(1L,rect.right-rect.left);
    auto flush=[&] {
        if(line.empty()){SelectObject(dc,MarkdownFont(heading,0));TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);ascent=tm.tmAscent;descent=tm.tmDescent;}
        int offset=align==2?std::max(0,(width-x)/2):align==3?std::max(0,width-x):0;
        if(paint)for(const auto& piece:line) {
            int left=rect.left+offset+piece.x,top=y+ascent-piece.ascent;
            if(piece.style&chat::MdCode)Round(dc,{left,top-Px(1),left+piece.width,top+piece.height+Px(1)},Surface,Px(3));
            SelectObject(dc,piece.font);SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,piece.style&chat::MdLink?RGB(110,180,245):piece.style&chat::MdCode?RGB(220,190,160):color);
            TextOutW(dc,left+((piece.style&chat::MdCode)?Px(3):0),top,piece.text.c_str(),static_cast<int>(piece.text.size()));
            RecordGlyphs(dc,piece.text,left+((piece.style&chat::MdCode)?Px(3):0),top);
            if(!piece.href.empty()&&SafeResponseLink(piece.href))responseLinks.push_back({{left,top,left+piece.width,top+piece.height},piece.href});
        }
        y+=ascent+descent+Px(4);line.clear();x=ascent=descent=0;
    };
    for(const auto& run:runs) {
        unsigned style=run.style|(bold?chat::MdBold:0);HFONT font=MarkdownFont(heading,style);
        SelectObject(dc,font);TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);
        size_t position=0;
        while(position<run.text.size()) {
            wchar_t c=run.text[position];if(c==L'\r'){++position;continue;}if(c==L'\n'){flush();SelectObject(dc,font);++position;continue;}
            bool space=!(style&chat::MdCode)&&(c==L' '||c==L'\t');size_t end=position+1;
            while(end<run.text.size()&&run.text[end]!=L'\n'&&run.text[end]!=L'\r'&&((style&chat::MdCode)||((run.text[end]==L' '||run.text[end]==L'\t')==space)))++end;
            std::wstring token=run.text.substr(position,end-position);std::replace(token.begin(),token.end(),L'\t',L' ');position=end;
            if(space&&x==0)continue;
            size_t start=0;
            while(start<token.size()) {
                SelectObject(dc,font);int padding=(style&chat::MdCode)?Px(6):0;SIZE extent{};
                GetTextExtentPoint32W(dc,token.data()+start,static_cast<int>(token.size()-start),&extent);
                if(x&&extent.cx+padding>width-x){flush();SelectObject(dc,font);if(space)break;}
                int fit=0;GetTextExtentExPointW(dc,token.data()+start,static_cast<int>(token.size()-start),std::max(1,width-x-padding),&fit,nullptr,&extent);
                size_t take=static_cast<size_t>(std::max(1,fit));take=std::min(take,token.size()-start);
                if(take<token.size()-start&&token[start+take-1]>=0xd800&&token[start+take-1]<=0xdbff){if(take>1)--take;else ++take;}
                GetTextExtentPoint32W(dc,token.data()+start,static_cast<int>(take),&extent);
                line.push_back({token.substr(start,take),run.href,font,style,x,static_cast<int>(extent.cx)+padding,static_cast<int>(tm.tmAscent),static_cast<int>(tm.tmHeight)});
                x+=extent.cx+padding;ascent=std::max(ascent,static_cast<int>(tm.tmAscent));descent=std::max(descent,static_cast<int>(tm.tmDescent));start+=take;
                if(start<token.size())flush();
            }
        }
    }
    if(!line.empty()||y==rect.top)flush();return y-rect.top;
}
int ResponseText(HDC dc,const std::wstring& value,RECT rect,bool paint,COLORREF color,bool markdown) {
    if(!markdown)return PlainResponseText(dc,value,rect,paint,color);
    int y=rect.top;
    for(const auto& block:chat::ParseMarkdown(value)) {
        int left=rect.left+Px(block.quote*16+(block.indent?((block.indent-1)*24+32):0));
        int top=y;if(block.kind==chat::MdKind::Heading&&y>rect.top)y+=Px(6);
        if(block.kind==chat::MdKind::Code)y+=CodeText(dc,{block.text,block.language,true},{left,y,rect.right,y},paint);
        else if(block.kind==chat::MdKind::Rule){if(paint)Line(dc,left,y+Px(10),rect.right,y+Px(10));y+=Px(24);}
        else if(block.kind==chat::MdKind::Table) {
            size_t columns=0;for(const auto& row:block.rows)columns=std::max(columns,row.size());
            if(!columns)continue;
            int cellWidth=std::max(1,static_cast<int>(rect.right-left)/static_cast<int>(columns));
            for(const auto& row:block.rows) {
                int height=0;
                for(size_t i=0;i<row.size();++i)height=std::max(height,RichText(dc,row[i].runs,{left+static_cast<int>(i)*cellWidth+Px(8),0,left+static_cast<int>(i+1)*cellWidth-Px(8),0},false,color,0,row[i].align,row[i].header)+Px(16));
                if(paint)for(size_t i=0;i<row.size();++i) {
                    int x=left+static_cast<int>(i)*cellWidth;RECT cell{x,y,x+cellWidth,y+height};
                    if(row[i].header)Fill(dc,cell,Panel);
                    SaveDC(dc);IntersectClipRect(dc,cell.left+1,cell.top+1,cell.right,cell.bottom);
                    RichText(dc,row[i].runs,{x+Px(8),y+Px(8),x+cellWidth-Px(8),y+height-Px(8)},true,color,0,row[i].align,row[i].header);RestoreDC(dc,-1);
                    Line(dc,cell.left,cell.top,cell.right,cell.top);Line(dc,cell.left,cell.bottom-1,cell.right,cell.bottom-1);
                    Line(dc,cell.left,cell.top,cell.left,cell.bottom);Line(dc,cell.right-1,cell.top,cell.right-1,cell.bottom);
                }
                y+=height;
            }
            y+=Px(10);
        } else {
            if(paint&&!block.marker.empty())Text(dc,block.marker,{left-Px(36),y,left-Px(8),y+Px(28)},bodyFont,color,DT_SINGLELINE|DT_RIGHT|DT_NOPREFIX);
            y+=RichText(dc,block.runs,{left,y,rect.right,y},paint,color,block.heading);
            y+=Px(block.indent?4:10);
        }
        if(paint)for(int quote=0;quote<block.quote;++quote)Line(dc,rect.left+Px(quote*16+2),top,rect.left+Px(quote*16+2),y-Px(4),RGB(80,80,80),Px(2));
    }
    return y-rect.top;
}
int MessageHeight(HDC dc,const chat::Message& message,int width) {
    bool user=message.role==L"user";
    int textWidth=user?width*3/4-Px(32):width;
    if(!user)return ResponseText(dc,message.text,{0,0,textWidth,0},false,Ink,message.role==L"assistant"||message.role==L"streaming"||message.role==L"partial")+ActivityHeight(dc,message,width);
    RECT rect{0,0,textWidth,0};SelectObject(dc,bodyFont);
    DrawTextW(dc,message.text.c_str(),-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    return rect.bottom+(user?Px(24):0)+ActivityHeight(dc,message,width);
}
void ComposerBackground(HDC dc) {
    auto rect=layout.composer;int width=rect.right-rect.left,height=rect.bottom-rect.top;
    if(width<=0||height<=0)return;
    // Blur a reduced backdrop, then tint it; work stays proportional to the prompt size.
    int smallWidth=std::max(1,width/4),smallHeight=std::max(1,height/4);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=smallWidth;
    info.bmiHeader.biHeight=-smallHeight;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* data=nullptr;auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
    auto sample=CreateCompatibleDC(dc);auto previous=SelectObject(sample,bitmap);
    if(!bitmap||!data||!sample){if(sample){SelectObject(sample,previous);DeleteDC(sample);}if(bitmap)DeleteObject(bitmap);Fill(dc,rect,Surface);return;}
    SetStretchBltMode(sample,HALFTONE);SetBrushOrgEx(sample,0,0,nullptr);
    StretchBlt(sample,0,0,smallWidth,smallHeight,dc,rect.left,rect.top,width,height,SRCCOPY);GdiFlush();
    auto pixels=static_cast<unsigned char*>(data);std::vector<unsigned char> scratch(static_cast<size_t>(smallWidth)*smallHeight*4);
    for(int pass=0;pass<2;++pass) {
        for(int y=0;y<smallHeight;++y)for(int x=0;x<smallWidth;++x)for(int channel=0;channel<3;++channel) {
            int sum=0;
            for(int delta=-2;delta<=2;++delta) {
                int sx=pass?x:std::clamp(x+delta,0,smallWidth-1),sy=pass?std::clamp(y+delta,0,smallHeight-1):y;
                sum+=pixels[(static_cast<size_t>(sy)*smallWidth+sx)*4+channel];
            }
            scratch[(static_cast<size_t>(y)*smallWidth+x)*4+channel]=static_cast<unsigned char>(sum/5);
        }
        memcpy(pixels,scratch.data(),scratch.size());
    }
    for(size_t i=0;i<scratch.size();i+=4)for(int channel=0;channel<3;++channel)
        pixels[i+channel]=static_cast<unsigned char>((pixels[i+channel]*24+36*76)/100);
    auto full=CreateCompatibleDC(dc);auto fullBitmap=CreateCompatibleBitmap(dc,width,height);auto old=SelectObject(full,fullBitmap);
    SetStretchBltMode(full,HALFTONE);SetBrushOrgEx(full,0,0,nullptr);
    StretchBlt(full,0,0,width,height,sample,0,0,smallWidth,smallHeight,SRCCOPY);
    auto brush=CreatePatternBrush(fullBitmap);
    if(brush){if(composerBackdropBrush)DeleteObject(composerBackdropBrush);composerBackdropBrush=brush;}
    BitBlt(dc,rect.left,rect.top,width,height,full,0,0,SRCCOPY);
    SelectObject(full,old);DeleteObject(fullBitmap);DeleteDC(full);
    SelectObject(sample,previous);DeleteObject(bitmap);DeleteDC(sample);
    RedrawWindow(input,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE);
    InvalidateRect(sendButton,nullptr,FALSE);InvalidateRect(clearAttachmentsButton,nullptr,FALSE);
}
void AlignComposerBrush(HDC dc,HWND control) {
    RECT rect{};GetWindowRect(control,&rect);MapWindowPoints(nullptr,app,reinterpret_cast<POINT*>(&rect),2);
    SetBrushOrgEx(dc,layout.composer.left-rect.left,layout.composer.top-rect.top,nullptr);
}
void Paint(HDC dc) {
    responseLinks.clear();textGlyphs.clear();captureText=false;
    auto& r=layout;
    Fill(dc,r.client,Background);
    Text(dc,L"\uf264",{Px(Space),0,Px(Space+24),Px(Header)},navIconFont,Ink,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    Line(dc,0,Px(Header)-1,r.client.right,Px(Header)-1);
    Line(dc,sideWidth,Px(Header),sideWidth,r.client.bottom);
    int sectionTop=Px(Header+72+56+32);
    if(sidebarWide)Text(dc,L"CHAT HISTORY",{Px(24),sectionTop,sideWidth-Px(24),sectionTop+Px(20)},labelFont,Muted,DT_SINGLELINE|DT_NOPREFIX);
    SaveDC(dc);IntersectClipRect(dc,r.conversation.left,r.conversation.top,r.conversation.right,r.conversation.bottom);
    if(messages.empty()) {
        int top=r.conversation.top;
        Text(dc,L"What can we build?",{r.column.left,top,r.column.right,top+Px(40)},headingFont);
        Text(dc,L"Ask a question, describe what you want to create, or drop images and documents.",{r.column.left,top+Px(52),r.column.right,top+Px(104)},bodyFont,Muted);
    }
    int width=r.column.right-r.column.left;
    std::vector<int> heights;heights.reserve(messages.size());
    int total=0;for(const auto& message:messages){int height=MessageHeight(dc,message,width);heights.push_back(height);total+=height+Px(Space);}
    if(busy&&!streamingMessage)total+=Px(40);
    // Leave enough scrollable space to bring the last message above the floating prompt.
    if(total>0)total+=r.client.bottom-r.composer.top;
    scroll=std::clamp(scroll,0,std::max(0,total-static_cast<int>(r.conversation.bottom-r.conversation.top)));
    if(conversationExtent!=total||conversationPage!=std::max(1L,r.conversation.bottom-r.conversation.top)) {
        conversationExtent=total;conversationPage=std::max(1L,r.conversation.bottom-r.conversation.top);
    }
    InvalidateRect(conversationScroll,nullptr,FALSE);
    UpdateWindow(conversationScroll);
    ShowWindow(conversationScroll,total>r.conversation.bottom-r.conversation.top?SW_SHOW:SW_HIDE);
    int y=r.conversation.top-scroll;
    for(size_t index=0;index<messages.size();++index) {
        const auto& message=messages[index];
        bool user=message.role==L"user";int height=heights[index];
        if(!HasTextSelection()&&!dragText&&(y+height<r.conversation.top||y>=r.conversation.bottom)){y+=height+Px(Space);continue;}
        captureText=true;textMessage=index;textOffset=0;
        int contentHeight=height-ActivityHeight(dc,message,width);
        int left=user?r.column.left+width/4:r.column.left;
        if(user)Round(dc,{left,y,r.column.right,y+contentHeight},Surface,Px(20));
        int padding=user?Px(16):0,topPadding=user?Px(12):0;
        auto color=message.role==L"error"?RGB(244,161,148):message.role==L"tool_display"?Muted:Ink;
        if(user)Text(dc,message.text,{left+padding,y+topPadding,r.column.right-padding,y+contentHeight},bodyFont,color);
        else ResponseText(dc,message.text,{left,y,r.column.right,y+contentHeight},true,color,message.role==L"assistant"||message.role==L"streaming"||message.role==L"partial");
        if(!message.activity.empty())Text(dc,message.activity,{left+Px(16),y+contentHeight+Px(8),r.column.right-Px(16),y+height},smallFont,Muted);
        captureText=false;y+=height+Px(Space);
    }
    for(const auto& glyph:textGlyphs)if(glyph.position>=std::min(textAnchor,textEnd)&&glyph.position<std::max(textAnchor,textEnd)) {
        auto rect=glyph.rect;PatBlt(dc,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,DSTINVERT);
    }
    if(busy&&!streamingMessage)Text(dc,L"Thinking...",{r.column.left,y,r.column.right,y+Px(28)},bodyFont,Muted);
    RestoreDC(dc,-1);
    ComposerBackground(dc);
    if(!pendingAttachments.empty())Text(dc,L"Attached: "+chat::AttachmentNames(pendingAttachments),{r.composer.left+Px(24),r.composer.top+Px(12),r.composer.right-Px(56),r.composer.top+Px(36)},smallFont,Ink,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    // The composer stays rectangular, with the same 16px inset on all sides.
}
void DrawButton(const DRAWITEMSTRUCT& item) {
    HDC dc=item.hDC;RECT rect=item.rcItem;
    POINT cursor;GetCursorPos(&cursor);ScreenToClient(item.hwndItem,&cursor);
    bool hover=PtInRect(&rect,cursor)!=FALSE,pressed=(item.itemState&ODS_SELECTED)!=0;
    bool caption=item.CtlID>=10;
    bool toggle=item.CtlID==4||item.CtlID==2;
    bool send=item.CtlID==5,clearAttachments=item.CtlID==6;
    bool modal=GetPropW(GetParent(item.hwndItem),L"ModalOriginalProc")!=nullptr;
    COLORREF background=(send||clearAttachments)?Surface:toggle?Background:caption?(modal?ModalHeader:RGB(28,28,28)):Panel;
    if(hover||pressed)background=item.CtlID==12?RGB(196,43,28):(pressed?RGB(62,62,62):RGB(47,47,47));
    if((send||clearAttachments)&&!hover&&!pressed&&composerBackdropBrush) {
        AlignComposerBrush(dc,item.hwndItem);FillRect(dc,&rect,composerBackdropBrush);
    }else Fill(dc,rect,background);
    if(clearAttachments) {
        Text(dc,L"\uf659",rect,captionIconFont,Ink,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    } else if(send) {
        if(busy) {
            RECT stop{Px(10),Px(10),rect.right-Px(10),rect.bottom-Px(10)};Fill(dc,stop,Ink);
        } else {
            // Right-pointing paper plane, drawn at the current DPI.
            POINT plane[]{{Px(7),Px(6)},{Px(26),Px(16)},{Px(7),Px(26)},{Px(11),Px(16)}};
            auto brush=CreateSolidBrush(Ink);auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
            Polygon(dc,plane,4);SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
        }
    } else if(toggle) {
        Text(dc,item.CtlID==2?L"\uf3e5":L"\uf45d",rect,navIconFont,Ink,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    } else if(!caption) {
        RECT icon=sidebarWide?RECT{Px(Space),0,Px(Space+20),rect.bottom}:rect;
        if(item.CtlID==8) {
            int x=(icon.left+icon.right)/2,y=(icon.top+icon.bottom)/2;
            POINT folder[]{{x-Px(10),y-Px(7)},{x-Px(3),y-Px(7)},{x,y-Px(4)},{x+Px(10),y-Px(4)},{x+Px(10),y+Px(7)},{x-Px(10),y+Px(7)},{x-Px(10),y-Px(7)}};
            auto pen=CreatePen(PS_SOLID,std::max(1,Px(1)),Ink);auto oldPen=SelectObject(dc,pen);Polyline(dc,folder,7);SelectObject(dc,oldPen);DeleteObject(pen);
            Line(dc,x-Px(10),y-Px(1),x+Px(10),y-Px(1),Ink,std::max(1,Px(1)));
        } else Text(dc,item.CtlID==9?L"\uf264":L"\uf64d",icon,navIconFont,Ink,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
        if(sidebarWide)Text(dc,item.CtlID==8?L"Open workspace":L"New chat",{Px(Space+32),0,rect.right-Px(Space),rect.bottom},bodyFont,Ink,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        Line(dc,rect.left,rect.bottom-1,rect.right,rect.bottom-1,Border);
        Line(dc,rect.right-1,rect.top,rect.right-1,rect.bottom,Border);
    } else {
        const wchar_t* glyph=item.CtlID==10?L"\uf63b":item.CtlID==12?L"\uf659":IsZoomed(app)?L"\uf759":L"\uf584";
        Text(dc,glyph,rect,captionIconFont,Ink,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    }
    if(caption)Line(dc,rect.left,rect.bottom-1,rect.right,rect.bottom-1,Border);
    if(!toggle&&(item.itemState&ODS_FOCUS)){RECT focus=rect;InflateRect(&focus,-Px(4),-Px(4));DrawFocusRect(dc,&focus);}
}
LRESULT CALLBACK ModalChromeProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto original=reinterpret_cast<WNDPROC>(GetPropW(window,L"ModalOriginalProc"));
    auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
    switch(message) {
    case WM_NCCALCSIZE:return 0;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(window,&point);RECT rect{};GetClientRect(window,&rect);
        return point.y>=0&&point.y<scale(Header)&&point.x<rect.right-scale(Header)?HTCAPTION:HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:if(wParam==HTCAPTION)return 0;break;
    case WM_COMMAND:if(LOWORD(wParam)==12&&HIWORD(wParam)==BN_CLICKED){SendMessageW(window,WM_CLOSE,0,0);return 0;}break;
    case WM_DRAWITEM:if(wParam==12){DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));return TRUE;}break;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);
        Fill(dc,rect,ModalBody);Fill(dc,{0,0,rect.right,scale(Header)},ModalHeader);
        wchar_t title[128]{};GetWindowTextW(window,title,128);
        Text(dc,title,{scale(Space),0,rect.right-scale(Header+16),scale(Header)},labelFont,Ink,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
        Line(dc,0,scale(Header)-1,rect.right,scale(Header)-1);
        EndPaint(window,&paint);return 0;
    }
    case WM_NCDESTROY: {
        auto result=CallWindowProcW(original,window,message,wParam,lParam);RemovePropW(window,L"ModalOriginalProc");return result;
    }
    }
    return CallWindowProcW(original,window,message,wParam,lParam);
}
void InitModalChrome(HWND window,const wchar_t* title,int contentWidth,int contentHeight) {
    SetWindowTextW(window,title);
    auto original=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
    SetPropW(window,L"ModalOriginalProc",reinterpret_cast<HANDLE>(original));
    SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ModalChromeProc));
    auto style=GetWindowLongPtrW(window,GWL_STYLE);
    SetWindowLongPtrW(window,GWL_STYLE,(style&~(WS_CAPTION|WS_DLGFRAME|WS_BORDER))|WS_CLIPCHILDREN);
    SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)&~WS_EX_DLGMODALFRAME);
    BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
    RECT owner{};GetWindowRect(GetParent(window),&owner);
    int width=scale(contentWidth),height=scale(contentHeight+Header);
    SetWindowPos(window,nullptr,owner.left+(owner.right-owner.left-width)/2,owner.top+(owner.bottom-owner.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    auto close=CreateWindowW(L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,width-scale(Header),0,scale(Header),scale(Header),window,reinterpret_cast<HMENU>(12),GetModuleHandleW(nullptr),nullptr);
    SetWindowLongPtrW(close,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ButtonProc));
}
INT_PTR CALLBACK ProviderErrorProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_INITDIALOG) {
        InitModalChrome(window,L"Provider error - response details",720,460);
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        auto text=reinterpret_cast<const std::wstring*>(lParam);
        auto edit=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_NOHIDESEL,
            scale(20),scale(Header+20),scale(680),scale(400),window,reinterpret_cast<HMENU>(201),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(codeFont),TRUE);SendMessageW(edit,EM_SETLIMITTEXT,131072,0);
        SetWindowTextW(edit,text->c_str());SetWindowSubclass(edit,SettingsEditProc,1,0);SetFocus(edit);return FALSE;
    }
    if(message==WM_CLOSE||(message==WM_COMMAND&&(LOWORD(wParam)==IDCANCEL||LOWORD(wParam)==IDOK))){EndDialog(window,IDCANCEL);return TRUE;}
    if(message==WM_CTLCOLORDLG)return reinterpret_cast<INT_PTR>(surfaceBrush);
    if(message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT){auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Background);SetDCBrushColor(dc,Background);return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));}
    return FALSE;
}
void ShowProviderError(HWND owner,const std::wstring& error) {
    if(error.empty())return;
    struct DialogTemplate {DLGTEMPLATE dialog;WORD menu,windowClass,title;} dialog{};
    dialog.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;dialog.dialog.cx=480;dialog.dialog.cy=300;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&dialog.dialog,owner,ProviderErrorProc,reinterpret_cast<LPARAM>(&error));
}
struct ModalMessage {const wchar_t* text;const wchar_t* title;UINT type;};
INT_PTR CALLBACK ModalMessageProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto data=reinterpret_cast<ModalMessage*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        data=reinterpret_cast<ModalMessage*>(lParam);SetWindowLongPtrW(window,DWLP_USER,lParam);
        auto scale=[&](int n){return MulDiv(n,GetDpiForWindow(window),96);};
        HDC dc=GetDC(window);SelectObject(dc,bodyFont);RECT measured{0,0,scale(440),0};
        DrawTextW(dc,data->text,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);ReleaseDC(window,dc);
        int textHeight=MulDiv(measured.bottom,96,GetDpiForWindow(window));
        int contentHeight=std::max(140,textHeight+100);InitModalChrome(window,data->title,488,contentHeight);
        auto label=CreateWindowW(L"STATIC",data->text,WS_CHILD|WS_VISIBLE,scale(24),scale(Header+24),scale(440),scale(textHeight+4),window,reinterpret_cast<HMENU>(301),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);
        auto button=[&](const wchar_t* title,int id,int x,DWORD style) {
            auto child=CreateWindowW(L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,scale(x),scale(Header+contentHeight-58),scale(100),scale(34),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);return child;
        };
        HWND focus;
        if((data->type&MB_TYPEMASK)==MB_YESNO){focus=button(L"Yes",IDYES,254,BS_DEFPUSHBUTTON);button(L"No",IDNO,364,0);}
        else focus=button(L"OK",IDOK,364,BS_DEFPUSHBUTTON);
        SetFocus(focus);return FALSE;
    }
    if(message==WM_COMMAND) {
        int id=LOWORD(wParam);
        if(id==IDOK||id==IDYES||id==IDNO){EndDialog(window,id);return TRUE;}
        if(id==IDCANCEL){EndDialog(window,(data->type&MB_TYPEMASK)==MB_YESNO?IDNO:IDOK);return TRUE;}
    }
    if(message==WM_CLOSE){EndDialog(window,(data->type&MB_TYPEMASK)==MB_YESNO?IDNO:IDOK);return TRUE;}
    if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC) {
        auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);return reinterpret_cast<INT_PTR>(surfaceBrush);
    }
    return FALSE;
}
int ShowModalMessage(HWND owner,const wchar_t* text,const wchar_t* title,UINT type) {
    struct DialogTemplate {DLGTEMPLATE dialog;WORD menu,windowClass,title;} dialog{};
    dialog.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;dialog.dialog.cx=326;dialog.dialog.cy=100;
    ModalMessage data{text,title,type};
    return static_cast<int>(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&dialog.dialog,owner,ModalMessageProc,reinterpret_cast<LPARAM>(&data)));
}
}
RECT ConversationThumb(HWND window) {
    RECT rect{};GetClientRect(window,&rect);
    int height=std::max(0L,rect.bottom),extent=std::max(1,conversationExtent);
    int thumb=std::min(height,std::max(Px(28),static_cast<int>(static_cast<long long>(height)*conversationPage/extent)));
    int maximum=std::max(0,conversationExtent-conversationPage);
    int top=maximum?static_cast<int>(static_cast<long long>(std::clamp(scroll,0,maximum))*(height-thumb)/maximum):0;
    return {Px(4),top,std::max(Px(4),static_cast<int>(rect.right)-Px(4)),top+thumb};
}
void SetConversationScroll(int position) {
    scroll=std::clamp(position,0,std::max(0,conversationExtent-conversationPage));
    // Paint the thumb immediately; conversation layout must not delay pointer feedback.
    InvalidateRect(conversationScroll,nullptr,FALSE);
    UpdateWindow(conversationScroll);
    InvalidateRect(app,&layout.conversation,FALSE);
}
void PageConversationScroll(HWND window) {
    POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);auto thumb=ConversationThumb(window);
    if((scrollRepeatDirection<0&&point.y<thumb.top)||(scrollRepeatDirection>0&&point.y>=thumb.bottom))
        SetConversationScroll(scroll+scrollRepeatDirection*conversationPage);
}
LRESULT CALLBACK ConversationScrollProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};auto target=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);
        if(rect.right>0&&rect.bottom>0) {
            auto dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,rect.right,rect.bottom);auto old=SelectObject(dc,bitmap);
            Fill(dc,rect,Background);auto thumb=ConversationThumb(window);
            Round(dc,thumb,scrollDragging?Muted:scrollHovered?RGB(112,112,112):RGB(80,80,80),Px(6));
            BitBlt(target,0,0,rect.right,rect.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
        }
        EndPaint(window,&paint);return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(window);SetCapture(window);auto thumb=ConversationThumb(window);int y=GET_Y_LPARAM(lParam);
        if(y>=thumb.top&&y<thumb.bottom){scrollDragging=true;scrollDragOffset=y-thumb.top;}
        else {scrollRepeatDirection=y<thumb.top?-1:1;PageConversationScroll(window);SetTimer(window,1,350,nullptr);}
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_MOUSEMOVE: {
        if(!scrollHovered){scrollHovered=true;TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window,0};TrackMouseEvent(&tracking);InvalidateRect(window,nullptr,FALSE);}
        if(scrollDragging) {
            RECT rect{};GetClientRect(window,&rect);auto thumb=ConversationThumb(window);
            int travel=rect.bottom-(thumb.bottom-thumb.top),maximum=std::max(0,conversationExtent-conversationPage);
            if(travel>0)SetConversationScroll(static_cast<int>(static_cast<long long>(std::clamp(GET_Y_LPARAM(lParam)-scrollDragOffset,0,travel))*maximum/travel));
        }
        return 0;
    }
    case WM_MOUSELEAVE:scrollHovered=false;InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_LBUTTONUP:if(GetCapture()==window)ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:scrollDragging=false;scrollRepeatDirection=0;KillTimer(window,1);InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_TIMER:if(wParam==1){PageConversationScroll(window);SetTimer(window,1,60,nullptr);return 0;}break;
    case WM_MOUSEWHEEL:SetConversationScroll(scroll-GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA*Px(64));return 0;
    case WM_KEYDOWN:
        switch(wParam) {
        case VK_UP:SetConversationScroll(scroll-Px(32));return 0;case VK_DOWN:SetConversationScroll(scroll+Px(32));return 0;
        case VK_PRIOR:SetConversationScroll(scroll-conversationPage);return 0;case VK_NEXT:SetConversationScroll(scroll+conversationPage);return 0;
        case VK_HOME:SetConversationScroll(0);return 0;case VK_END:SetConversationScroll(conversationExtent);return 0;
        }break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
LRESULT CALLBACK TranscriptProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto edit=GetDlgItem(window,1);
    if(message==WM_CREATE) {
        edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_NOHIDESEL,0,0,0,0,window,reinterpret_cast<HMENU>(1),GetModuleHandleW(nullptr),nullptr);
        SetWindowSubclass(edit,[](HWND control,UINT msg,WPARAM key,LPARAM data,UINT_PTR,DWORD_PTR)->LRESULT {
            if(msg==WM_KEYDOWN&&key=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(control,EM_SETSEL,0,-1);return 0;}
            return DefSubclassProc(control,msg,key,data);
        },1,0);
        SendMessageW(edit,EM_SETLIMITTEXT,0x7ffffffe,0);
        SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);
        std::wstring text;
        for(const auto& item:messages) {
            text+=item.role+L"\r\n";
            for(wchar_t c:item.text){if(c==L'\n'&&(text.empty()||text.back()!=L'\r'))text+=L'\r';text+=c;}
            if(!item.activity.empty())text+=L"\r\n"+item.activity;
            text+=L"\r\n\r\n";
        }
        SetWindowTextW(edit,text.c_str());return 0;
    }
    if(message==WM_SIZE){MoveWindow(edit,0,0,LOWORD(lParam),HIWORD(lParam),TRUE);return 0;}
    if(message==WM_SETFOCUS){SetFocus(edit);return 0;}
    if(message==WM_COMMAND&&LOWORD(wParam)==2){SendMessageW(edit,EM_SETSEL,0,-1);return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
void OpenTranscript() {
    WNDCLASSW cls{};cls.lpfnWndProc=TranscriptProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"ChatSelectableTranscript";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
    auto window=CreateWindowExW(0,cls.lpszClassName,L"Chat history ? select text and press Ctrl+C",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,Px(800),Px(600),app,nullptr,cls.hInstance,nullptr);
    auto menu=CreateMenu();AppendMenuW(menu,MF_STRING,2,L"Select all");SetMenu(window,menu);SetFocus(window);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_NCCALCSIZE:
        if(wParam){auto params=reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);if(IsZoomed(window)){MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);params->rgrc[0]=monitor.rcWork;}return 0;}
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(window,&point);RECT rect;GetClientRect(window,&rect);
        if(!IsZoomed(window)) {
            int edge=Px(6);bool left=point.x<edge,right=point.x>=rect.right-edge,top=point.y<edge,bottom=point.y>=rect.bottom-edge;
            if(top)return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
            if(bottom)return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
            if(left)return HTLEFT;if(right)return HTRIGHT;
        }
        if(point.y<Px(Header)&&point.x<rect.right-Px(Header)*3)return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CREATE: {
        app=window;dpi=GetDpiForWindow(window);auto instance=reinterpret_cast<CREATESTRUCTW*>(lParam)->hInstance;
        BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        try {worker=std::make_unique<chat::ChatWorker>(configDirectory,[window](chat::Result result){auto value=std::make_unique<chat::Result>(std::move(result));if(PostMessageW(window,WM_APP+1,0,reinterpret_cast<LPARAM>(value.get())))value.release();});}
        catch(...){return -1;}
        auto resource=FindResourceW(instance,MAKEINTRESOURCEW(101),RT_RCDATA);
        auto loaded=resource?LoadResource(instance,resource):nullptr;DWORD fonts=0;
        if(loaded)iconFontResource=AddFontMemResourceEx(LockResource(loaded),SizeofResource(instance,resource),nullptr,&fonts);
        if(!iconFontResource)return -1;
        surfaceBrush=CreateSolidBrush(Surface);CreateFonts();
        auto button=[&](const wchar_t* name,int id){HWND control=CreateWindowW(L"BUTTON",name,WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);auto original=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(control,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ButtonProc)));if(!oldButtonProc)oldButtonProc=original;return control;};
        newButton=button(L"New chat",1);settingsButton=button(L"Settings",2);sidebarToggle=button(L"Collapse sidebar",4);sendButton=button(L"Send message",5);minButton=button(L"Minimize",10);maxButton=button(L"Restore",11);closeButton=button(L"Close",12);
        WNDCLASSW scrollClass{};scrollClass.lpfnWndProc=ConversationScrollProc;scrollClass.hInstance=instance;
        scrollClass.lpszClassName=L"ChatDarkScroll";scrollClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&scrollClass);
        conversationScroll=CreateWindowW(scrollClass.lpszClassName,L"Conversation scrollbar",WS_CHILD|WS_TABSTOP,0,0,0,0,window,reinterpret_cast<HMENU>(13),instance,nullptr);
        historyList=CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,0,0,0,0,window,reinterpret_cast<HMENU>(7),instance,nullptr);
        SetWindowSubclass(historyList,HistoryListProc,1,0);
        SendMessageW(historyList,LB_SETITEMHEIGHT,0,Px(36));
        historyButton=button(L"Chat history",9);
        workspaceButton=button(L"Open workspace",8);
        clearAttachmentsButton=button(L"Remove attachments",6);ShowWindow(clearAttachmentsButton,SW_HIDE);
        INITCOMMONCONTROLSEX commonControls{sizeof(commonControls),ICC_WIN95_CLASSES};InitCommonControlsEx(&commonControls);
        navTooltips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,instance,nullptr);
        for(auto control:{newButton,historyButton,workspaceButton,settingsButton,sidebarToggle,sendButton,clearAttachmentsButton}) {
            TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(control);tool.lpszText=LPSTR_TEXTCALLBACKW;
            SendMessageW(navTooltips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        }
        input=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|ES_NOHIDESEL,0,0,0,0,window,reinterpret_cast<HMENU>(3),instance,nullptr);
        SendMessageW(input,WM_SETFONT,reinterpret_cast<WPARAM>(bodyFont),TRUE);SendMessageW(input,EM_SETLIMITTEXT,12000,0);
        SendMessageW(input,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
        oldInputProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(input,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(InputProc)));
        try {
            savedHistory=chat::LoadChatHistory(configDirectory);
            for(const auto& item:savedHistory.chats)if(item.id==savedHistory.activeId){messages=item.messages;apiHistory=item.context;restoringChat=true;SetWindowTextW(input,item.draft.c_str());restoringChat=false;chat::ChatWorkspace(configDirectory,item.id);break;}
            PersistChat();scroll=1000000;
        }catch(const std::exception& error){historyWritable=false;historyError=chat::Wide(error.what());PostMessageW(window,WM_APP+2,0,0);PersistChat();}
        RefreshChats();DragAcceptFiles(window,TRUE);DragAcceptFiles(input,TRUE);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi=HIWORD(wParam);CreateFonts();auto rect=reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);Layout();return 0;
    }
    case WM_SIZE:Layout();return 0;
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize={Px(360),Px(540)};return 0;
    case WM_COMMAND:
        if(LOWORD(wParam)==9&&HIWORD(wParam)==BN_CLICKED){
            auto menu=CreatePopupMenu();
            AppendMenuW(menu,MF_STRING,999,L"Open workspace");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            for(size_t i=0;i<savedHistory.chats.size();++i){auto label=savedHistory.chats[i].title;size_t at=0;while((at=label.find(L'&',at))!=label.npos){label.insert(at,1,L'&');at+=2;}AppendMenuW(menu,MF_STRING|(savedHistory.chats[i].id==savedHistory.activeId?MF_CHECKED:0),1000+i,label.c_str());}
            RECT rect{};GetWindowRect(historyButton,&rect);auto choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,rect.right,rect.top,0,window,nullptr);DestroyMenu(menu);
            if(choice==999)SendMessageW(window,WM_COMMAND,8,0);else if(choice>=1000)OpenChat(choice-1000);return 0;
        }
        if(LOWORD(wParam)==7&&HIWORD(wParam)==LBN_SELCHANGE){auto index=SendMessageW(historyList,LB_GETCURSEL,0,0);if(index!=LB_ERR)OpenChat(static_cast<size_t>(index));return 0;}
        if(LOWORD(wParam)==8&&HIWORD(wParam)==BN_CLICKED){try{auto path=chat::ChatWorkspace(configDirectory,savedHistory.activeId);ShellExecuteW(window,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}catch(const std::exception& error){ShowModalMessage(window,chat::Wide(error.what()).c_str(),L"Workspace",MB_OK|MB_ICONERROR);}return 0;}
        if(LOWORD(wParam)==6&&HIWORD(wParam)==BN_CLICKED){pendingAttachments.clear();Layout();SetFocus(input);return 0;}
        if(LOWORD(wParam)==4){sidebarExpanded=!sidebarExpanded;Layout();return 0;}
        if(HIWORD(wParam)==BN_CLICKED){switch(LOWORD(wParam)){case 1:Reset();break;case 2:OpenSettings();break;case 5:if(busy)StopAgent();else Send();break;case 10:ShowWindow(window,SW_MINIMIZE);break;case 11:ShowWindow(window,IsZoomed(window)?SW_RESTORE:SW_MAXIMIZE);break;case 12:PostMessageW(window,WM_CLOSE,0,0);break;}}
        if(HIWORD(wParam)==EN_CHANGE&&LOWORD(wParam)==3){RedrawWindow(input,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE);if(!recallingPrompt){recalledPrompt.reset();promptRecallDraft.clear();}Layout(true);if(!restoringChat)SetTimer(window,9,700,nullptr);}
        return 0;
    case WM_NOTIFY: {
        auto notification=reinterpret_cast<NMHDR*>(lParam);
        if(notification->hwndFrom==navTooltips&&notification->code==TTN_GETDISPINFOW) {
            auto info=reinterpret_cast<NMTTDISPINFOW*>(lParam);
            auto control=reinterpret_cast<HWND>(info->hdr.idFrom);
            info->lpszText=const_cast<wchar_t*>(control==clearAttachmentsButton?L"Remove attachments":control==sendButton?(busy?L"Stop agent":L"Send message"):control==sidebarToggle?(sidebarWide?L"Collapse sidebar (Ctrl+B)":L"Expand sidebar (Ctrl+B)"):control==settingsButton?L"Settings":control==historyButton?L"Chat history":control==workspaceButton?L"Open workspace":L"New chat");
            return 0;
        }
        break;
    }
    case WM_DROPFILES: {
        auto drop=reinterpret_cast<HDROP>(wParam);
        struct DropGuard {HDROP value;~DropGuard(){DragFinish(value);}} guard{drop};
        std::wstring errors;
        UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
        for(UINT i=0;i<count;++i) {
            UINT length=DragQueryFileW(drop,i,nullptr,0);std::wstring path(length+1,L'\0');
            DragQueryFileW(drop,i,path.data(),length+1);path.resize(length);
            try {
                if(pendingAttachments.size()>=8)throw std::runtime_error("Attach up to 8 files per message.");
                auto attachment=chat::LoadAttachment(path);
                size_t total=attachment.bytes;
                for(const auto& item:pendingAttachments)total+=item.bytes;
                for(const auto& historyMessage:apiHistory)for(const auto& item:historyMessage.attachments)total+=item.bytes;
                if(total>32*1024*1024)throw std::runtime_error("Attachment context is limited to 32 MiB per conversation. Start a new chat or remove pending attachments.");
                pendingAttachments.push_back(std::move(attachment));
            } catch(const std::exception& error) {errors+=std::filesystem::path(path).filename().wstring()+L": "+chat::Wide(error.what())+L"\n";}
        }
        Layout();SetFocus(input);
        if(!errors.empty())ShowModalMessage(window,errors.c_str(),L"Attachments",MB_OK|MB_ICONINFORMATION);
        return 0;
    }
    case WM_APP+1: {
        std::unique_ptr<chat::Result> result(reinterpret_cast<chat::Result*>(lParam));
        if(result->generation!=generation)return 0;
        if(!result->final&&result->kind==chat::UpdateKind::Status) {
            if(currentPrompt)messages[*currentPrompt].activity=result->reply.text;
        } else if(!result->final&&result->kind==chat::UpdateKind::Tool) {
            messages.push_back({L"tool_display",std::move(result->reply.text)});
        } else if(!result->final&&result->kind==chat::UpdateKind::SegmentEnd) {
            if(streamingMessage)messages[*streamingMessage]={L"assistant",std::move(result->reply.text)};
            else if(!result->reply.text.empty())messages.push_back({L"assistant",std::move(result->reply.text)});
            streamingMessage.reset();
        } else
        if(!result->final) {
            if(!streamingMessage){streamingMessage=messages.size();messages.push_back({L"streaming",{}});}
            messages[*streamingMessage].text+=result->reply.text;
        } else {
            busy=false;
            SetWindowTextW(sendButton,L"Send message");InvalidateRect(sendButton,nullptr,TRUE);
            if(currentPrompt)messages[*currentPrompt].activity=chat::CallActivity(result->reply.metrics,result->reply.ok?L"Complete":L"Failed");
            if(!result->history.empty())apiHistory=std::move(result->history);
            if(result->reply.ok&&streamingMessage)messages[*streamingMessage]={L"assistant",std::move(result->reply.text)};
            else {
                if(streamingMessage)messages[*streamingMessage].role=L"partial";
                messages.push_back({result->reply.ok?L"assistant":L"error",std::move(result->reply.text)});
            }
            streamingMessage.reset();
            currentPrompt.reset();PersistChat();
        }
        scroll=1000000;InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_APP+2:if(!historyError.empty()){auto error=std::move(historyError);historyError.clear();ShowModalMessage(window,error.c_str(),L"Chat history",MB_OK|MB_ICONERROR);}return 0;
    case WM_TIMER:
        if(wParam==9){KillTimer(window,9);PersistChat();return 0;}
        if(wParam==10&&dragText) {
            POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);
            if(point.y<layout.conversation.top+Px(16)||point.y>layout.composer.top-Px(16)) {
                SetConversationScroll(scroll+(point.y<layout.conversation.top+Px(16)?-Px(24):Px(24)));
                UpdateWindow(window);point.y=std::clamp<LONG>(point.y,layout.conversation.top,layout.composer.top-1);
                textEnd=HitChatText(point);InvalidateRect(window,&layout.conversation,FALSE);
            }
            return 0;
        }break;
    case WM_KEYDOWN:
        if(wParam=='C'&&(GetKeyState(VK_CONTROL)&0x8000)){UpdateWindow(window);CopyChatSelection();return 0;}
        if(wParam=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){textAnchor={0,0};textEnd={messages.size(),0};InvalidateRect(window,&layout.conversation,FALSE);return 0;}
        if(wParam==VK_ESCAPE){textAnchor=textEnd={};InvalidateRect(window,&layout.conversation,FALSE);return 0;}
        break;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if(PtInRect(&layout.conversation,point)&&!PtInRect(&layout.composer,point)) {
            SetFocus(window);textAnchor=textEnd=HitChatText(point);dragText=true;SetCapture(window);SetTimer(window,10,40,nullptr);
            InvalidateRect(window,&layout.conversation,FALSE);return 0;
        }break;
    }
    case WM_MOUSEMOVE:
        if(dragText){textEnd=HitChatText({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});InvalidateRect(window,&layout.conversation,FALSE);return 0;}break;
    case WM_CAPTURECHANGED:dragText=false;KillTimer(window,10);break;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};auto client=point;ScreenToClient(window,&client);
        if(PtInRect(&layout.conversation,client)&&!PtInRect(&layout.composer,client)) {
            UpdateWindow(window);auto menu=CreatePopupMenu();if(HasTextSelection())AppendMenuW(menu,MF_STRING,2,L"Copy\tCtrl+C");AppendMenuW(menu,MF_STRING,1,L"Select/copy chat text...");
            auto choice=TrackPopupMenu(menu,TPM_RETURNCMD,point.x,point.y,0,window,nullptr);DestroyMenu(menu);
            if(choice==1)OpenTranscript();else if(choice==2)CopyChatSelection();return 0;
        }break;
    }
    case WM_MOUSEWHEEL:SetConversationScroll(scroll-GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA*Px(64));return 0;
    case WM_LBUTTONUP: {
        if(dragText){textEnd=HitChatText({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});dragText=false;ReleaseCapture();InvalidateRect(window,&layout.conversation,FALSE);if(HasTextSelection())return 0;}
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if(PtInRect(&layout.conversation,point)&&!PtInRect(&layout.composer,point))for(const auto& link:responseLinks)if(PtInRect(&link.rect,point)){ShellExecuteW(window,L"open",link.href.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return 0;}
        break;
    }
    case WM_SETCURSOR: {
        POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);
        if(LOWORD(lParam)==HTCLIENT&&PtInRect(&layout.conversation,point)&&!PtInRect(&layout.composer,point))for(const auto& link:responseLinks)if(PtInRect(&link.rect,point)){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        if(LOWORD(lParam)==HTCLIENT&&PtInRect(&layout.conversation,point)&&!PtInRect(&layout.composer,point)){SetCursor(LoadCursorW(nullptr,IDC_IBEAM));return TRUE;}
        break;
    }
    case WM_CTLCOLORLISTBOX:SetTextColor(reinterpret_cast<HDC>(wParam),Ink);SetBkColor(reinterpret_cast<HDC>(wParam),Background);SetDCBrushColor(reinterpret_cast<HDC>(wParam),Background);return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_CTLCOLOREDIT: {
        auto dc=reinterpret_cast<HDC>(wParam);SetTextColor(dc,Ink);SetBkColor(dc,Surface);
        if(reinterpret_cast<HWND>(lParam)==input&&composerBackdropBrush){SetBkMode(dc,TRANSPARENT);AlignComposerBrush(dc,input);return reinterpret_cast<LRESULT>(composerBackdropBrush);}
        return reinterpret_cast<LRESULT>(surfaceBrush);
    }
    case WM_DRAWITEM: {
        auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if(wParam==7){
            bool hovered=static_cast<int>(item.itemID)==hoveredChat;
            Fill(item.hDC,item.rcItem,(item.itemState&ODS_SELECTED)?Surface:hovered?Panel:Background);
            if(item.itemID<savedHistory.chats.size()) {
                auto rect=item.rcItem;rect.left+=Px(12);rect.right-=Px(hovered?40:8);
                Text(item.hDC,savedHistory.chats[item.itemID].title,rect,bodyFont,Ink,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
                if(hovered) {
                    auto button=ChatDeleteRect(item.rcItem);POINT cursor{};GetCursorPos(&cursor);ScreenToClient(historyList,&cursor);
                    auto color=PtInRect(&button,cursor)?RGB(244,161,148):Muted;
                    int x=(button.left+button.right)/2,y=(button.top+button.bottom)/2;
                    Line(item.hDC,x-Px(6),y-Px(5),x+Px(6),y-Px(5),color);
                    Line(item.hDC,x-Px(4),y-Px(3),x-Px(4),y+Px(6),color);Line(item.hDC,x+Px(4),y-Px(3),x+Px(4),y+Px(6),color);
                    Line(item.hDC,x-Px(4),y+Px(6),x+Px(4),y+Px(6),color);
                    Line(item.hDC,x-Px(2),y-Px(7),x+Px(2),y-Px(7),color);
                    Line(item.hDC,x-Px(1),y-Px(2),x-Px(1),y+Px(3),color);Line(item.hDC,x+Px(1),y-Px(2),x+Px(1),y+Px(3),color);
                }
            }
            return TRUE;
        }
        DrawButton(item);return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT rect;GetClientRect(window,&rect);
        if(rect.right>0&&rect.bottom>0){HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,rect.right,rect.bottom);auto old=SelectObject(buffer,bitmap);Paint(buffer);BitBlt(dc,0,0,rect.right,rect.bottom,buffer,0,0,SRCCOPY);SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);}EndPaint(window,&paint);return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_CLOSE:
        try{chat::SaveWindowState(configDirectory,window);}catch(const std::exception& error){ShowModalMessage(window,chat::Wide(error.what()).c_str(),L"Window position",MB_OK|MB_ICONERROR);}
        break;
    case WM_QUERYENDSESSION:try{chat::SaveWindowState(configDirectory,window);}catch(...){}return TRUE;
    case WM_DESTROY:
        KillTimer(window,9);if(busy)StopAgent();else PersistChat();
        if(worker){worker->Stop();worker.reset();}
        {MSG pending;while(PeekMessageW(&pending,window,WM_APP+1,WM_APP+1,PM_REMOVE))delete reinterpret_cast<chat::Result*>(pending.lParam);}
        for(auto font:{bodyFont,labelFont,headingFont,smallFont,codeFont,navIconFont,captionIconFont})if(font)DeleteObject(font);
        for(const auto& font:markdownFonts)DeleteObject(font.second);markdownFonts.clear();
        if(iconFontResource)RemoveFontMemResourceEx(iconFontResource);
        if(composerBackdropBrush)DeleteObject(composerBackdropBrush);
        DeleteObject(surfaceBrush);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);configDirectory=std::filesystem::path(path).parent_path();
    if(!std::filesystem::exists(configDirectory/L"provider-settings.json")&&!std::filesystem::exists(configDirectory/L".env")&&(std::filesystem::exists(configDirectory.parent_path()/L".env")||std::filesystem::exists(configDirectory.parent_path()/L"provider-settings.json")))configDirectory=configDirectory.parent_path();
    WNDCLASSW windowClass{};windowClass.lpfnWndProc=WindowProc;windowClass.hInstance=instance;windowClass.lpszClassName=L"ChatHarness";windowClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    windowClass.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassW(&windowClass);
    auto savedWindow=chat::LoadWindowState(configDirectory);
    RECT bounds=savedWindow?savedWindow->normal:RECT{CW_USEDEFAULT,CW_USEDEFAULT,0,0};
    HWND window=CreateWindowW(windowClass.lpszClassName,L"",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,bounds.left,bounds.top,savedWindow?bounds.right-bounds.left:1120,savedWindow?bounds.bottom-bounds.top:820,nullptr,nullptr,instance,nullptr);
    if(!window)return 1;
    auto smallIcon=LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED);
    SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(smallIcon));
    if(savedWindow){auto rect=chat::VisibleWindowRect(savedWindow->normal,GetDpiForWindow(window));SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);}
    int showCommand=!savedWindow||savedWindow->maximized?SW_SHOWMAXIMIZED:SW_SHOWNORMAL;
    ShowWindow(window,showCommand);
    // A launcher's initial hidden state must not hide the restored interactive window.
    if(!IsWindowVisible(window))ShowWindow(window,showCommand);
    SetFocus(input);
    ACCEL toggle{FVIRTKEY|FCONTROL,'B',4};auto shortcuts=CreateAcceleratorTableW(&toggle,1);
    MSG message;BOOL status;while((status=GetMessageW(&message,nullptr,0,0))>0){if(!TranslateAcceleratorW(window,shortcuts,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(shortcuts)DestroyAcceleratorTable(shortcuts);return status==-1?1:0;
}

