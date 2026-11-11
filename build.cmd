@echo off
setlocal
cd /d "%~dp0"

if defined VSCMD_VER goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: Visual Studio Installer was not found. Install Visual Studio with Desktop development with C++.
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo Error: MSVC C++ build tools were not found. Install Desktop development with C++.
    exit /b 1
)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

:build
if not exist build mkdir build
cl /nologo /w /TC /c /DMD4C_USE_UTF16 third_party\md4c\md4c.c /Fobuild\md4c.obj
if errorlevel 1 exit /b %errorlevel%
cl /nologo /w /TC /c third_party\md4c\entity.c /Fobuild\entity.obj
if errorlevel 1 exit /b %errorlevel%
if /i "%~1"=="test" goto test
rc /nologo /fo build\icons.res icons.rc
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE main.cpp markdown.cpp config.cpp chat_client.cpp stream_decoder.cpp powershell_tool.cpp mcp_client.cpp agent.cpp chat_worker.cpp /Fo:build\ /Fe:build\chat.exe /link build\icons.res build\md4c.obj build\entity.obj /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" user32.lib gdi32.lib winhttp.lib dwmapi.lib
exit /b %errorlevel%

:test
cl /nologo /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE tests.cpp markdown.cpp config.cpp chat_client.cpp stream_decoder.cpp powershell_tool.cpp mcp_client.cpp agent.cpp chat_worker.cpp /Fo:build\ /Fe:build\chat-tests.exe /link build\md4c.obj build\entity.obj /INCREMENTAL:NO winhttp.lib
if errorlevel 1 exit /b %errorlevel%
node tests\mock_server.cjs
exit /b %errorlevel%
