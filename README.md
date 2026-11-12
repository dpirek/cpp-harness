# Native agent chat harness

A Win32 C++ desktop chat app styled after `docs/image.png`. It launches maximized with a 48 DIP custom dark title bar, square window controls, and a dark sidebar, rounded user messages, and a rectangular prompt box with a Send icon button on the right. The prompt begins as one line, expands upward as text wraps or newlines are added, and scrolls internally after six lines. It shrinks again when text is removed. Enter or the Send button sends; Shift+Enter inserts a newline. While the agent runs, Send becomes Stop. Stop cancels the current operation, keeps partial text and your draft, and restores Send; completed filesystem changes remain. New chat saves the current conversation, cancels its active request, and opens a new conversation with its own workspace. Scroll with the mouse wheel.

## Build and run

Install Visual Studio's Desktop development with C++ workload:

```powershell
.\build.cmd
.\build\chat.exe
```

The app stays named `chat.exe` and has an embedded `asInvoker` manifest, so it runs with standard user permissions. Close the running app before rebuilding it.

Navigation and window controls use [Bootstrap Icons](https://icons.getbootstrap.com/) v1.13.1, embedded in the executable for offline use. The icon font is loaded privately in memory; no font installation or extra runtime files are required. Its MIT license and original assets are in `third_party/bootstrap-icons`, and the license is also embedded in the executable.

The top-left header carries a Bootstrap chat icon as the app logo. The same icon is embedded in the executable for the Windows taskbar, Alt+Tab, and Explorer, with sizes from 16 to 256 pixels. Regenerate `resources/app.ico` with `powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-app-icon.ps1` when changing the logo. Click the sidebar icon at the top of the left column, above New chat, or press **Ctrl+B**, to switch between the expanded navigation and a 72 DIP icon rail. New chat and Settings remain accessible in both modes, with hover labels on the icon buttons. Collapsing the sidebar gives the conversation and composer more room while retaining the current chat and draft.

Settings, MCP settings, and message dialogs use square window corners and the main window's custom 48 DIP dark header, Bootstrap close icon, and continuous 1px divider. Drag the header to move a modal; its close button cancels the dialog (or declines a confirmation).

Drag files from Explorer onto the window or prompt to attach them to your next message. Pending filenames appear above the prompt; the × button removes pending attachments. Send with or without a text prompt. Sent filenames remain in the user message in chat history, and their contents stay in the model context for follow-ups. Dropping files during a run queues them for your next message. New chat clears attachments and history.

Supported attachments are PNG, JPEG, GIF, WebP, PDF, and UTF-8/UTF-16 text and source documents (including TXT, Markdown, CSV, JSON, XML, logs, and common programming languages). Text documents are combined with the prompt as ordinary string content for compatibility with text-only providers; mixed attachments use content parts. Images use Chat Completions `image_url` data URLs and require a vision-capable model. PDFs use `file` parts with `filename` and base64 `file_data`. On OpenRouter, PDF requests explicitly enable the free Cloudflare text parser so text-only models can read extracted text; scanned pages may require OCR or a vision-capable model. Other providers must support PDF file parts themselves. Catalog access alone does not establish attachment support. Rejected attachment requests show model compatibility or size guidance. Export Office documents to PDF or text first. Up to 8 files per message, 10 MiB per file, 1 MiB per text document, and 32 MiB of attachments per conversation are allowed. File contents are captured at drop time, kept in memory, and sent to your configured provider only when you send the message. Conversations and sent attachments are saved locally in `chat-history.json`.

Assistant responses render Markdown natively: headings, bold/italic/strikethrough text, inline code, nested lists, task lists, blockquotes, horizontal rules, tables, and clickable HTTP/HTTPS/email links. Fenced and indented code blocks preserve whitespace; PowerShell, XML, and JSON syntax highlighting remains available. Partial Markdown is parsed as responses stream. User prompts and tool logs keep their original text formatting. HTML appears as literal text, and Markdown images show their alt text without downloading images.

PowerShell and MCP results automatically display XML and JSON payloads as highlighted code blocks. JSON is indented for readability, with distinct colors for property names, strings, numbers, and literals. MCP structured results and embedded text resources use the same formatting. Plain logs remain plain text, and the original tool results sent back to the model remain unchanged.

The MD4C 0.5.2 parser is compiled into the app. Its MIT license is in `third_party/md4c/LICENSE.md` and embedded in the executable; no additional runtime files are needed.

The API client and agent loop are native C++. Windows PowerShell is launched only when the model requests the powershell tool; it is not used for API networking or JSON. No external scripts, Node.js, or downloaded libraries are required at runtime. The nlohmann/json 3.11.3 single header and MIT license are included under `third_party/nlohmann` and compiled into the executable. Windows provides WinHTTP and DWM.

## AI configuration

Click **Settings** at the bottom of the sidebar to open the provider modal. Enter a Chat Completions compatible base URL or full endpoint URL, and an API key if required. After typing pauses, the app loads the provider's model list from the matching `/models` endpoint in the background. Choose a model from the dropdown, then click Save to persist the selection for subsequent requests. Reopening Settings restores the saved model and refreshes the list. Loading errors appear below the dropdown; a model ID can also be entered manually. Cancel discards edits. Active requests keep their current configuration. The key field is masked, and Windows DPAPI encrypts the saved key for the current Windows account.

Saved `provider-settings.json` in the configuration directory takes precedence over environment and `.env` values, including an intentionally empty key. Without saved settings, `AI_API_KEY`, `AI_MODEL`, and `AI_BASE_URL` configure any Chat Completions compatible endpoint. Shell values take precedence over `.env` values; within each source, generic names take precedence over the legacy `OPEN_ROUTER_KEY` / `OPENROUTER_API_KEY`, `OPENROUTER_MODEL`, and `AI_PROVIDER_URL` names. Empty environment values use the next nonempty fallback. The default endpoint remains `https://openrouter.ai/api/v1/chat/completions`.

Settings accepts a base URL such as `http://localhost:8000/v1` or the full `/chat/completions` URL, including a trailing slash. Inference appends `/chat/completions` to base URLs; discovery uses the sibling `/models` endpoint. API keys are optional for endpoints that allow anonymous access; empty keys omit the Authorization header. Authentication failures from hosted services are still reported. Providers must support Chat Completions SSE streaming and function tools. OpenAI Responses and native Ollama `/api/chat` protocols are not implemented; use a Chat Completions compatible endpoint instead. Discovery success verifies catalog access only, not generation or tool compatibility.

See [the provider specification review](docs/provider-integration-review.md) for how the supplied Vanilla Harness specification maps to this implementation.

The app loads `.env` beside the executable first, or from its parent directory (the project root for `build/chat.exe`). Configuration is reloaded for each request. Credentials are never compiled in or logged; `.env` files are ignored by Git.

```dotenv
OPEN_ROUTER_KEY=your-key-here
# Optional; defaults to openrouter/auto
OPENROUTER_MODEL=openrouter/auto
```

`OPENROUTER_API_KEY` is also supported. Nonempty process environment variables override file values; `OPEN_ROUTER_KEY` takes precedence over its alias within each source. The loader supports UTF-8 BOMs, quoted values, comments, and `export KEY=value` lines.

Requests include conversation history and use OpenRouter's HTTPS chat completions API. Responses stream into one assistant message as text arrives. The client parses SSE frames incrementally, including split UTF-8 characters, keepalive comments, and the completion marker. Errors appear in the chat and are excluded from future requests. Requests have a 120-second overall deadline and a 4 MB response limit. New chat and closing the app cancel active networking; stale replies are discarded. Interrupted streams retain visible partial text and show an error, but partial answers are excluded from subsequent API history.

The app supports an agent loop with a `powershell` function tool. It receives streamed tool-call fragments, waits for the complete call, validates the arguments, runs commands sequentially, adds each result with its matching `tool_call_id`, and calls the model again. Text continues streaming throughout model responses. Select an `OPENROUTER_MODEL` that supports tool calling.

Each sent prompt shows LLM progress, call count, sent/received token counts, and elapsed time beneath it. Token counts come from provider usage reports and are summed across all model calls for that prompt, including tool follow-ups; missing usage is shown as "not reported," and incomplete totals show how many calls reported usage. LLM time measures model requests; total time also includes configuration, MCP connection setup, and tool execution. Tool commands, output, and exit status appear in execution order. Set **Max model turns** (1?100) and **Max tool calls** (1?1000) in **Settings > Provider** to control each run. Defaults are eight turns and sixteen executed tool calls. Limits are saved for subsequent runs; active runs keep their existing limits. The last model turn cannot execute further tools. Unknown tools and invalid arguments return structured errors to the model.

PowerShell calls start fresh in the configuration/project directory. They run hidden, noninteractively, with normal user permissions; this working directory is **not a filesystem sandbox**. Commands can inspect and modify files. Calls default to a 30-second timeout (configurable by the model from 1 to 120 seconds), capture at most 64 KB of combined output, and terminate their process tree on cancellation, timeout, or completion. The chat displays up to 4,000 characters of each output. Provider key environment variables are removed from the child process; the active API key is redacted from tool output and displayed activity. The system prompt instructs the model not to read credential files.

New chat clears history and cancels the current operation. Completed filesystem changes remain. API history retains assistant tool calls and their results for follow-up prompts; it is separate from the visible progress and tool display. Conversations remain in memory only. Requests use your OpenRouter account and may incur model charges.

Open **Settings > System Prompt** to edit the model?s system instructions. **Restore default** restores the built-in instructions in the editor; **Save** applies changes to subsequent runs, and **Cancel** discards edits. An empty prompt is allowed.

The main window can be resized down to 360 DIP wide (half the previous minimum); the sidebar automatically uses its compact rail below 720 DIP.

## Computer control

Open **Settings > Computer Control > Test access** to verify a real mouse click, keyboard text delivery, and screenshot capture in the settings test field. The result identifies failed checks. **Test mouse circle** moves the pointer around a visible circle for three seconds, checks that each position was reached, and returns it to its starting position. Keep Settings in the foreground and the desktop unlocked while testing. Windows blocks input to elevated applications and secure desktops from a normal application.

The built-in `computer` tool lets the agent operate the Windows desktop using screenshots, pointer movement, left/right/middle clicks, double clicks, dragging, vertical scrolling, Unicode typing, key combinations, and cancellable waits. Choose a model that supports both vision and function tools, then ask for a desktop task in chat. No MCP server or extra runtime is needed. Computer actions execute from structured model tool calls in either streamed or JSON Chat Completions responses. Action arguments and results appear in chat; JSON written as ordinary assistant prose is not executed.

Screenshots capture all monitors as a PNG and send it to your configured provider. The result includes `left`, `top`, `width`, and `height` in physical desktop pixels. Image pixel `(0,0)` maps to desktop `(left,top)`; monitors left of the primary display can have negative coordinates. Actions use those desktop coordinates. Only the newest screenshot batch stays in model history to bound image usage. Screenshots are held in memory and are not written to disk.

Examples: `{"action":"screenshot"}`, `{"action":"click","x":500,"y":400}`, `{"action":"key","keys":["CTRL","L"]}`, `{"action":"type","text":"Hello"}`, and `{"action":"scroll","delta":-3}`. Drag uses `x`, `y`, `to_x`, and `to_y`. Positive scroll deltas go up. Key combinations release their modifiers after execution.

Input affects the active desktop and focused application, with the app's ordinary Windows permissions. Keep the session unlocked and avoid using the mouse and keyboard during a task. Elevated windows and secure desktops may reject input. Stop cancels further actions and waits; completed input remains. The configured model-turn and tool-call limits apply; send a follow-up to continue longer tasks. Tool activity and errors appear in chat.

## MCP servers

Open **Settings** and select the **MCP** tab beside **Provider**. The tabs use square edges and 1px separators. Add servers using the standard `mcpServers` JSON object, click **Test connections** to see discovered tools, then **Save**. Switching tabs retains your edits, and Save validates and keeps changes from both tabs. **Example** inserts disabled local and remote entries. Cancel discards unsaved edits.

**Disable** immediately saves a global MCP-off setting, retaining all server definitions and credentials. The button changes to **Enable** so MCP can be restored without re-entering server details. This affects new prompts; in-progress requests keep their existing connections. Each individual server can also use `"enabled": false` in the JSON configuration.

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node.exe",
      "args": ["C:\\tools\\my-server\\index.js"],
      "env": {"SERVICE_TOKEN": "your-token"},
      "cwd": ".",
      "enabled": true
    },
    "remote-tools": {
      "url": "https://your-server.example/mcp",
      "headers": {"Authorization": "Bearer your-token"},
      "enabled": true
    }
  }
}
```

Local servers use newline-delimited JSON-RPC over stdio, with stderr kept separate. `command` is an executable and `args` is an array of individual arguments; use an installed runtime such as `node.exe` or `python.exe` directly. For batch wrappers, configure `cmd.exe` explicitly. Relative `cwd` values resolve from the app's configuration/project directory. Server runtimes and packages must be installed separately. Process environment variables are inherited except the legacy OpenRouter key variables; `env` supplies explicit overrides.

Remote servers use Streamable HTTP with JSON or SSE responses, optional static authentication headers, and server-assigned sessions. The client implements the [MCP 2025-11-25 lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle) (also accepting 2025-06-18, 2025-03-26, and 2024-11-05 for compatible servers). OAuth login, the older standalone HTTP+SSE transport, automatic SSE reconnection, resources/prompts browsing, sampling, elicitation, and task-based calls are not implemented. The UI and model consume text/structured tool results; binary image/audio data is omitted.

Enabled servers connect and discover tools at the beginning of each prompt. The model receives stable namespaced tools alongside PowerShell. Invocations and results appear in the conversation, with the same agent step limits. A failed server is reported without blocking the remaining servers. Each MCP operation has a 30-second deadline and 4 MB response limit; tool results sent to the model are limited to 64 KB. New chat, cancellation, or app exit closes local server process jobs and cancels networking. Tool lists refresh on the next prompt. Connection tests initialize servers and list tools without invoking those tools.

MCP configuration, including environment values and HTTP headers, is encrypted for the current Windows account in `mcp-servers.json`. Do not distribute this personal configuration file with the app. App distribution still needs only the executable and third-party license notices; local MCP servers may require their own runtimes and dependencies.

## Source layout

```text
src/
  app/       # Win32 entry point, UI integration, worker lifecycle
  agent/     # Model/tool loop and progress events
  client/    # Chat API, SSE decoding, call metrics
  common/    # Text encoding, attachments, tool-output formatting
  config/    # Provider settings and credential loading
  tools/     # PowerShell, desktop control, MCP client
  ui/        # Markdown parsing and code highlighting
resources/   # Windows resource script and application icon
third_party/ # Vendored dependencies and their licenses
tests/      # C++ test runner, mock servers, UI smoke tests
tools/      # Development scripts, including icon generation
docs/       # Reference images and design notes
```

Implementation files and their private headers are colocated. Project includes use paths relative to `src/`, such as `"client/chat_client.h"`; dependency includes resolve from `third_party/`, such as `<nlohmann/json.hpp>`. There is no public `include/` directory because this project builds an application, not an installed library. Keep generated files in `build/` or `out/`, both ignored for new files, and personal CMake settings in `CMakeUserPresets.json`.

`build.cmd` remains the direct MSVC build entry point, with a shared source list for the app and test executable. `CMakeLists.txt` also provides `harness_core` (shared internal implementation), `chat` (Win32 app and resources), and `chat-tests` targets. Compiler settings, include paths, and Windows library dependencies are scoped to their targets. Vendored MD4C C code is compiled separately.

For CMake with Visual Studio's C++ CMake tools installed, use a developer shell:

```powershell
cmake -S . -B out/build
cmake --build out/build --config Release
ctest --test-dir out/build -C Release --output-on-failure
```

Node.js is required to run the integration tests. CTest launches the same loopback mock server and test executable as `build.cmd test`; tests run with the repository root as their working directory. Desktop screenshot coverage requires an unlocked interactive Windows desktop. Use `-DBUILD_TESTING=OFF` at configure time for an app-only build. Visual Studio can also open the repository directly as a CMake project. CMake executables live in its build directory (under `Release/` for Visual Studio); configure the provider through Settings when running from that location, or put `.env` beside the executable. The direct build still produces `build/chat.exe`.

The client uses asynchronous WinHTTP handles internally so it can cancel in-flight I/O safely. Request buffers and callback state stay alive until WinHTTP signals handle closure. The UI never performs network operations or closes networking handles across threads.

## Tests

Launch the native Windows test dashboard with:

```powershell
.\test-ui.cmd
```

Build the app and test executable first (`.\build.cmd` and `.\build.cmd test`), or enter your CMake executable paths in the dashboard. Select **Run selected suite** to run one suite, or **Run all suites** to execute all three sequentially. The dashboard shows suite descriptions, colored pass/fail status, elapsed time, live C++ assertion results, and process output. **Stop** terminates the running process tree; **Save report** exports the current run as JSON. A missing executable or dependency is reported as a failed suite with an explanation. Desktop suites require an unlocked interactive desktop and may move the pointer or take focus. The live paid API smoke test is excluded.

Individual C++ assertions currently share setup and execute sequentially inside the core suite; the dashboard runs suites individually and displays each assertion as it completes. C++ execution stops at the first failure, so later assertions have not run. The app executable path is also used for the UI/mock API suite.

```powershell
.\build.cmd test
```

Tests additionally require Node.js, which runs a loopback-only mock server. They cover configuration precedence, Unicode/JSON, history filtering, response validation, HTTP errors, chunked responses, incremental SSE/UTF-8 parsing, first-token delivery before EOF, interrupted streams, redirect refusal, response limits, deadlines, cancellation, request replacement, worker shutdown, real PowerShell execution/error/output limits, tool-argument validation, tool result IDs, and an end-to-end agent loop against the mock API. No real API key or public network is used by these tests.

An optional live smoke test uses the current `.env` and makes one small paid request:

```powershell
.\build\chat-tests.exe --live
```

UI smoke test (launches the app, verifies square caption controls and window actions, then closes it):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui_smoke.ps1
```

A preview is saved to `build/ui-preview.png`. The UI uses 24 DIP section padding, a centered conversation column up to 880 DIP wide, and a rectangular prompt with a 16 DIP inset.

MCP tests use `tests/mock_mcp.cjs` for stdio and the mock HTTP server for session headers, SSE frames, authenticated discovery/calls, server failures, and the complete model/tool loop. UI integration against the mock server (after building `build/chat-preview.exe`) runs with `node tests/mock_server.cjs --ui`; it also checks model selection and MCP configuration persistence in an isolated test directory.

Responses support fenced code blocks with preserved indentation, monospace text, and language labels. PowerShell (powershell, pwsh, ps, ps1) XML/HTML, and JSON blocks include syntax highlighting. Long code lines wrap to the conversation width. Executed PowerShell commands use the same code formatting.



## Local chat history and workspaces

Conversations, tool results, sent attachments, and text drafts are stored in `chat-history.json` beside `mcp-servers.json` and provider settings. The latest active chat is restored on startup. Select a conversation in the sidebar (or the **Chats** menu on the compact rail) to continue it; switching chats stops an active run. New chat preserves earlier conversations. Hover over a sidebar history item and click its trash icon to delete that saved conversation. Deleting the active chat opens another saved chat or a new blank chat. Workspace files are kept. History is written through a temporary JSON file and replaced atomically. If existing history cannot be read, the app reports the error and leaves that file unchanged.

Each chat gets a unique five-character alphanumeric ID and a `.workspace/<ID>/` directory beside the settings files. PowerShell commands and local MCP servers start in that chat?s workspace. **Open workspace** in the sidebar opens its files in Explorer. Tool access remains governed by your Windows permissions; the workspace is a working directory, not a filesystem sandbox. History and attachments are local JSON data, not encrypted; the history file and workspace directories are excluded from Git.
