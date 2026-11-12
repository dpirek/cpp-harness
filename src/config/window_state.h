#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace chat {
struct WindowState { RECT normal; bool maximized=false; };
inline RECT VisibleWindowRect(RECT rect,UINT dpi=96) {
    MONITORINFO monitor{sizeof(monitor)};
    if(!GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&monitor))return rect;
    auto work=monitor.rcWork;
    LONG width=std::clamp(rect.right-rect.left,std::min<LONG>(MulDiv(360,dpi,96),work.right-work.left),work.right-work.left);
    LONG height=std::clamp(rect.bottom-rect.top,std::min<LONG>(MulDiv(540,dpi,96),work.bottom-work.top),work.bottom-work.top);
    rect.left=std::clamp(rect.left,work.left,work.right-width);rect.top=std::clamp(rect.top,work.top,work.bottom-height);
    rect.right=rect.left+width;rect.bottom=rect.top+height;return rect;
}
inline std::optional<WindowState> LoadWindowState(const std::filesystem::path& directory) {
    try {
        auto path=directory/L"window-state.json";
        if(!std::filesystem::exists(path)||std::filesystem::file_size(path)>4096)return {};
        std::ifstream file(path,std::ios::binary);auto json=nlohmann::json::parse(file);
        if(json.at("version")!=1)return {};
        auto coordinate=[&](const char* name){const auto& value=json.at(name);if(!value.is_number_integer()||value < -1000000||value > 1000000)throw std::runtime_error("Invalid window bounds.");return value.get<LONG>();};
        RECT rect{coordinate("left"),coordinate("top"),coordinate("right"),coordinate("bottom")};
        if(rect.right<=rect.left||rect.bottom<=rect.top)return {};
        return WindowState{VisibleWindowRect(rect),json.at("maximized").get<bool>()};
    }catch(...){return {};}
}
inline void SaveWindowState(const std::filesystem::path& directory,HWND window) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if(!GetWindowPlacement(window,&placement))throw std::runtime_error("Could not read window position.");
    RECT rect=placement.rcNormalPosition;
    // WINDOWPLACEMENT uses workspace coordinates; save screen coordinates for restoration.
    MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))
        OffsetRect(&rect,monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top);
    bool maximized=placement.showCmd==SW_SHOWMAXIMIZED||((placement.showCmd==SW_SHOWMINIMIZED||placement.showCmd==SW_MINIMIZE)&&(placement.flags&WPF_RESTORETOMAXIMIZED));
    auto json=nlohmann::json{{"version",1},{"left",rect.left},{"top",rect.top},{"right",rect.right},{"bottom",rect.bottom},{"maximized",maximized}};
    auto temporary=directory/L"window-state.json.tmp",destination=directory/L"window-state.json";
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<json.dump(2);file.flush();if(!file)throw std::runtime_error("Could not save window position.");}
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not replace window position settings.");
}
}
