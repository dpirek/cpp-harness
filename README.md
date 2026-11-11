# Native agent chat harness

A Win32 C++ desktop chat app styled after `docs/image.png`. It launches maximized with a 48 DIP custom dark title bar, square window controls, and a dark sidebar, rounded user messages, and a rectangular prompt box with a Send icon button on the right. The prompt begins as one line, expands upward as text wraps or newlines are added, and scrolls internally after six lines. It shrinks again when text is removed. Enter or the Send button sends; Shift+Enter inserts a newline. While the agent runs, Send becomes Stop. Stop cancels the current operation, keeps partial text and your draft, and restores Send; completed filesystem changes remain. New chat clears the conversation and cancels the active request. Scroll with the mouse wheel.

## Build and run

Install Visual Studio's Desktop development with C++ workload:

```powershell
.\build.cmd
.\build\chat.exe
```

The app stays named `chat.exe` and has an embedded `asInvoker` manifest, so it runs with standard user permissions. Close the running app before rebuilding it.

Navigation and window controls use [Bootstrap Icons](https://icons.getbootstrap.com/) v1.13.1, embedded in the executable for offline use. The icon font is loaded privately in memory; no font installation or extra runtime files are required. Its MIT license and original assets are in `third_party/bootstrap-icons`, and the license is also embedded in the executable.

The top-left header carries a Bootstrap chat icon as the app logo. The same icon is embedded in the executable for the Windows taskbar, Alt+Tab, and Explorer, with sizes from 16 to 256 pixels. Regenerate `app.ico` with `powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-app-icon.ps1` when changing the logo. Click the sidebar icon at the top of the left column, above New chat, or press **Ctrl+B**, to switch between the expanded navigation and a 72 DIP icon rail. New chat and Settings remain accessible in both modes, with hover labels on the icon buttons. Collapsing the sidebar gives the conversation and composer more room while retaining the current chat and draft.

Settings, MCP settings, and message dialogs use square window corners and the main window's custom 48 DIP dark header, Bootstrap close icon, and continuous 1px divider. Drag the header to move a modal; its close button cancels the dialog (or declines a confirmation).

Drag files from Explorer onto the window or prompt to attach them to your next message. Pending filenames appear above the prompt; the × button removes pending attachments. Send with or without a text prompt. Sent filenames remain in the user message in chat history, and their contents stay in the model context for follow-ups. Dropping files during a run queues them for your next message. New chat clears attachments and history.

Supported attachments are PNG, JPEG, GIF, WebP, PDF, and UTF-8/UTF-16 text and source documents (including TXT, Markdown, CSV, JSON, XML, logs, and common programming languages). Text documents are combined with the prompt as ordinary string content for compatibility with text-only providers; mixed attachments use content parts. Images use Chat Completions `image_url` data URLs and require a vision-capable model. PDFs use `file` parts with `filename` and base64 `file_data`. On OpenRouter, PDF requests explicitly enable the free Cloudflare text parser so text-only models can read extracted text; scanned pages may require OCR or a vision-capable model. Other providers must support PDF file parts themselves. Catalog access alone does not establish attachment support. Rejected attachment requests show model compatibility or size guidance. Export Office documents to PDF or text first. Up to 8 files per message, 10 MiB per file, 1 MiB per text document, and 32 MiB of attachments per conversation are allowed. File contents are captured at drop time, kept in memory, and sent to your configured provider only when you send the message. The app does not persist conversations or attachments.

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

Each sent prompt shows LLM progress, call count, sent/received token counts, and elapsed time beneath it. Token counts come from provider usage reports and are summed across all model calls for that prompt, including tool follow-ups; missing usage is shown as "not reported," and incomplete totals show how many calls reported usage. LLM time measures model requests; total time also includes configuration, MCP connection setup, and tool execution. Tool commands, output, and exit status appear in execution order. The loop stops after eight model turns or sixteen executed tool calls. Unknown tools and invalid arguments return structured errors to the model.

PowerShell calls start fresh in the configuration/project directory. They run hidden, noninteractively, with normal user permissions; this working directory is **not a filesystem sandbox**. Commands can inspect and modify files. Calls default to a 30-second timeout (configurable by the model from 1 to 120 seconds), capture at most 64 KB of combined output, and terminate their process tree on cancellation, timeout, or completion. The chat displays up to 4,000 characters of each output. Provider key environment variables are removed from the child process; the active API key is redacted from tool output and displayed activity. The system prompt instructs the model not to read credential files.

New chat clears history and cancels the current operation. Completed filesystem changes remain. API history retains assistant tool calls and their results for follow-up prompts; it is separate from the visible progress and tool display. Conversations remain in memory only. Requests use your OpenRouter account and may incur model charges.

## MCP servers

Open **Settings** and select the **MCP** tab beside **Provider**. Both tabs use square edges and 1px separators. Add servers using the standard `mcpServers` JSON object, click **Test connections** to see discovered tools, then **Save**. Switching tabs retains your edits, and Save validates and keeps changes from both tabs. **Example** inserts disabled local and remote entries. Cancel discards unsaved edits.

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

- `main.cpp`: DPI-aware Win32 drawing, shared spacing/layout, input, custom title bar, and UI message handling.
- `markdown.h` / `markdown.cpp`: Markdown parsing into native blocks and styled text runs, including tables, lists, links, and entity decoding.
- `config.h` / `config.cpp`: configuration and credential loading.
- `chat_client.h` / `chat_client.cpp`: streaming requests, WinHTTP, timeouts, and cancellation.
- `stream_decoder.h` / `stream_decoder.cpp`: incremental SSE framing, UTF-8 content deltas, and stream completion validation.
- `agent.h` / `agent.cpp`: bounded model/tool loop, validated tool dispatch, protocol history, and progress events.
- `powershell_tool.h` / `powershell_tool.cpp`: hidden command execution, UTF-8 output capture, timeout, and process-tree cancellation.
- `mcp_client.h` / `mcp_client.cpp`: encrypted MCP settings, stdio server processes, initialization, tool discovery, namespacing, and invocation. The HTTP transport uses the shared asynchronous WinHTTP client.
- `chat_worker.h` / `chat_worker.cpp`: one owned worker thread, request replacement, and clean shutdown. Results are posted to the UI thread.

The client uses asynchronous WinHTTP handles internally so it can cancel in-flight I/O safely. Request buffers and callback state stay alive until WinHTTP signals handle closure. The UI never performs network operations or closes networking handles across threads.

## Tests

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


