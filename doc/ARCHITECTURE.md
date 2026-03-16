# Professor_AI Architecture (v3)

Updated: 2026-03-16
Incorporates security audit findings, performance optimizations, and operational improvements.

## Overview

Professor_AI is a Unix daemon (`professord`) that wraps llama.cpp to serve a local LLM via an OpenAI-compatible REST API. Written in pure C (C11), targets AMD GPUs via ROCm, serves a single primary client (Hermes on Jetson Orin Nano) over LAN.

## Component Diagram

```mermaid
graph TB
    subgraph professord["professord process"]
        main["main.c"]
        config["config<br/>(INI + CLI)"]
        logger["logger<br/>(stderr + file)"]
        daemon["daemon<br/>(signals, PID, optional fork)"]
        worker["worker<br/>(job queue, token ring)"]
        inference["inference engine"]
        server["server<br/>(auth, routing, SSE)"]
        api_types["api_types<br/>(parse / serialize)"]
    end

    subgraph external["External Libraries"]
        llama["llama.cpp C API<br/>(model, context, sampler, vocab)"]
        mongoose["mongoose<br/>(HTTP, SSE)"]
        cjson["cJSON<br/>(JSON lib)"]
    end

    main --> config
    main --> logger
    main --> daemon
    main --> worker
    main --> inference
    main --> server

    config --> logger
    worker --> inference
    inference --> llama
    server --> mongoose
    server --> worker
    server --> api_types
    api_types --> cjson

    logger -.->|used by all| worker
    logger -.->|used by all| server
    logger -.->|used by all| inference
```

## Module Dependency Graph

```mermaid
graph TD
    main --> config
    main --> logger
    main --> daemon
    main --> inference
    main --> worker
    main --> server
    main --> api_types
    main --> recommend

    config --> logger
    recommend -.->|reads /proc, /sys| nothing2["(no deps)"]
    inference --> llama_cpp["llama.cpp"]
    inference --> config
    inference --> logger
    inference --> api_types
    inference --> daemon["daemon (g_shutdown_requested)"]
    worker --> inference
    worker --> logger
    worker --> api_types
    worker --> daemon
    server --> mongoose
    server --> config
    server --> logger
    server --> api_types
    server --> worker
    api_types --> cJSON

    daemon -.->|POSIX only| nothing["(no deps)"]
    logger -.-> nothing
```

No circular dependencies. Build order follows dependency graph bottom-up.

## Threading Model

```mermaid
graph TD
    subgraph main_thread["Thread 1: Event Loop (main)"]
        evloop["mg_mgr_poll()"]
        evloop -->|MG_EV_HTTP_MSG| parse["parse + auth + route"]
        parse -->|inference request| submit["worker_submit(job)"]
        submit -->|busy| reject["503 Server Busy"]
        submit -->|accepted| wait["store job on connection"]
        evloop -->|MG_EV_WAKEUP| drain["drain token ring<br/>send SSE chunks<br/>or build final response"]
        evloop -->|MG_EV_CLOSE| cancel["set job->cancel = 1"]
        evloop -->|health/models| respond["respond immediately"]
    end

    subgraph worker_thread["Thread 2: Inference Worker"]
        wloop["wait for job (condvar)"]
        wloop --> gen["inference_complete / inference_chat"]
        gen -->|per token| ring["write to token ring<br/>mg_wakeup()"]
        gen -->|per token| check["check cancel + shutdown + timeout"]
        gen -->|done| done["set job->state = DONE<br/>mg_wakeup()"]
        done --> wloop
    end

    submit -.->|condvar signal| wloop
    ring -.->|mg_wakeup| evloop
    cancel -.->|cancel_flag| check
```

**Two-thread design**: The main thread runs the mongoose event loop handling all network I/O, parsing, authentication, SSE writes, and health checks. The worker thread runs exactly one inference job at a time. Communication uses a token ring buffer (protected by mutex + condvar) and `mg_wakeup()` to notify the event loop. `job->state` is `atomic_int` for safe cross-thread reads without requiring the mutex.

**Why two threads**: Even though only one GPU context exists (concurrent inference is impossible), decoupling inference from the event loop provides:

1. **Responsive health checks** -- never blocked by inference
2. **Disconnect detection** -- main thread sees `MG_EV_CLOSE` immediately, sets cancel flag
3. **Clean shutdown** -- SIGTERM is checked per-token, not after generation completes
4. **Backpressure** -- slow clients cause the ring buffer to fill; worker blocks briefly then checks cancellation, preventing unbounded memory growth

**Concurrency policy**: Exactly one inference at a time. No queue. Second request while worker is busy returns 503 immediately.

## Data Flow

### Non-Streaming Request

```mermaid
sequenceDiagram
    participant C as Client
    participant EV as Event Loop (main)
    participant W as Worker Thread
    participant L as llama.cpp

    C->>EV: HTTP POST /v1/chat/completions
    EV->>EV: auth check
    EV->>EV: parse JSON (api_types)
    EV->>W: worker_submit(job, stream=false)
    Note over EV: event loop continues polling

    W->>L: tokenize + decode prompt
    loop generation
        W->>L: sample token
        W->>W: append to response_buf
        W->>W: check cancel/shutdown/timeout
    end
    W->>EV: mg_wakeup (job done)

    EV->>EV: build JSON response from response_buf
    EV->>C: HTTP 200 JSON
    EV->>EV: log_access
```

### Streaming Request (SSE)

```mermaid
sequenceDiagram
    participant C as Client
    participant EV as Event Loop (main)
    participant W as Worker Thread
    participant L as llama.cpp

    C->>EV: HTTP POST (stream: true)
    EV->>EV: auth check
    EV->>EV: parse JSON, send SSE headers
    EV->>W: worker_submit(job, stream=true)
    Note over EV: event loop continues polling

    loop per token
        W->>L: sample token
        W->>W: write token to ring buffer
        W->>W: check cancel/shutdown/timeout (every 32 tokens)
        Note over W,EV: mg_wakeup batched:<br/>on empty->non-empty or every 8 tokens
        W->>EV: mg_wakeup (batched)
        EV->>EV: batch-drain ring (single lock, up to 32 tokens)
        EV->>C: data: {"delta":...} (direct format, no cJSON)
    end

    W->>EV: mg_wakeup (done)
    EV->>C: data: [DONE]
    EV->>EV: log_access

    Note over EV,C: If client disconnects:
    C--xEV: TCP close
    EV->>W: set job->cancel=1, job->orphaned=1
    W->>W: sees cancel, breaks loop
    W->>W: frees orphaned job
```

## Request State Machine

```mermaid
stateDiagram-v2
    [*] --> received
    received --> rejected_auth: 401 auth failed
    received --> rejected_invalid: 400 parse/validation failed
    received --> rejected_busy: 503 worker occupied
    received --> rejected_method: 405 wrong HTTP method
    received --> running: worker accepts job

    running --> streaming: first token (SSE mode)
    running --> completed: response_buf complete (non-SSE)
    streaming --> completed: finish_reason=stop/length

    running --> aborted: SIGTERM or client disconnect
    running --> timed_out: wall-clock limit exceeded
    running --> failed: llama_decode error
    streaming --> aborted: SIGTERM or client disconnect
    streaming --> timed_out: wall-clock limit exceeded
    streaming --> failed: llama_decode error

    rejected_auth --> [*]
    rejected_invalid --> [*]
    rejected_busy --> [*]
    rejected_method --> [*]
    completed --> [*]
    aborted --> [*]
    timed_out --> [*]
    failed --> [*]
```

**Per-token checks in generation loop** (priority order):
1. `g_shutdown_requested` -> finish_reason = `"abort"`
2. `cancel_flag` (disconnect) -> finish_reason = `"abort"`
3. elapsed > `max_inference_seconds` -> finish_reason = `"time_limit"` (checked every 32 tokens)
4. `llama_decode` return != 0 -> finish_reason = `"backend_error"`
5. token == EOS -> finish_reason = `"stop"`
6. stop sequence suffix match (rolling window with pre-computed lengths) -> finish_reason = `"stop"`
7. `n_generated >= max_tokens` -> finish_reason = `"length"`

## API Contract

Base URL: `http://<host>:8080`

Default bind: `127.0.0.1:8080` (loopback). Must explicitly configure `listen_addr` for LAN access.

### Authentication

If `api_key` is set in config, all endpoints except `/health` and `/v1/health` require:

```
Authorization: Bearer <api_key>
```

Missing or invalid key returns 401. Empty `api_key` in config disables authentication.

### Endpoints

#### `GET /health` or `GET /v1/health`

Health check. No auth required.

**Response**:
```json
{"status": "ok"}
```

#### `GET /v1/models`

List available models.

**Response**:
```json
{
  "object": "list",
  "data": [
    {
      "id": "<model_alias>",
      "object": "model",
      "created": 0,
      "owned_by": "local"
    }
  ]
}
```

#### `POST /v1/chat/completions`

Chat completion (OpenAI-compatible).

**Request**:
```json
{
  "model": "local-model",
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Hello"}
  ],
  "temperature": 0.7,
  "top_p": 0.9,
  "max_tokens": 512,
  "stream": false,
  "stop": ["\n\n"]
}
```

All fields except `messages` are optional. `model` is accepted but ignored (single model).

**Constraints**:
- `messages`: max `PROF_MAX_MESSAGES` (64). Roles must be `system`, `user`, or `assistant`.
- `max_tokens`: capped to `n_ctx - prompt_tokens` at runtime.
- `stop`: max 4 sequences, 64 chars each.

**Response** (non-streaming):
```json
{
  "id": "chatcmpl-abc123",
  "object": "chat.completion",
  "created": 1710000000,
  "model": "local-model",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "Hello! How can I help you?"
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 12,
    "completion_tokens": 8,
    "total_tokens": 20
  }
}
```

**Response** (streaming, `stream: true`):
```
data: {"id":"chatcmpl-abc123","object":"chat.completion.chunk","created":1710000000,"model":"local-model","choices":[{"index":0,"delta":{"role":"assistant","content":"Hello"},"finish_reason":null}]}

data: {"id":"chatcmpl-abc123","object":"chat.completion.chunk","created":1710000000,"model":"local-model","choices":[{"index":0,"delta":{"content":"!"},"finish_reason":null}]}

data: {"id":"chatcmpl-abc123","object":"chat.completion.chunk","created":1710000000,"model":"local-model","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: [DONE]
```

#### `POST /v1/completions`

Text completion (OpenAI-compatible).

**Request**:
```json
{
  "model": "local-model",
  "prompt": "Once upon a time",
  "temperature": 0.7,
  "max_tokens": 256,
  "stream": false
}
```

**Response** (non-streaming):
```json
{
  "id": "cmpl-abc123",
  "object": "text_completion",
  "created": 1710000000,
  "model": "local-model",
  "choices": [
    {
      "index": 0,
      "text": " there was a brave knight...",
      "finish_reason": "length"
    }
  ],
  "usage": {
    "prompt_tokens": 5,
    "completion_tokens": 256,
    "total_tokens": 261
  }
}
```

### Error Responses

All errors return OpenAI-format JSON:

```json
{
  "error": {
    "message": "Invalid JSON in request body",
    "type": "invalid_json",
    "code": 400
  }
}
```

| HTTP | `type` | Condition |
|------|--------|-----------|
| 400 | `invalid_json` | Malformed JSON body |
| 400 | `invalid_request` | Missing fields, bad values, too many messages, unknown role, prompt too long |
| 401 | `unauthorized` | Bad/missing API key |
| 405 | `method_not_allowed` | Wrong HTTP method for endpoint |
| 404 | `not_found` | Unknown endpoint |
| 503 | `server_busy` | Worker thread occupied, try again |
| 500 | `backend_error` | llama_decode failure, internal error |

### Sampling Parameters

| Parameter | Type | Default | Clamp Range | Notes |
|-----------|------|---------|-------------|-------|
| `temperature` | float | 0.7 | [0.0, 2.0] | 0 = greedy decoding |
| `top_p` | float | 0.9 | (0.0, 1.0] | Nucleus sampling |
| `top_k` | int | 40 | [1, vocab] | Top-k sampling |
| `max_tokens` | int | 512 | [1, n_ctx-prompt] | Capped at runtime |
| `repeat_penalty` | float | 1.1 | [1.0, 2.0] | llama.cpp native |
| `frequency_penalty` | float | 0.0 | [0.0, 2.0] | OpenAI-compatible |
| `presence_penalty` | float | 0.0 | [0.0, 2.0] | OpenAI-compatible |
| `stop` | string[] | [] | max 4, 64 chars | Stop sequences |

Per-request parameters override config defaults. Omitted parameters (sentinel `NAN` / `-1`) fall back to config values. NaN and infinity in float fields are replaced with config defaults.

## Prompt Construction Contract

### Role Validation

- Accepted roles: `system`, `user`, `assistant`
- Unknown roles: rejected at parse time (400 `invalid_request`)
- Empty `content`: allowed (enables assistant prefill for guided generation)

### Template Application

1. Extract chat template from GGUF metadata via `llama_model_chat_template(model, NULL)`
2. If NULL, fall back to ChatML:
   ```
   <|im_start|>system\n{content}<|im_end|>\n
   <|im_start|>user\n{content}<|im_end|>\n
   <|im_start|>assistant\n
   ```
3. Apply template via `llama_chat_apply_template()` with `add_assistant=true`
4. Heap-allocate output buffer based on sizing call

### Tokenization

- `add_special=false`: the chat template already includes BOS/EOS/special tokens
- `parse_special=true`: the tokenizer must recognize special tokens embedded by the template

### Context Budget

```mermaid
flowchart TD
    A["render prompt via template"] --> B["tokenize"]
    B --> C{prompt_tokens >= n_ctx?}
    C -->|Yes| D["400: prompt too long"]
    C -->|No| E{prompt_tokens + max_tokens > n_ctx?}
    E -->|Yes| F["reduce max_tokens = n_ctx - prompt_tokens"]
    E -->|No| G["proceed with generation"]
    F --> G
```

- If the rendered prompt alone exceeds `n_ctx`: return 400 with explicit error message including token counts.
- If `prompt_tokens + max_tokens > n_ctx`: silently reduce `max_tokens` to fit. The response `usage` field shows actual counts.
- Prompt content is never silently truncated. The client is responsible for managing conversation length.

## Resource Budgets

```mermaid
flowchart LR
    subgraph layer1["Layer 1: HTTP"]
        body["body <= 1 MB"]
        conn["connections <= 32"]
    end

    subgraph layer2["Layer 2: Parse"]
        msgs["messages <= 64"]
        role["role in system/user/assistant"]
        stop["stop <= 4 x 64 chars"]
    end

    subgraph layer3["Layer 3: Render"]
        prompt["rendered <= 256 KB"]
    end

    subgraph layer4["Layer 4: Inference"]
        tokens["prompt_tokens < n_ctx"]
        budget["max_tokens <= n_ctx - prompt_tokens"]
        time["elapsed <= max_inference_seconds"]
    end

    layer1 --> layer2 --> layer3 --> layer4
```

Each layer rejects early. Expensive operations (template rendering, tokenization, inference) only run after cheaper checks pass.

## Configuration

Two sources, later overrides earlier:

1. **INI file** (`--config /path/to/file.ini`)
2. **CLI arguments** (`--model`, `--n-ctx`, `--api-key`, etc.)

CLI `--config` is extracted with a targeted scan, then the INI file is loaded, then CLI is parsed once to override INI values. This avoids double-parsing which caused accumulating options (like `--allow-ip`) to duplicate.

Default bind address is `127.0.0.1:8080` (loopback). LAN binding requires explicit `listen_addr = 0.0.0.0:8080` in config.

See `etc/professord.ini.example` for all options.

## Process Lifecycle

```mermaid
flowchart TD
    start([startup]) --> config_phase["parse config<br/>(defaults -> INI -> CLI)"]
    config_phase --> validate["validate config"]
    validate --> logger_start["init logger"]
    logger_start --> signals["install signal handlers"]
    signals --> model_load["load model (llama.cpp)<br/>-- BEFORE daemonize --"]

    model_load -->|failure| exit_fail([exit 1 -- caller sees failure])
    model_load -->|success| fork_check{daemonize?}

    fork_check -->|No| worker_start
    fork_check -->|Yes| dfork["double-fork<br/>write PID file"]
    dfork --> worker_start

    worker_start["start worker thread"]
    worker_start --> server_start["bind HTTP port (mongoose)"]
    server_start --> notify["sd_notify(READY=1)"]
    notify --> evloop["event loop<br/>(mg_mgr_poll)<br/>blocks until g_shutdown_requested"]

    evloop --> cleanup["cleanup:<br/>server_destroy<br/>worker_destroy (join thread)<br/>inference_destroy (free model, backend)<br/>daemon_remove_pidfile<br/>logger_destroy"]
    cleanup --> exit_ok([exit 0])
```

## Memory Management Strategy

- **Stack-allocated structs**: `config_t`, `logger_t`, `inference_engine_t`, `worker_t` are all owned by `main()` on the stack.
- **Fixed-size buffers**: Most strings use `char[N]` to avoid per-field malloc/free. All copies use `snprintf`, never `strncpy`.
- **Minimal heap usage**:
  - `chat_request_t.messages` -- heap array (capped at `PROF_MAX_MESSAGES`), freed by `chat_request_free()`
  - `*_to_json()` return heap strings -- caller frees with `free()`
  - `llama_chat_apply_template` output -- heap buffer sized by first call, freed after tokenization
  - Non-streaming response buffer -- heap with realloc-double, freed after send
  - llama.cpp model/context -- allocated by llama.cpp, freed by `inference_destroy()`
- **No heap in hot path**: Logger uses stack buffers. Token ring buffer is pre-allocated in `worker_job_t`. SSE streaming uses direct `mg_printf` formatting instead of cJSON allocation per token.

```mermaid
graph LR
    subgraph stack["Stack (main)"]
        cfg["config_t"]
        lg["logger_t"]
        eng["inference_engine_t"]
        wrk["worker_t"]
    end

    subgraph heap["Heap"]
        msgs["chat_request_t.messages<br/>(capped, freed by chat_request_free)"]
        tmpl["template output buffer<br/>(sized by first call, freed after tokenize)"]
        resp["response_buf / JSON strings<br/>(freed after send)"]
        model["llama model + context<br/>(freed by inference_destroy)"]
    end

    eng -->|owns| model
    wrk -->|owns during job| resp
```

## Build System

CMake with FetchContent for dependencies. See `doc/PLAN.md` for full CMakeLists.txt and build commands.

```bash
# Standard build
cmake -B build -DGGML_HIP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# With systemd readiness
cmake -B build -DGGML_HIP=ON -DPROF_USE_SYSTEMD=ON -DCMAKE_BUILD_TYPE=Release

# Run
./build/professord --model /path/to/model.gguf

# Run on LAN
./build/professord --model /path/to/model.gguf --listen-addr 0.0.0.0:8080 --api-key mysecretkey
```

## Security Measures

- **Constant-time auth**: API key comparison always runs full expected-length loop to prevent timing side-channel attacks on key length.
- **IP ACL normalization**: IPv4-mapped IPv6 addresses (`::ffff:127.0.0.1`) are normalized before ACL comparison to prevent bypass.
- **PID file safety**: Stale PID files are validated (checks if old PID is still running) before overwriting. Uses `O_EXCL` to prevent symlink attacks.
- **Atomic job state**: `job->state` uses `atomic_int` to prevent data races between worker and event loop threads.
- **Orphaned job cleanup**: When a client disconnects during inference, the job is marked `orphaned` and freed by the worker thread after inference completes, preventing memory leaks.
- **Bounds checking**: Log level validated before array indexing. cJSON creation checked for NULL on all serialization paths. Sampler chain checked for NULL after allocation.
- **Daemon hardening**: Daemonize fails if `/dev/null` cannot be opened instead of silently continuing with inherited file descriptors.
- **Log routing**: llama.cpp and mongoose output routed through the logger to respect `--log-level` and prevent information leakage at high log levels.

## Performance Architecture

Token generation throughput is memory-bandwidth bound. The `--recommend` flag reports estimated tok/s per model based on detected hardware bandwidth.

**llama.cpp configuration**:
- `n_threads` auto-detects to `nproc/2` for optimal CPU utilization during prompt processing
- `n_batch` defaults to 2048 (matching llama.cpp default) for efficient prompt eval
- KV cache uses q8_0 quantization to halve attention memory bandwidth vs f16
- Flash attention enabled (auto mode)
- Performance counters logged per request (prompt tok/s, eval tok/s)

**Streaming hot path** (zero heap allocation per token):
- Ring buffer uses `memcpy` instead of `snprintf` for token copies
- `mg_wakeup()` batched to reduce syscalls (empty-to-non-empty transition or every 8 tokens)
- Ring drain acquires lock once for up to 32 tokens instead of per-token
- SSE chunks formatted directly via `mg_printf` with `MG_ESC` (no cJSON tree build/serialize/free per token)
- Timeout checked every 32 tokens instead of every token
- Stop sequence lengths pre-computed once before generation loop

## Testing

Test suite: `tests/test_all.sh` runs 43 tests across 5 categories and generates timestamped reports in `tests/reports/`.

| Category | Tests | Coverage |
|----------|-------|----------|
| Unit tests | 2 | config parsing, API types |
| Functional | 7 | health, models, routing, auth, error codes |
| Inference | 4 | chat, completion, streaming, admission control |
| Stress | 21 | malformed input, numeric edges, method abuse, large payloads, rapid-fire, concurrency, auth edges |
| Stability | 2 | server alive, final inference after stress |
