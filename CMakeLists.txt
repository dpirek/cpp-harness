cmake_minimum_required(VERSION 3.24)
project(cpp_harness VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "This application requires Windows and the MSVC toolchain.")
endif()
enable_language(RC)

add_library(md4c STATIC
    third_party/md4c/md4c.c
    third_party/md4c/entity.c
)
target_compile_definitions(md4c PRIVATE MD4C_USE_UTF16)
target_compile_options(md4c PRIVATE /w)

# Internal application library; headers live beside their implementations.
add_library(harness_core STATIC
    src/agent/agent.cpp src/agent/agent.h
    src/app/chat_worker.cpp src/app/chat_worker.h
    src/client/chat_client.cpp src/client/chat_client.h
    src/client/stream_decoder.cpp src/client/stream_decoder.h
    src/client/call_metrics.h
    src/common/attachments.h src/common/text_encoding.h src/common/tool_format.h
    src/config/config.cpp src/config/config.h
    src/tools/computer_tool.cpp src/tools/computer_tool.h
    src/tools/mcp_client.cpp src/tools/mcp_client.h
    src/tools/powershell_tool.cpp src/tools/powershell_tool.h
    src/ui/markdown.cpp src/ui/markdown.h src/ui/code_format.h
)
target_compile_features(harness_core PUBLIC cxx_std_17)
set_target_properties(harness_core PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(harness_core PUBLIC UNICODE _UNICODE)
target_include_directories(harness_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(harness_core SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_options(harness_core PRIVATE /W4 /EHsc)
target_link_libraries(harness_core PRIVATE md4c winhttp crypt32 user32 gdi32 gdiplus ole32)
get_target_property(core_sources harness_core SOURCES)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${core_sources})

add_executable(chat WIN32 src/app/main.cpp resources/icons.rc)
# RC resource filenames resolve against this directory, independent of build cwd.
target_include_directories(chat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(chat PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/EHsc>")
target_link_libraries(chat PRIVATE harness_core dwmapi shell32 comctl32)
target_link_options(chat PRIVATE /INCREMENTAL:NO /MANIFEST:EMBED
    "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

include(CTest)
if(BUILD_TESTING)
    add_executable(chat-tests tests/tests.cpp)
    target_compile_options(chat-tests PRIVATE /W4 /EHsc)
    target_link_libraries(chat-tests PRIVATE harness_core)
    find_program(NODE_EXECUTABLE NAMES node)
    if(NODE_EXECUTABLE)
        add_test(NAME harness-integration
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_server.cjs"
                --test-executable "$<TARGET_FILE:chat-tests>")
        set_tests_properties(harness-integration PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 45 RUN_SERIAL TRUE)
    else()
        message(WARNING "Node.js was not found; integration tests cannot run.")
    endif()
endif()
