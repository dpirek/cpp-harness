@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cl /nologo /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /c tests.cpp /Fobuild\tests.obj
if errorlevel 1 exit /b %errorlevel%
link /OUT:build\chat-tests.exe build\tests.obj build\markdown.obj build\config.obj build\chat_client.obj build\stream_decoder.obj build\powershell_tool.obj build\mcp_client.obj build\agent.obj build\chat_worker.obj build\icons.res build\md4c.obj build\entity.obj /SUBSYSTEM:CONSOLE /INCREMENTAL:NO user32.lib gdi32.lib winhttp.lib dwmapi.lib