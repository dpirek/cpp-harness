# Agent Office harness integration specification

This document specifies how to connect another agent harness to Agent Office using the protocol implemented by this repository. It is intended to be handed directly to an engineer or coding agent implementing an adapter.

**Baseline:** repository commit `d464691d235010753fe48926cb983e1572c3d5d4`, inspected November 13, 2026. This is a source-derived compatibility specification, not a claim that every Office deployment supports every optional endpoint. No live Office server was contacted. Examples contain placeholder credentials only.

“Required” below means required to reproduce this worker's integration behavior. Optional features and recommended improvements are identified explicitly. There is no protocol-version negotiation on the worker WebSocket; the package version `1.0.0` is not a negotiated wire version.

## 1. Architecture and integration choices

The worker opens and owns one persistent outbound WebSocket to Office. Registration, task assignments, direct questions, progress, cancellation, and final responses use that socket. Office does not submit tasks to a worker HTTP endpoint, and the worker does not issue HTTP completion callbacks.

Artifact bytes and project-context MCP calls use separate outbound HTTP(S) requests. The worker's local HTTP server serves its console, diagnostics, metadata, and workspace file links. The advertised worker `/agent` URL is an identity URL; this implementation does not require Office to open a second socket to it.

```text
Agent harness adapter                         Agent Office
  |--- WebSocket connect /ws/workers ------------>|
  |--- register (shared token + metadata) ------->|
  |<-- registered (connectionId) -----------------|
  |<-- task / direct_message / task_cancel -------|
  |--- working / activity ----------------------->|
  |--- HTTP MCP requests (task credential) ------>|
  |--- HTTP ZIP upload (task credential) -------->|
  |<-- artifactId -------------------------------|
  |--- completed + uploadedArtifactIds ---------->|
  |<-- task_update_ack --------------------------|
```

Choose one implementation route:

1. **Reuse this worker shell (recommended for a Node-compatible adapter).** Implement `runTask(prompt, context)` and pass it to `createWorkerServer`. The shell owns connection management, queues, cancellation, MCP setup, ZIP creation, uploads, and task history. `codex.js` and `claude.js` demonstrate this boundary.
2. **Implement the protocol independently.** Reproduce the message formats and lifecycle below in the target language. The target harness can be any CLI, SDK, or agent loop; model-provider APIs are not part of the Office protocol.

A first compatible release needs registration, heartbeats, task execution, direct replies, cancellation, reconnect handling, and artifact upload. Support supplied task MCP configuration for project-aware work. History lookup, teammate delegation, connectivity probes, and the local dashboard are separate features.

## 2. Configuration and addressing

| Setting | Meaning / reference default |
| --- | --- |
| `AI_HARNESS_OFFICE_URL` | Required for Office operation; `ws://host:port/ws/workers` or `wss://host/ws/workers`. |
| `AI_HARNESS_WORKER_TOKEN` | Shared registration token configured by the Office operator. |
| `WORKER_NAME` | Stable registry identity; default `Coding Worker Agent`. Use a distinct name per independently active worker. |
| `WORKER_DESCRIPTION` | Human-readable routing/discovery description. |
| `WORKER_PUBLIC_URL` | Credential-free HTTP(S) base for worker identity and local file links; defaults to the local listening address. |
| `WORKER_SKILLS` | JSON array of `{id, name, description}`; every value must be non-empty after normalization. |
| `WORKER_TOOLS` | Comma-separated advertised tool names. In the built-in runner this also selects enabled tools. |
| `WORKER_CONCURRENCY` | Task execution slots; default `1`. |
| `WORKER_DIRECT_MESSAGE_CONCURRENCY` | Independent direct-answer slots; default `2`. |
| `WORKER_RECONNECT_MIN_MS` / `WORKER_RECONNECT_MAX_MS` | Backoff bounds; defaults `1000` / `30000`. |
| `WORKER_HEARTBEAT_MS` | Application heartbeat interval; default `30000`. |
| `WORKER_WORKSPACE` | Workspace root; default `.workspace`. |
| `WORKER_TASK_DB` | Local history database; default `db/tasks.sqlite`. |
| `WORKER_UPLOAD_TIMEOUT_MS` | Per-upload timeout; default `120000`. |
| `AI_HARNESS_OFFICE_HTTP_URL` | Optional HTTP origin override for uploads and legacy context/collaboration APIs. |
| `AI_HARNESS_OFFICE_UPLOAD_WORKSPACE` | Legacy Office upload workspace; default `.`. |

Example deployment configuration:

```dotenv
AI_HARNESS_OFFICE_URL=wss://office.example.com/ws/workers
AI_HARNESS_WORKER_TOKEN=REPLACE_WITH_SHARED_WORKER_TOKEN
WORKER_NAME=my-harness-worker
WORKER_DESCRIPTION=Implements and validates software changes
WORKER_PUBLIC_URL=https://worker.example.com
WORKER_CONCURRENCY=1
WORKER_DIRECT_MESSAGE_CONCURRENCY=2
WORKER_WORKSPACE=.workspace
```

URL rules:

- A bare Office WS(S) origin expands to `/ws/workers`; an explicit path is preserved. Reject credentials and query parameters; strip fragments.
- Convert `ws:` to `http:` and `wss:` to `https:` for HTTP APIs, then use absolute API paths from the origin. Upload/context helpers honor the HTTP override; task MCP validation specifically uses the WebSocket origin.
- Convert the public worker HTTP(S) URL to WS(S) and append `/agent` to its path for registration. Reject credentials and query parameters; strip fragments.
- Use verified TLS. The current native WebSocket implementation rejects `AI_HARNESS_OFFICE_TLS_REJECT_UNAUTHORIZED=false` for WSS. A private CA can be supplied through `NODE_EXTRA_CA_CERTS` at process startup. The older integration note suggesting that verification can be disabled is stale.

## 3. Transport and registration

Use UTF-8 JSON text WebSocket messages, at most **2,097,152 bytes** per complete application message, including final results. Binary application messages are rejected. The current client uses the native Node WebSocket API, with no custom handshake authorization header or subprotocol. Credentials travel in the first JSON message, never in the URL.

Immediately after socket open, send:

```json
{
  "type": "register",
  "credentials": { "token": "SHARED_WORKER_TOKEN" },
  "worker": {
    "name": "my-harness-worker",
    "description": "Implements and validates software changes",
    "url": "wss://worker.example.com/agent",
    "capabilities": {
      "skills": [
        { "id": "coding", "name": "Coding", "description": "Build and test software" }
      ],
      "tools": ["my_harness_cli"],
      "mcp": true,
      "workspaceArtifacts": true
    },
    "model": {
      "provider": "my-harness",
      "name": "configured-model",
      "url": "local://my-harness-cli"
    }
  }
}
```

Worker names match `^[A-Za-z0-9][A-Za-z0-9 _-]{0,99}$`. Capabilities are discovery metadata, not executable tool definitions. Advertise only implemented features; the reference shell advertises MCP and workspace artifacts as enabled. Model metadata must not contain API keys or private authorization information.

Office acknowledges registration with:

```json
{ "type": "registered", "connectionId": "connection-123" }
```

An optional `worker` field is accepted. Require a non-empty connection ID before accepting assignments, direct messages, or cancellation. Store the connection identity with each assignment. Connection IDs are not task IDs and are not included in outgoing task updates.

Send `{ "type": "ping" }` every heartbeat interval. Office returns `{ "type": "pong" }`. These are application messages, distinct from WebSocket control frames. The reference starts heartbeats at socket open, including registration time. It has no registration deadline or missed-pong watchdog; those are recommended operational improvements, not existing protocol guarantees.

### Message dispatch table

| Direction | Type | Handling |
| --- | --- | --- |
| Worker → Office | `register` | Authenticate and advertise metadata. |
| Office → Worker | `registered` | Set active connection identity and become ready. |
| Worker → Office / Office → Worker | `ping` / `pong` | Heartbeat. |
| Office → Worker | `task` | Validate and queue an assignment. |
| Worker → Office | `task_update` | Working, completed, or failed result. |
| Office → Worker | `task_update_ack` | Accepted without further action. |
| Office → Worker | `direct_message` | Queue a direct answer or execute a stop command. |
| Worker → Office | `direct_message_response` | Correlated plain-text answer. |
| Office → Worker | `direct_message_ack` | Accepted without further action. |
| Office → Worker | `task_cancel` | Stop matching task without a final update. |
| Worker → Office | `activity` | Optional model/tool telemetry. |
| Office → Worker | `error` | Log `error`; does not itself close the connection. |

Malformed JSON, oversized/non-text messages, unexpected message types, and synchronous assignment validation failures close the reference connection with code `4002`, reason `Worker protocol error`. Unknown fields on known envelopes are generally ignored. Ack bodies are not inspected; do not depend on undocumented fields.

## 4. Assignment envelope and identities

```json
{
  "type": "task",
  "taskId": "task-001",
  "projectId": "project-alpha",
  "officeTaskId": "office-task-42",
  "message": {
    "messageId": "message-001",
    "role": "user",
    "parts": [{ "kind": "text", "text": "Build a report and save the deliverables." }]
  },
  "artifactUpload": {
    "url": "/api/worker-artifacts?taskId=task-001",
    "token": "TASK_UPLOAD_TOKEN",
    "method": "POST",
    "contentType": "application/octet-stream"
  },
  "mcpServers": {
    "office_project": {
      "type": "http",
      "url": "/mcp/projects/project-alpha/tasks/task-001",
      "headers": { "Authorization": "Bearer TASK_MCP_TOKEN" }
    }
  }
}
```

| Field | Interpretation |
| --- | --- |
| `taskId` | Worker execution identity. Required; trim and validate against `^[A-Za-z0-9][A-Za-z0-9_-]{0,199}$`. Used for workspace, updates, upload URL binding, and MCP task binding. |
| `message.messageId` | Required incoming correlation ID; non-empty after normalization, maximum 200 characters. Copy to every update's `inReplyTo`. |
| `message.parts` | Non-empty array containing at least one non-empty string text part with `kind: "text"`. Trim each usable part and join with two newlines. Non-text parts are ignored by this adapter. |
| `message.role` | Present in examples, but not validated by the reference receiver. |
| `projectId` | Context project. Fallback: `context.project.id`, then `central-office`. |
| `officeTaskId` | Office's own task identity, separate from worker `taskId`. Fallback: `context.task.id`, then `null`. |
| `artifactUpload` | Optional only for legacy compatibility. If present, validate and use it; malformed configuration must not trigger fallback. |
| `mcpServers` | Optional task-scoped project access; see section 9. |

Do not replace either incoming identity with a newly generated one. Each outgoing message also gets its own fresh `message.messageId`; that ID does not replace `inReplyTo`.

The reference rejects a task ID already in its in-memory task map, including completed tasks from the current process. It is not a durable deduplication/replay protocol. Its task database also requires unique message IDs, and workspace creation fails if the task directory already exists. Office should assign fresh task and message IDs for new executions.

## 5. Task lifecycle and updates

Validate and enqueue in the socket handler, then return immediately. Run model work, subprocesses, MCP, and uploads asynchronously. Keep heartbeats and cancellation responsive while other work is active.

Local states are `submitted` → `working` → `completed` or `failed`. `submitted` is a local queue state; the shell sends no queued acknowledgement. Office cancellation additionally produces local `cancelled` state.

When a queue slot opens, emit:

```json
{
  "type": "task_update",
  "taskId": "task-001",
  "inReplyTo": "message-001",
  "status": { "state": "working" },
  "message": {
    "messageId": "working-UNIQUE_ID",
    "role": "agent",
    "parts": [{ "kind": "text", "mimeType": "text/markdown", "text": "Task accepted; work is starting." }]
  }
}
```

Execution sequence:

1. Create a fresh directory `<workspaceRoot>/<taskId>/`.
2. Validate and connect supplied Office MCP servers. Invalid supplied MCP configuration or failed initialization fails this task; it is not silently discarded.
3. Run the harness in that directory with the prompt, cancellation signal, and project context.
4. Check task ownership/cancellation before accepting the result.
5. Write the final Markdown response to `output.md` inside the task workspace.
6. Package workspace files into `<workspaceRoot>/<taskId>.zip`, outside the directory being archived.
7. Upload the ZIP and validate the upload response.
8. Check cancellation/connection ownership again, then persist and send one terminal completion.
9. Close MCP sessions and discard task-scoped credentials on every terminal path.

Successful task-scoped completion:

```json
{
  "type": "task_update",
  "taskId": "task-001",
  "inReplyTo": "message-001",
  "status": { "state": "completed" },
  "message": {
    "messageId": "completed-UNIQUE_ID",
    "role": "agent",
    "parts": [{ "kind": "text", "mimeType": "text/markdown", "text": "Created the requested report." }]
  },
  "uploadedArtifactIds": ["artifact-123456"]
}
```

Failure:

```json
{
  "type": "task_update",
  "taskId": "task-001",
  "inReplyTo": "message-001",
  "status": { "state": "failed" },
  "message": {
    "messageId": "failed-UNIQUE_ID",
    "role": "agent",
    "parts": [{ "kind": "text", "mimeType": "text/markdown", "text": "Task failed: artifact upload failed." }]
  },
  "error": { "code": "TASK_FAILED", "message": "Artifact upload failed." }
}
```

Failed updates contain no `artifacts` or `uploadedArtifactIds`. The reference may include diagnostic `error.details`; sanitize diagnostics before transmission. An upload failure is a task failure even if the model completed successfully.

“One terminal update” means one attempted terminal send for a normal connected execution, not guaranteed exactly-once delivery. There is no durable outgoing queue, ack wait, retransmission, or resume handshake. A lost socket can prevent the final result from reaching Office. Office-originated cancellation intentionally sends no terminal update.

### Optional activity

```json
{
  "type": "activity",
  "taskId": "task-001",
  "messageId": "message-001",
  "event": {
    "category": "tool",
    "message": "Running project tests",
    "tone": "info",
    "metadata": { "tool": "run_tests" }
  }
}
```

Direct-message activity omits `taskId`. Activity uses the incoming `messageId` at the top level, unlike result envelopes' `inReplyTo`. This is optional telemetry, not a substitute for task updates. The generic runner supplies events; the CLI adapters do not currently translate their entire output stream into these events. Redact tokens, sensitive headers, and tool arguments before sending telemetry.

## 6. Artifact delivery

### Preferred: assignment-scoped upload

Resolve `artifactUpload.url` against the configured Office HTTP origin, then validate all of these conditions before transmitting credentials:

- Same origin as Office, no URL username/password, and no fragment.
- Path exactly `/api/worker-artifacts`.
- Exactly one `taskId` query value, exactly matching the worker task ID.
- `method` exactly `POST`; `contentType` exactly `application/octet-stream`.
- Token is a non-empty string containing no whitespace.

Replace/add the `name` query parameter with `<taskId>.zip`; preserve other permitted query parameters. Reject redirects. The current archive size ceiling is **104,857,600 bytes (100 MiB)**.

```http
POST /api/worker-artifacts?taskId=task-001&name=task-001.zip
Authorization: Bearer TASK_UPLOAD_TOKEN
Accept: application/json
Content-Type: application/octet-stream
Content-Length: ZIP_BYTE_LENGTH

[raw ZIP bytes, not multipart and not base64]
```

Require an HTTP success response and a JSON object such as:

```json
{ "ok": true, "artifactId": "artifact-123456" }
```

The ID must match `^artifact-[A-Za-z0-9-]+$`. Cap the response at **1,000,000 bytes** (the implementation's error text loosely calls this 1 MiB). Apply the upload timeout and task cancellation signal to the request and response reading.

Only after upload succeeds, send `completed` with the returned ID in `uploadedArtifactIds`. Do not create a download-URL artifact for this path. Do not substitute the shared worker token for a task upload token. On invalid configuration, authentication failure, timeout, redirect, malformed JSON, or missing ID, fail the task; never downgrade to the legacy endpoint.

The reference ZIP contains regular files relative to the workspace root, including `output.md`, with UTF-8 names. It recursively includes hidden directories/files and skips symlinks and empty directories. It uses uncompressed ZIP32. Another implementation should produce a compatible standard ZIP with safe relative member paths. Keep credentials and harness configuration outside the deliverable directory; the reference packager has no secret-file exclusion list.

### Legacy: assignments without `artifactUpload`

Only if the field is absent, the worker uses:

```http
POST /api/workspace-upload?workspace=.&name=task-001.zip
Authorization: Bearer SHARED_WORKER_TOKEN
X-Agent-Name: my-harness-worker
X-Office-Task-Id: task-001
X-Office-Message-Id: message-001
Accept: application/json
Content-Type: application/zip
Content-Length: ZIP_BYTE_LENGTH

[raw ZIP bytes]
```

`X-Office-Task-Id` here is the worker task ID despite its header name. The `workspace` value comes from configuration. The client accepts an HTTP success JSON payload unless `ok` is explicitly `false`; it reads `uri`, `relativePath`, and `size`. Prefer an Office response containing all of them. When `uri` is absent, it builds `/api/workspace-file-asset?projectId=<assignedProject>&path=<relativePath-or-name>` on Office's HTTP origin.

The completion contains this `artifacts` array instead of `uploadedArtifactIds`:

```json
{
  "artifacts": [{
    "artifactId": "workspace-task-001",
    "name": "task-001.zip",
    "parts": [{
      "kind": "file",
      "file": {
        "name": "task-001.zip",
        "mimeType": "application/zip",
        "uri": "https://office.example.com/api/workspace-file-asset?projectId=project-alpha&path=task-001.zip"
      }
    }],
    "metadata": { "fileCount": 2, "size": 1234 }
  }]
}
```

This is a fragment to add to the ordinary completed envelope. Do not confuse the legacy MIME type/shared token with the preferred task-scoped MIME type/token.

## 7. Direct messages

```json
{
  "type": "direct_message",
  "projectId": "project-alpha",
  "message": {
    "messageId": "question-001",
    "role": "user",
    "parts": [{ "kind": "text", "text": "What did you find?" }]
  }
}
```

Use the same message validation and project fallback rules as assignments. Run direct questions on a separate queue, in a fresh temporary workspace such as `.direct-<UUID>`, so a long task does not block questions. Return:

```json
{
  "type": "direct_message_response",
  "inReplyTo": "question-001",
  "message": {
    "messageId": "reply-UNIQUE_ID",
    "role": "agent",
    "parts": [{ "kind": "text", "mimeType": "text/plain", "text": "The answer to your question." }]
  }
}
```

Return non-empty text and no `taskId`, `status`, artifacts, or upload IDs. Direct questions do not create task-history records or publish workspace ZIPs; they can appear in message history. Delete their temporary workspace afterward. The reference converts execution errors into an ordinary text reply beginning `Unable to answer:`.

The current direct runner receives no assignment MCP configuration or task cancellation signal. It does not resume a previous conversation; prior knowledge must be explicitly loaded through available context tools. Direct work is not aborted by connection-loss handling, but its reply remains bound to the original socket and cannot be delivered through a replacement connection. An independent implementation should cancel stale direct work as a resource-management improvement.

## 8. Cancellation, disconnects, and reconnects

### Office-originated cancellation

```json
{
  "type": "task_cancel",
  "taskId": "task-001",
  "inReplyTo": "message-001",
  "reason": "The assignment was cancelled."
}
```

Match `taskId`, the receiving connection identity, and an active local state (`submitted` or `working`). If `inReplyTo` is present and truthy, require a match to the original message ID. Unknown, finished, wrong-connection, or mismatched tasks are ignored. `reason` is optional; the default is `Task cancelled by Office.`

Mark the task stopped and locally `cancelled`, abort model/tool/upload operations, revoke task credentials, and suppress late completion. **Do not send a cancellation ack or terminal `task_update` in response:** this is how the existing handler behaves, because Office initiated the cancellation.

### User stop commands

The worker intercepts `stop current task`, `stop task TASK_ID`, and `stop all tasks` before invoking a model. `cancel` and `abort` are aliases; conversational forms are recognized. Explicit target lookup accepts either worker task ID or incoming message ID. Scope candidates to the current connection; if “current” matches multiple active/queued tasks, return the IDs instead of guessing.

Unlike `task_cancel`, a stopped target emits a failed update with `error.code: "TASK_STOPPED"`. A direct stop request gets a normal direct-message reply. A stop request delivered as a task becomes a separate control task with working/completed updates and no artifact upload. Exclude that control task from the tasks it stops.

### Connection loss

Bind every task and send closure to the socket that accepted it. When that connection closes, abort unfinished tasks and mark them failed locally. Do not send their updates over a new socket, resume them automatically, or publish late artifacts. Remove scoped credentials. Persisting a failure locally does not imply that Office received a failure message.

Reconnect delay is `min(maxDelay, minDelay × 2^min(attempts, 10))`, initially 1 second and capped at 30 seconds by default. Increment attempts after scheduling; reset on `registered`. The reference adds no jitter. Reconnect creates a fresh socket and sends registration again.

| Close code | Reference behavior |
| --- | --- |
| `4001` | Worker was replaced by a newer connection; enter `replaced` and do not reconnect. |
| `4000`, `4002`, other remote closes | Retry with backoff, including a remote normal close while the client is still enabled. |
| Local shutdown | Disable reconnect; close with `1000`, reason `Worker shutting down`. |

On process startup, persisted `submitted`/`working` tasks become locally failed with `TASK_INTERRUPTED`; they are not resumed or replayed to Office. Completed history is retained. The adapter should explicitly cancel running harness operations during graceful shutdown; closing only the transport is not a complete subprocess cleanup strategy.

Reference edge case: its disconnect loop excludes only `completed` and `failed`, so a previously `cancelled` record can later become locally `failed` on disconnect. Preserve cancellation as terminal in a new implementation; no extra wire message is needed.

## 9. Task-scoped Office MCP

The `mcpServers` object supplies project read access for an assignment. Missing or `null` means no Office MCP; a supplied empty object is invalid. Accept 1–8 entries. Labels match `^[A-Za-z][A-Za-z0-9_-]{0,31}$`; each entry must have `type: "http"`, a string URL, and a Bearer authorization header.

Resolve relative URLs against the Office WebSocket origin converted to HTTP(S). Require the same origin and path `/mcp/projects/<projectId>/tasks/<taskId>`, with the decoded task segment equal to the assigned worker task ID. Reject credentials, query parameters, fragments, and redirects. Forward only the `Authorization` header from the supplied configuration, not arbitrary routing headers.

The reference binds tool calls to the project encoded in the validated MCP URL. It rejects a tool argument requesting another project and injects that URL project into each call. It does not separately compare the URL project to the assignment's top-level `projectId`; a new adapter should check consistency if its Office deployment guarantees those values agree.

### Implemented HTTP MCP exchange

This describes this repository's client, not a general claim about all current MCP implementations.

1. POST JSON-RPC `initialize` with a unique numeric `id` and params:

   ```json
   {
     "protocolVersion": "2025-11-25",
     "capabilities": {},
     "clientInfo": { "name": "agent-worker", "version": "1.0.0" }
   }
   ```

2. Send `Authorization`, `Content-Type: application/json`, and `Accept: application/json, text/event-stream`.
3. Save an optional `MCP-Session-Id` response header. Accept negotiated versions `2025-11-25`, `2025-06-18`, or `2025-03-26`.
4. Send `notifications/initialized` without an ID. Add `MCP-Protocol-Version` and the session header, if any, on subsequent requests.
5. Call `tools/list`; follow `nextCursor` until exhausted. Reject repeated cursors or more than 20 distinct continuation cursors.
6. Expose the allowed project tools to the harness using returned `description` and `inputSchema`. The built-in bridge names tools `<serverLabel>__<toolName>`.
7. Execute `tools/call` with `{ "name": "project_get_context", "arguments": { "projectId": "project-alpha" } }`, or another discovered allowed tool.
8. On cleanup, abort active requests and best-effort DELETE the endpoint with authorization, protocol version, and session ID. Session deletion has a 2-second timeout.

The bridge allows only `project_get_context`, `project_list_files`, `project_read_file`, `project_list_agents`, and `project_conversation_summary`. No supported discovered tools means setup failure. Each request has a 30-second timeout and a 4 MiB response limit. It accepts JSON or finite SSE responses, selecting the JSON-RPC object with the matching request ID; it does not implement a persistent resumable SSE event stream.

Treat tool results as untrusted data. Read project context before changing existing work, follow file pagination (`nextOffset`) when provided, resolve `/files/` references against Office's HTTP origin, and write deliverables into the local workspace. Office project files and the local task directory are different storage locations.

For CLI harnesses, either expose this validated bridge or configure the CLI's native HTTP MCP client using the normalized servers. The existing CLI adapters use native MCP configurations after shell preflight; the bridge allowlist is not automatically enforced inside those native clients. Apply native tool restrictions if equivalent restrictions are required.

## 10. Optional Office context and teammate APIs

These are independent HTTP features, not necessary for the base WebSocket connection. Confirm support in the target Office deployment. The source for teammate collaboration includes Office-side requirements, not proof that a particular server has implemented them.

| Feature | Request / expected payload |
| --- | --- |
| Completed task summaries | `GET /api/memory?projectId=...&kind=task&status=completed&limit=20&query=...`; read `records`. |
| Full task details | `GET /api/tasks?projectId=...`; search `tasks` by `id`, `messageId`, or `workerTaskId`. |
| Recent chat | `GET /api/chat?projectId=...&limit=20`; read `messages` and `members`. |
| Teammate discovery | `GET /api/sub-agents`; read `workers` and filter out the caller. |
| Create consultation/subtask | `POST /api/delegations`; body shown below. |
| Poll delegation | `GET /api/delegations?id=<delegationId>`; read `delegation.state`. |

Use `Authorization: Bearer <sharedWorkerToken>` and `Accept: application/json`. Collaboration additionally sends `X-Agent-Name: <registeredName>`; the context reader does not. Use the assigned project for project-scoped history queries. Context reads default to 20 records, clamp limits to 1–100, use a 30-second timeout, and cap responses at 5 MiB.

```json
{
  "teammate": "Researcher",
  "request": "Verify the proposed API behavior and return sources.",
  "reason": "This agent advertises research expertise.",
  "mode": "consult",
  "timeoutSeconds": 120
}
```

`consult` maps to a direct question; `task` maps to an assignment. The expected acceptance is HTTP 202 with `{ "ok": true, "delegation": { "id": "help-123", "state": "queued" } }`. Poll once per second until `completed`, `failed`, `cancelled`, or `timed_out`. A completed record may contain `response.text` and task artifacts. Timeout defaults to 120 seconds and is bounded to 5–600 seconds; request text is capped at 100,000 characters. Aborting the local wait does not issue a remote cancellation request.

Office must enforce requester authorization, target availability, delegation timeouts, and loop/depth protection. Full server expectations are in [OFFICE-COLLABORATION.md](OFFICE-COLLABORATION.md). A new harness must not assume these protections exist merely because it can send the HTTP requests.

## 11. Connectivity probes and local observability

After every successful registration, the reference shell starts two non-blocking probes:

- Upload `test.md` through the legacy workspace-upload API, using the shared worker token. This creates an Office file and logs success/failure.
- Initialize MCP at `/mcp/projects/central-office/tasks/worker-connectivity` using the shared worker token, discover tools, call `project_get_context`, and close the session. The probe has a 30-second lifetime and is cancelled when superseded/disconnected.

Probe failure does not disconnect the worker or block task execution. These probes are optional for another harness and may require permissions beyond task-scoped execution. Do not attach the probe's broad credential to task tools.

The reference exposes local `GET /health`, `/api/status`, `/api/info`, `/.well-known/agent-card.json`, `/workspace/<taskId>/<path>`, and `/workspace/<taskId>.zip`. A dashboard is not a protocol requirement. Worker-hosted file links need a reachable public URL; preferred artifact handoff instead uses the uploaded Office artifact ID. Do not interpret successful registration as evidence that worker file links are reachable.

Track connection status, queue depth, active IDs, execution state, upload failures, MCP state, and delivery failures. Retain task history without retaining `artifactUpload` or `mcpServers` credentials. The reference stores message text separately from task history, so logs/history should also be treated as potentially sensitive.

## 12. Harness adapter interface

When reusing this repository, the shell calls an asynchronous function with this effective contract:

```ts
type HarnessContext = {
  workspace: string;
  resourceBaseUrl: string;
  projectId: string;
  officeTaskId: string | null;
  signal?: AbortSignal;
  executionControl?: {
    signal: AbortSignal;
    stop(reason: string): boolean;
    waitIfPaused(): Promise<void>;
  };
  officeMcpTools?: Array<{
    name: string;
    description: string;
    parameters: object;
    execute(args: object, context?: { signal?: AbortSignal }): Promise<unknown>;
  }>;
  officeMcpServers?: Array<{
    label: string;
    url: string;
    headers: { Authorization: string };
  }>;
  officeHttpOrigin?: string;
  onActivity?: (event: object) => void;
};

type RunTask = (prompt: string, context: HarnessContext) => Promise<string>;
```

Some context fields are absent for direct messages; do not require task MCP or a signal for that path. Worker `taskId` is not passed as a separate runner field; the shell owns execution identity. The return value is final Markdown, not a protocol envelope. Throw on execution failure. The shell writes `output.md`, uploads, and sends updates.

Minimal wiring (within this repository):

```js
import { createWorkerServer, ensureWorkerWorkspace } from './lib/worker.js';

const env = { ...process.env };
await ensureWorkerWorkspace(env);
const server = createWorkerServer({
  env,
  async runTask(prompt, context) {
    // Implement runMyHarness using your CLI or SDK.
    // It must return final text and honor cancellation when supplied.
    return await runMyHarness(prompt, context);
  },
});
server.listen(Number(env.PORT || 3000), env.HOST || '0.0.0.0');
```

This wiring is illustrative; `runMyHarness` must be implemented. Registration begins on the server's `listening` event. The reference runtime requires Node.js 22.5 or newer, including native WebSocket and `node:sqlite` support; this is not a requirement for independent protocol clients.

The harness implementation should:

1. Start an isolated conversation/session per assignment and direct message.
2. Use `context.workspace` as working directory and keep deliverables within it.
3. Pass the prompt as data (stdin/SDK arguments), not interpolated shell code.
4. Use locally configured model authentication; do not confuse model credentials with Office credentials.
5. Set unattended tool policy explicitly and enforce the intended sandbox independently of Office registration.
6. Honor `signal` by aborting the SDK call or terminating the child process; escalate termination after a bounded grace period if necessary.
7. Bound captured output and require a non-empty final answer.
8. Keep temporary MCP configuration outside the workspace, restrict its permissions, and delete it on exit.
9. Strip task tokens from errors, final text, and telemetry. Prefer an allowlisted child environment; the reference CLI adapters inherit the configured environment, which is not a credential-isolation boundary.

`codex.js` demonstrates final-response-file extraction and per-task MCP header environment variables. `claude.js` demonstrates a temporary mode-0600 MCP JSON configuration and structured final-response parsing. Their CLI flags are examples from this checkout, not a specification for another harness's command line.

## 13. Independent adapter control flow

```text
start:
  validate configuration; open local history/workspace
  connect Office socket

on socket open:
  send register; start heartbeat timer

on registered:
  record connection generation + ID; reset backoff

on task:
  validate identity and text; reject duplicates
  bind job + scoped credentials + send function to this generation
  intercept control commands, otherwise enqueue and return

task runner:
  if no longer active: return
  send working
  create workspace; initialize supplied MCP
  run harness with abort signal
  if no longer active: return
  write output.md; build ZIP; upload with scoped token
  if no longer active: return
  atomically claim completed terminal state; send completion
  on execution/upload error, if still active:
    atomically claim failed terminal state; send failure
  finally close MCP and erase scoped credentials

on task_cancel:
  verify task + generation + optional correlation
  atomically mark cancelled; abort; suppress future publication

on direct_message:
  handle stop commands or enqueue isolated direct answer
  send one correlated plain-text response on original socket

on disconnect:
  invalidate generation; clear heartbeat
  abort outstanding work; persist local interruption
  do not replay old results; reconnect unless replaced/stopped
```

Use a terminal-state guard around upload/completion races. A process that ignores cancellation must still be unable to publish its eventual output. An upload that succeeded just before cancellation may leave a staged artifact; the worker has no cleanup API in this contract, and must not publish a completed update afterward.

## 14. Acceptance checklist

Validate with a mock Office and then a target deployment before treating an adapter as compatible:

- Registration is the first application message, uses the configured name/token, and does not expose credentials in URLs.
- Tasks/direct messages before registration are rejected; registered workers remain responsive during long executions.
- Multiple task text parts are combined correctly; invalid IDs, missing text, duplicate task IDs, oversized messages, and binary messages are rejected.
- Working and terminal updates preserve both task ID and original message correlation; outgoing message IDs are fresh.
- Concurrent tasks use isolated directories and conversations; direct messages use their independent queue.
- Successful task upload sends raw bytes with the assignment token, validates artifact ID, then emits completion by ID only.
- Wrong-origin/path/task upload URLs, duplicate `taskId` query values, redirects, bad tokens, oversized ZIPs, timeouts, and malformed responses fail without legacy fallback.
- Legacy upload is selected only when `artifactUpload` is absent and uses its distinct MIME type and shared-token headers.
- Failure updates contain no deliverables. Oversized final text is handled before send (keep full content in artifacts and a bounded final summary).
- Direct replies use plain text, preserve correlation, create no task/artifact, and clean temporary workspaces.
- Office cancellation during queueing, execution, and upload aborts work and emits no late terminal response.
- User stop commands return a response and fail their target with `TASK_STOPPED`; ambiguous targets are not guessed.
- Socket loss aborts owned tasks; a replacement socket never sends old results. `4001` does not reconnect; other closes back off and re-register.
- Restart retains history but never resumes interrupted work automatically.
- MCP validates origin/task binding, negotiates supported versions, follows bounded tool pagination, uses the assigned project, and releases credentials/sessions.
- Tokens are absent from public status, persisted task records, result text, logs, telemetry, and ZIP contents.
- Optional probes fail independently of the main connection; optional collaboration endpoints are verified separately.

Relevant existing automated suites can be run with:

```sh
node --test test/office-connection.test.js test/worker.test.js test/office-upload.test.js test/office-mcp.test.js test/codex.test.js test/claude.test.js test/read-office-context.test.js test/team-collaboration.test.js
```

These suites test this implementation, often with mocked Office behavior; passing them does not certify another Office deployment or another harness.

## 15. Source map

| Concern | Source |
| --- | --- |
| WS URL validation, registration, dispatcher, heartbeat, reconnect | [lib/office-connection.js](../lib/office-connection.js) |
| Metadata, assignment validation, queues, runner interface, result envelopes, cancellation | [lib/worker.js](../lib/worker.js) |
| Scoped/legacy artifact upload and connectivity file | [lib/office-upload.js](../lib/office-upload.js) |
| MCP normalization, client, allowlist, probe | [lib/office-mcp.js](../lib/office-mcp.js) |
| ZIP layout and traversal | [lib/zip-workspace.js](../lib/zip-workspace.js) |
| History persistence | [lib/task-store.js](../lib/task-store.js) |
| Context reads / teammate calls | [read-office-context.js](../lib/tools/read-office-context.js), [team-collaboration.js](../lib/tools/team-collaboration.js) |
| Existing harness adapters | [codex.js](../codex.js), [claude.js](../claude.js) |
| Optional Office-side delegation contract | [OFFICE-COLLABORATION.md](OFFICE-COLLABORATION.md) |

When older prose differs from executable behavior, use the source baseline and this document's explicit compatibility notes. Before adding retries, resume, new status states, or new acknowledgement semantics, coordinate that extension with Office; those mechanisms are not negotiated by the current protocol.
