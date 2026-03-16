# Professor_AI

A Unix daemon (`professord`) that wraps [llama.cpp](https://github.com/ggml-org/llama.cpp) to serve a local LLM via an OpenAI-compatible REST API. Written in pure C (C11).

Designed for a single-GPU host (AMD ROCm) serving one primary client (Hermes on Jetson Orin Nano) over LAN.

## Features

- OpenAI-compatible `/v1/chat/completions` and `/v1/completions` endpoints
- Server-Sent Events (SSE) streaming with backpressure handling
- Two-thread architecture: event loop + dedicated inference worker
- Chat template auto-detection from GGUF model metadata
- Bearer token authentication
- Admission control (503 when busy)
- Per-token cancellation on disconnect, shutdown, or timeout
- Runtime stats endpoint (`/v1/stats`) and periodic log reporting
- Hardware detection and model recommendations (`--recommend`)
- INI config file + CLI argument overrides
- Optional daemonization with syslog logging
- systemd `Type=notify` with readiness signaling

## Quick Start

```bash
# Build (auto-downloads dependencies, ROCm GPU acceleration)
cmake -B build -DGGML_HIP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# See what models are recommended for your hardware
./build/professord --recommend

# Run (binds to localhost by default)
./build/professord --model /path/to/model.gguf

# Run on LAN with auth
./build/professord --model /path/to/model.gguf \
  --listen-addr 0.0.0.0:8080 --api-key mysecretkey

# Test
curl http://localhost:8080/health
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}]}'
```

See [INSTALL](INSTALL) for detailed build and installation instructions.

## Configuration

Professor_AI reads configuration from an INI file and/or CLI arguments. CLI always overrides the INI file.

```bash
# With config file
./build/professord --config /etc/professord.ini

# CLI overrides
./build/professord --config /etc/professord.ini --n-ctx 8192 --temperature 0.5

# CLI only
./build/professord --model /path/to/model.gguf --listen-addr 0.0.0.0:9090
```

See [etc/professord.ini.example](etc/professord.ini.example) for all options.

### CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--config PATH` | (none) | Path to INI config file |
| `--model PATH` | (required) | Path to GGUF model file |
| `--model-alias NAME` | `local-model` | Model name reported in API responses |
| `--n-ctx N` | 4096 | Context window size (tokens) |
| `--n-gpu-layers N` | 99 | Layers to offload to GPU |
| `--n-batch N` | 512 | Batch size for prompt processing |
| `--temperature F` | 0.7 | Sampling temperature (0 = greedy) |
| `--top-p F` | 0.9 | Nucleus sampling threshold |
| `--top-k N` | 40 | Top-k sampling |
| `--repeat-penalty F` | 1.1 | Repetition penalty |
| `--max-tokens N` | 512 | Default max tokens per response |
| `--listen-addr ADDR` | `127.0.0.1:8080` | HTTP listen address |
| `--api-key KEY` | (none) | Bearer token for auth (empty = no auth) |
| `--allow-ip IP` | (none) | Add IP to ACL (repeatable; empty = allow all) |
| `--max-inference-seconds N` | 300 | Wall-clock timeout per generation |
| `--stats-interval N` | 60 | Seconds between stats log reports (0 = off) |
| `--recommend` | | Detect hardware, recommend models, and exit |
| `--daemonize` | off | Run as daemon (logs switch to syslog) |
| `--pid-file PATH` | `/run/professord/professord.pid` | PID file path |
| `--log-level N` | 2 (INFO) | 0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=FATAL |
| `--log-file PATH` | (none) | Log to file in addition to stderr/syslog |
| `--help` | | Print usage and exit |

### Logging Modes

| Mode | stderr | syslog | file |
|------|--------|--------|------|
| Foreground (default) | yes (color if tty) | no | optional `--log-file` |
| systemd | yes (captured by journal) | no | optional `--log-file` |
| `--daemonize` | no (redirected to /dev/null) | yes (`LOG_DAEMON`) | optional `--log-file` |

View syslog output from a daemonized instance: `journalctl -t professord -f`

## Connecting Hermes

This section covers configuring professord to serve Hermes (on a Jetson Orin Nano) over a LAN.

### 1. Generate a Pre-Shared Key

Generate a strong random key on the workstation:

```bash
professord --gen-api-key
```

This reads 256 bits from the kernel CSPRNG (`/dev/urandom`) and outputs a 64-character hex string. This key will be shared between professord and Hermes. Store it securely -- do not commit it to version control.

### 2. Configure professord for LAN Access

Create `/etc/professord.ini` on the workstation:

```ini
# Model
model_path = /path/to/Hermes-3-Llama-3.1-70B-Q4_K_M.gguf
model_alias = hermes-70b

# Context -- match Hermes expectations
n_ctx = 8192
n_gpu_layers = 99
max_tokens = 2048

# Bind to LAN interface (not 0.0.0.0 -- restrict to the LAN subnet)
listen_addr = 192.168.1.100:8080

# Authentication
api_key = a1b2c3d4e5f6...your-64-char-hex-key...

# IP ACL -- only allow Hermes and localhost
allow_ip = 192.168.1.50, 127.0.0.1

# Timeouts
max_inference_seconds = 300

# Stats reporting every 60 seconds
stats_interval = 60

# Logging
log_level = 2
log_file = /var/log/professord/professord.log
```

**Bind address**: Use the workstation's LAN IP (e.g., `192.168.1.100:8080`) rather than `0.0.0.0:8080`. This ensures professord only accepts connections on the LAN interface, not on any other network the workstation may be connected to.

**IP ACL**: The `allow_ip` setting restricts connections at the TCP level. IPs not on the list are dropped silently -- no HTTP response, no error, no interaction. This is defense in depth on top of the API key. In the example above, only the Hermes device (`192.168.1.50`) and localhost can connect.

Find your LAN IP with: `ip addr show | grep 'inet '`

### 3. Firewall Rules (optional, defense in depth)

The `allow_ip` setting in the config already drops unlisted IPs at the TCP level. For additional hardening, add OS-level firewall rules. Replace `192.168.1.50` with Hermes's IP:

**Using iptables:**
```bash
# Allow Hermes
sudo iptables -A INPUT -p tcp --dport 8080 -s 192.168.1.50 -j ACCEPT
# Allow localhost (for local testing)
sudo iptables -A INPUT -p tcp --dport 8080 -s 127.0.0.1 -j ACCEPT
# Drop everything else
sudo iptables -A INPUT -p tcp --dport 8080 -j DROP

# Persist rules
sudo netfilter-persistent save
```

**Using nftables:**
```bash
sudo nft add rule inet filter input tcp dport 8080 ip saddr 192.168.1.50 accept
sudo nft add rule inet filter input tcp dport 8080 ip saddr 127.0.0.1 accept
sudo nft add rule inet filter input tcp dport 8080 drop
```

**Using ufw:**
```bash
sudo ufw allow from 192.168.1.50 to any port 8080 proto tcp
sudo ufw deny 8080/tcp
```

### 4. Start professord

**As a systemd service (recommended):**
```bash
sudo systemctl enable --now professord
sudo systemctl status professord
```

**Foreground for testing:**
```bash
professord --config /etc/professord.ini
```

Verify from the workstation:
```bash
curl http://192.168.1.100:8080/health
curl -H "Authorization: Bearer a1b2c3d4..." http://192.168.1.100:8080/v1/models
```

### 5. Configure Hermes

On the Jetson Orin Nano, configure Hermes to use professord as its OpenAI-compatible backend. The exact configuration depends on Hermes's setup, but the general pattern is:

```yaml
# Hermes configuration (example)
llm:
  provider: openai-compatible
  base_url: http://192.168.1.100:8080/v1
  api_key: a1b2c3d4e5f6...your-64-char-hex-key...
  model: hermes-70b
```

Or via environment variables:
```bash
export OPENAI_API_BASE=http://192.168.1.100:8080/v1
export OPENAI_API_KEY=a1b2c3d4e5f6...your-64-char-hex-key...
export OPENAI_MODEL=hermes-70b
```

### 6. Verify the Connection

From the Jetson, test connectivity:

```bash
# Health check (no auth required)
curl http://192.168.1.100:8080/health

# Authenticated model list
curl -H "Authorization: Bearer $OPENAI_API_KEY" \
  http://192.168.1.100:8080/v1/models

# Chat completion
curl -X POST http://192.168.1.100:8080/v1/chat/completions \
  -H "Authorization: Bearer $OPENAI_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"hermes-70b","messages":[{"role":"user","content":"Hello"}],"max_tokens":64}'
```

### 7. Monitor

Check professord stats from either machine:
```bash
curl -H "Authorization: Bearer $OPENAI_API_KEY" \
  http://192.168.1.100:8080/v1/stats | python3 -m json.tool
```

Watch the stats log on the workstation:
```bash
# systemd
journalctl -u professord -f | grep stats

# daemonized
journalctl -t professord -f | grep stats

# foreground
# stats lines appear on stderr every --stats-interval seconds
```

### Security Summary

| Layer | Mechanism |
|-------|-----------|
| Network binding | `listen_addr` = LAN IP only (not 0.0.0.0) |
| IP ACL | `allow_ip` drops unlisted IPs at TCP level (no HTTP response) |
| Authentication | Pre-shared Bearer token (`api_key`) |
| OS firewall | iptables/nftables/ufw as defense in depth |
| Transport | Plaintext HTTP (acceptable on trusted LAN) |
| Admission control | 503 when busy (prevents resource exhaustion) |
| Input validation | Body size, message count, role, and parameter limits |
| Inference timeout | `max_inference_seconds` prevents runaway generation |

**Note on TLS**: professord does not support TLS directly. If the LAN is untrusted, place a reverse proxy (nginx, caddy) in front of professord to terminate TLS. The API key travels in plaintext over HTTP.

## API Reference

Base URL: `http://<host>:8080`

Default bind is `127.0.0.1` (loopback). Set `--listen-addr` for LAN access.

### Authentication

If `--api-key` is set, all endpoints except `/health` require:
```
Authorization: Bearer <api-key>
```

### GET /health

Returns `{"status": "ok"}` if the server is running. No auth required.

### GET /v1/stats

Runtime statistics. Returns connection counts, request counts, token throughput, error counts, inference latency, and current state.

```json
{
  "uptime_seconds": 3600,
  "connections": {"total": 150, "active": 2, "rejected": 0},
  "requests": {"chat": 45, "completion": 3, "stream": 12, "health": 90, "models": 5},
  "errors": {"bad_request": 2, "unauthorized": 0, "method_not_allowed": 1, "server_busy": 5, "backend_error": 0},
  "inference": {"total": 60, "tokens_prompt": 12000, "tokens_completion": 8000, "avg_latency_ms": 1500, "avg_tokens_per_second": 42.5, "generating": false}
}
```

### GET /v1/models

Lists the loaded model.

```json
{
  "object": "list",
  "data": [{"id": "local-model", "object": "model", "created": 0, "owned_by": "local"}]
}
```

### POST /v1/chat/completions

OpenAI-compatible chat completion.

**Request:**
```json
{
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Hello"}
  ],
  "temperature": 0.7,
  "max_tokens": 512,
  "stream": false
}
```

**Constraints:** max 64 messages, roles must be `system`/`user`/`assistant`, `max_tokens` capped to available context.

**Response:**
```json
{
  "id": "chatcmpl-abc123",
  "object": "chat.completion",
  "created": 1710000000,
  "model": "local-model",
  "choices": [{
    "index": 0,
    "message": {"role": "assistant", "content": "Hello! How can I help?"},
    "finish_reason": "stop"
  }],
  "usage": {"prompt_tokens": 12, "completion_tokens": 8, "total_tokens": 20}
}
```

With `"stream": true`, returns Server-Sent Events with `chat.completion.chunk` objects, terminated by `data: [DONE]`.

### POST /v1/completions

OpenAI-compatible text completion. Same parameters as chat but uses `prompt` (string) instead of `messages`.

### Errors

All errors return OpenAI-format JSON:

| HTTP | Type | Condition |
|------|------|-----------|
| 400 | `invalid_json` | Malformed JSON body |
| 400 | `invalid_request` | Missing fields, bad values, prompt too long, unknown role |
| 401 | `unauthorized` | Bad/missing API key |
| 405 | `method_not_allowed` | Wrong HTTP method |
| 503 | `server_busy` | Inference already in progress |
| 500 | `backend_error` | llama.cpp failure |

## Architecture

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for component diagrams, data flow, threading model, and full API contract details.

## License

GPL-3.0. See [COPYING](COPYING).
