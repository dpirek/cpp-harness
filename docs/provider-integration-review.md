# Provider integration review

Reviewed: 2026-11-11

The supplied Vanilla Harness specification describes a different JavaScript repository. Its source map, SQLite records, browser/CLI APIs, presets, provider identities, and three protocol adapters do not describe this native Windows C++ application. Those sections should not be used as its implementation baseline.

This application saves one connection (URL, model, DPAPI-protected key) in `provider-settings.json`. Settings saves synchronously and displays failures while keeping the dialog open. In-flight requests retain their configuration. No state API exposes saved credentials to a browser. Discovery uses a 15-second deadline and a 4 MiB response limit; inference has a 120-second total deadline, cancellation, redirect refusal, and a 4 MiB limit. Provider error bodies are not displayed because they can disclose request details.

Implemented improvements:

- Both discovery and inference accept a base URL or full Chat Completions URL. Trailing slashes are normalized. Inference preserves query parameters; discovery retains the existing behavior of dropping them.
- Empty credentials are valid, survive encrypted persistence, and omit Authorization on both request paths. Saved settings remain authoritative and do not borrow environment credentials.
- Generic `AI_API_KEY`, `AI_MODEL`, and `AI_BASE_URL` settings support compatible providers while preserving legacy OpenRouter aliases. Shell values outrank `.env`; generic names outrank legacy names within each source. New API key environment variables are excluded from PowerShell child processes.
- URL and header validation runs before inference as well as discovery and persistence. Only HTTP(S) URLs without embedded credentials, fragments, or whitespace are accepted.

Validation uses loopback HTTP fixtures for authenticated and anonymous discovery/inference, normalization, malformed catalogs, cancellation, and existing stream/tool behavior. Persistence tests check DPAPI and saved empty-key behavior. No live provider credentials are needed.

Remaining scope: multi-connection identity, model capability metadata, protocol-specific Responses/native Ollama adapters, structured error categories, connection probes distinct from benchmarks, and retries. These require separate implementation; the supplied document's proposed requirements must remain proposals here. Do not label a Chat Completions endpoint as Responses compatible based on `/models` access alone.
