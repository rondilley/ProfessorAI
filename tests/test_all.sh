#!/bin/bash
# test_all.sh -- Run all test suites and generate a timestamped report
#
# Part of Professor_AI (professord)
# SPDX-License-Identifier: GPL-3.0-only
#
# Usage: bash tests/test_all.sh [base_url] [api_key]
#
# Generates: tests/reports/report-YYYYMMDD-HHMMSS.md

BASE="${1:-http://localhost:8080}"
API_KEY="${2:-}"
PASS=0
FAIL=0
SKIP=0
TOTAL_START=$(date +%s%N)

AUTH=()
if [ -n "$API_KEY" ]; then
    AUTH=(-H "Authorization: Bearer $API_KEY")
fi

# Report setup
TIMESTAMP=$(date -u +%Y%m%d-%H%M%S)
REPORT_DIR="$(dirname "$0")/reports"
mkdir -p "$REPORT_DIR"
REPORT="$REPORT_DIR/report-${TIMESTAMP}.md"

# Collect system info
HOSTNAME=$(hostname)
KERNEL=$(uname -r)
CPU=$(grep 'model name' /proc/cpuinfo | head -1 | sed 's/model name.*: //')
RAM_TOTAL=$(free -h | awk '/Mem:/ {print $2}')
GPU=$(cat /sys/class/drm/card*/device/product_name 2>/dev/null | head -1)
if [ -z "$GPU" ]; then
    GPU=$(grep 'model name' /proc/cpuinfo | head -1 | sed 's/model name.*: //')
fi

# Collect server info
SERVER_STATS=""
if [ -n "$API_KEY" ]; then
    SERVER_STATS=$(curl -s --max-time 5 -H "Authorization: Bearer $API_KEY" "$BASE/v1/stats" 2>/dev/null)
else
    SERVER_STATS=$(curl -s --max-time 5 "$BASE/v1/stats" 2>/dev/null)
fi

# --- Helpers ---

pass() {
    echo "  PASS: $1 (${2}ms)"
    PASS=$((PASS + 1))
}

fail() {
    echo "  FAIL: $1 (${2}ms)"
    FAIL=$((FAIL + 1))
}

skip() {
    echo "  SKIP: $1"
    SKIP=$((SKIP + 1))
}

now_ms() {
    echo $(( $(date +%s%N) / 1000000 ))
}

# Run a test, measure time, record result
# Usage: run_test "name" expected_code curl_args...
# For grep-based tests, use run_test_grep
RESULTS=""

run_test() {
    local name="$1"
    local expected="$2"
    shift 2
    local start=$(now_ms)
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "$@")
    local elapsed=$(( $(now_ms) - start ))

    if [ "$expected" = "any" ]; then
        if [ "$code" -ge 200 ] 2>/dev/null && [ "$code" -lt 600 ] 2>/dev/null; then
            pass "$name" "$elapsed"
            RESULTS="${RESULTS}| $name | PASS | $code | ${elapsed}ms |\n"
        else
            fail "$name (got $code)" "$elapsed"
            RESULTS="${RESULTS}| $name | FAIL | $code | ${elapsed}ms |\n"
        fi
    else
        if [ "$code" = "$expected" ]; then
            pass "$name" "$elapsed"
            RESULTS="${RESULTS}| $name | PASS | $code | ${elapsed}ms |\n"
        else
            fail "$name (got $code, expected $expected)" "$elapsed"
            RESULTS="${RESULTS}| $name | FAIL | $code (expected $expected) | ${elapsed}ms |\n"
        fi
    fi
}

run_test_grep() {
    local name="$1"
    local pattern="$2"
    shift 2
    local start=$(now_ms)
    local body
    body=$(curl -s "$@")
    local elapsed=$(( $(now_ms) - start ))

    if echo "$body" | grep -q "$pattern"; then
        pass "$name" "$elapsed"
        RESULTS="${RESULTS}| $name | PASS | 200 | ${elapsed}ms |\n"
    else
        fail "$name" "$elapsed"
        RESULTS="${RESULTS}| $name | FAIL | - | ${elapsed}ms |\n"
    fi
}

# Inference test: returns response body, measures time
run_inference() {
    local name="$1"
    shift
    local start=$(now_ms)
    local body
    body=$(curl -s --max-time 120 "$@")
    local elapsed=$(( $(now_ms) - start ))

    local prompt_tok=$(echo "$body" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('usage',{}).get('prompt_tokens',0))" 2>/dev/null || echo 0)
    local comp_tok=$(echo "$body" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('usage',{}).get('completion_tokens',0))" 2>/dev/null || echo 0)
    local tps="0.0"
    if [ "$elapsed" -gt 0 ] && [ "$comp_tok" -gt 0 ]; then
        tps=$(python3 -c "print(f'{$comp_tok / ($elapsed / 1000.0):.1f}')")
    fi

    if echo "$body" | grep -q '"choices"'; then
        pass "$name" "$elapsed"
        RESULTS="${RESULTS}| $name | PASS | ${elapsed}ms | prompt=${prompt_tok} comp=${comp_tok} | ${tps} tok/s |\n"
    else
        fail "$name" "$elapsed"
        RESULTS="${RESULTS}| $name | FAIL | ${elapsed}ms | - | - |\n"
    fi
}

# --- Check server ---

if ! curl -s --max-time 5 "$BASE/health" | grep -q '"status"'; then
    echo "ERROR: professord not responding at $BASE"
    exit 1
fi

echo "============================================"
echo "  Professor_AI Test Suite"
echo "  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "  Target: $BASE"
echo "============================================"
echo ""

# ============================================================
# UNIT TESTS
# ============================================================

echo "=== Unit Tests ==="
UNIT_START=$(now_ms)
UNIT_OUTPUT=$(ctest --test-dir "$(dirname "$0")/../build-release" --output-on-failure 2>&1)
UNIT_ELAPSED=$(( $(now_ms) - UNIT_START ))
UNIT_PASSED=$(echo "$UNIT_OUTPUT" | grep -c 'Passed')
UNIT_FAILED=$(echo "$UNIT_OUTPUT" | grep -c 'Failed\|FAILED')
UNIT_TOTAL=$((UNIT_PASSED + UNIT_FAILED))
echo "  $UNIT_PASSED/$UNIT_TOTAL passed (${UNIT_ELAPSED}ms)"
if [ "$UNIT_FAILED" -gt 0 ]; then
    echo "$UNIT_OUTPUT" | grep -A2 'FAILED'
fi
echo ""

# ============================================================
# FUNCTIONAL TESTS
# ============================================================

echo "=== Functional Tests ==="
FUNC_RESULTS=""
RESULTS=""
FUNC_START=$(now_ms)

run_test_grep "health" '"status"' "$BASE/health"
run_test_grep "v1_health" '"status"' "$BASE/v1/health"
run_test_grep "models" '"data"' "${AUTH[@]}" "$BASE/v1/models"
run_test "method_405" "405" "${AUTH[@]}" "$BASE/v1/chat/completions"
run_test "bad_json" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' -d 'not json'
run_test "bad_role" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' -d '{"messages":[{"role":"villain","content":"hi"}]}'
run_test "not_found" "404" "${AUTH[@]}" "$BASE/v1/nonexistent"

FUNC_RESULTS="$RESULTS"
RESULTS=""

echo ""
echo "=== Inference Tests ==="
INF_RESULTS=""

run_inference "chat_completion" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":8}'

run_inference "text_completion" -X POST "${AUTH[@]}" "$BASE/v1/completions" \
    -H 'Content-Type: application/json' \
    -d '{"prompt":"The capital of France is","max_tokens":8}'

# Streaming
STREAM_START=$(now_ms)
STREAM_BODY=$(curl -sN --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":8,"stream":true}')
STREAM_ELAPSED=$(( $(now_ms) - STREAM_START ))
STREAM_CHUNKS=$(echo "$STREAM_BODY" | grep -c '^data: {')
if echo "$STREAM_BODY" | grep -q 'data: \[DONE\]'; then
    pass "streaming" "$STREAM_ELAPSED"
    RESULTS="${RESULTS}| streaming | PASS | ${STREAM_ELAPSED}ms | ${STREAM_CHUNKS} chunks | - |\n"
else
    fail "streaming" "$STREAM_ELAPSED"
    RESULTS="${RESULTS}| streaming | FAIL | ${STREAM_ELAPSED}ms | - | - |\n"
fi

# Admission control
ADM_START=$(now_ms)
curl -s --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Count slowly"}],"max_tokens":64}' > /dev/null &
BG_PID=$!
sleep 0.3
ADM_CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":4}')
ADM_ELAPSED=$(( $(now_ms) - ADM_START ))
if [ "$ADM_CODE" = "503" ]; then
    pass "busy_503" "$ADM_ELAPSED"
    RESULTS="${RESULTS}| busy_503 | PASS | ${ADM_ELAPSED}ms | - | - |\n"
else
    fail "busy_503 (got $ADM_CODE)" "$ADM_ELAPSED"
    RESULTS="${RESULTS}| busy_503 | FAIL | ${ADM_ELAPSED}ms | - | - |\n"
fi
wait "$BG_PID" 2>/dev/null

INF_RESULTS="$RESULTS"
RESULTS=""
FUNC_ELAPSED=$(( $(now_ms) - FUNC_START ))

echo ""

# ============================================================
# STRESS TESTS
# ============================================================

echo "=== Stress Tests ==="
STRESS_RESULTS=""
STRESS_START=$(now_ms)

# Malformed input battery
echo "  --- Malformed Input ---"
run_test "empty_body" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d ''
run_test "whitespace_body" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '   '
run_test "truncated_json" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","con'
run_test "array_body" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '[1,2,3]'
run_test "messages_number" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":42}'
run_test "empty_messages" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[]}'
run_test "missing_role" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"content":"hello"}]}'
run_test "role_number" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":123,"content":"hello"}]}'
run_test "missing_prompt" "400" -X POST "${AUTH[@]}" "$BASE/v1/completions" -H 'Content-Type: application/json' -d '{"max_tokens":4}'

# Numeric edge cases
echo "  --- Numeric Edge Cases ---"
run_test "neg_max_tokens" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"hi"}],"max_tokens":-999}'
run_test "huge_max_tokens" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"hi"}],"max_tokens":999999999}'
run_test "zero_max_tokens" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"hi"}],"max_tokens":0}'
run_test "temp_overflow" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"hi"}],"temperature":1e308,"max_tokens":4}'
run_test "neg_temperature" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"hi"}],"temperature":-5.0,"max_tokens":4}'
run_test "top_p_100" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"hi"}],"top_p":100.0,"max_tokens":4}'

# HTTP method abuse
echo "  --- HTTP Method Abuse ---"
run_test "delete_models" "405" -X DELETE "${AUTH[@]}" "$BASE/v1/models"
run_test "put_chat" "405" -X PUT "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{}'
run_test "patch_chat" "405" -X PATCH "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d '{}'

# Large payloads
echo "  --- Large Payloads ---"
BIG_CONTENT=$(python3 -c "print('A' * 16000)")
run_test "16k_content" "any" --max-time 30 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"messages\":[{\"role\":\"user\",\"content\":\"$BIG_CONTENT\"}],\"max_tokens\":4}"

OVER_MSGS=$(python3 -c "
msgs = ','.join(['{\"role\":\"user\",\"content\":\"msg %d\"}' % i for i in range(65)])
print('{\"messages\":[' + msgs + '],\"max_tokens\":4}')
")
run_test "65_messages" "400" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d "$OVER_MSGS"

BIGFILE=$(mktemp)
python3 -c "print('{\"messages\":[{\"role\":\"user\",\"content\":\"' + 'X' * 2000000 + '\"}]}')" > "$BIGFILE"
run_test "2mb_body" "400" --max-time 10 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" -H 'Content-Type: application/json' -d @"$BIGFILE"
rm -f "$BIGFILE"

STRESS_RESULTS="$RESULTS"
RESULTS=""

# Rapid-fire
echo "  --- Rapid-Fire ---"
RAPID_START=$(now_ms)
HEALTH_FAIL=0
for i in $(seq 1 50); do
    if ! curl -s --max-time 2 "$BASE/health" | grep -q '"status"'; then
        HEALTH_FAIL=$((HEALTH_FAIL + 1))
    fi
done
RAPID_HEALTH_MS=$(( $(now_ms) - RAPID_START ))
RAPID_HEALTH_AVG=$((RAPID_HEALTH_MS / 50))
if [ "$HEALTH_FAIL" -eq 0 ]; then
    pass "50_rapid_health" "$RAPID_HEALTH_MS"
    RESULTS="${RESULTS}| 50_rapid_health | PASS | ${RAPID_HEALTH_MS}ms total | avg=${RAPID_HEALTH_AVG}ms | 0 failed |\n"
else
    fail "50_rapid_health ($HEALTH_FAIL failed)" "$RAPID_HEALTH_MS"
    RESULTS="${RESULTS}| 50_rapid_health | FAIL | ${RAPID_HEALTH_MS}ms total | avg=${RAPID_HEALTH_AVG}ms | $HEALTH_FAIL failed |\n"
fi

RAPID_MODEL_START=$(now_ms)
MODEL_FAIL=0
for i in $(seq 1 20); do
    if ! curl -s --max-time 2 "${AUTH[@]}" "$BASE/v1/models" | grep -q '"data"'; then
        MODEL_FAIL=$((MODEL_FAIL + 1))
    fi
done
RAPID_MODEL_MS=$(( $(now_ms) - RAPID_MODEL_START ))
RAPID_MODEL_AVG=$((RAPID_MODEL_MS / 20))
if [ "$MODEL_FAIL" -eq 0 ]; then
    pass "20_rapid_models" "$RAPID_MODEL_MS"
    RESULTS="${RESULTS}| 20_rapid_models | PASS | ${RAPID_MODEL_MS}ms total | avg=${RAPID_MODEL_AVG}ms | 0 failed |\n"
else
    fail "20_rapid_models ($MODEL_FAIL failed)" "$RAPID_MODEL_MS"
    RESULTS="${RESULTS}| 20_rapid_models | FAIL | ${RAPID_MODEL_MS}ms total | avg=${RAPID_MODEL_AVG}ms | $MODEL_FAIL failed |\n"
fi

# Concurrent health during inference
echo "  --- Concurrency ---"
curl -s --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Write a story"}],"max_tokens":64}' > /dev/null &
INF_PID=$!
sleep 0.5

HEALTH_OK=0
CONC_START=$(now_ms)
for i in $(seq 1 5); do
    if curl -s --max-time 2 "$BASE/health" | grep -q '"status"'; then
        HEALTH_OK=$((HEALTH_OK + 1))
    fi
    sleep 0.2
done
CONC_MS=$(( $(now_ms) - CONC_START ))
if [ "$HEALTH_OK" -ge 3 ]; then
    pass "health_during_inference ($HEALTH_OK/5)" "$CONC_MS"
    RESULTS="${RESULTS}| health_during_inference | PASS | ${CONC_MS}ms | $HEALTH_OK/5 responsive | - |\n"
else
    fail "health_during_inference ($HEALTH_OK/5)" "$CONC_MS"
    RESULTS="${RESULTS}| health_during_inference | FAIL | ${CONC_MS}ms | $HEALTH_OK/5 responsive | - |\n"
fi
wait "$INF_PID" 2>/dev/null

# Connection abuse
echo "  --- Connection Abuse ---"
ABUSE_START=$(now_ms)
for i in $(seq 1 20); do
    curl -s --max-time 1 --connect-timeout 1 "$BASE/health" > /dev/null 2>&1 &
done
wait
ABUSE_MS=$(( $(now_ms) - ABUSE_START ))
if curl -s --max-time 5 "$BASE/health" | grep -q '"status"'; then
    pass "20_rapid_connect_disconnect" "$ABUSE_MS"
    RESULTS="${RESULTS}| 20_rapid_connect_disconnect | PASS | ${ABUSE_MS}ms | - | - |\n"
else
    fail "20_rapid_connect_disconnect" "$ABUSE_MS"
    RESULTS="${RESULTS}| 20_rapid_connect_disconnect | FAIL | ${ABUSE_MS}ms | - | - |\n"
fi

# Auth edge cases
echo "  --- Auth Edge Cases ---"
if [ -n "$API_KEY" ]; then
    run_test "no_auth" "401" "$BASE/v1/models"
    run_test "wrong_key" "401" -H "Authorization: Bearer wrongkey" "$BASE/v1/models"
    run_test "basic_auth" "401" -H "Authorization: Basic dXNlcjpwYXNz" "$BASE/v1/models"
    run_test "empty_bearer" "401" -H "Authorization: Bearer " "$BASE/v1/models"
    run_test_grep "health_no_auth" '"status"' "$BASE/health"
else
    skip "auth tests (no API key)"
fi

LOAD_RESULTS="$RESULTS"
RESULTS=""

STRESS_ELAPSED=$(( $(now_ms) - STRESS_START ))

# Final stability
echo ""
echo "=== Final Stability ==="
FINAL_START=$(now_ms)
run_test_grep "server_alive" '"status"' "$BASE/health"

run_inference "final_inference" -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Say OK"}],"max_tokens":4}'
FINAL_RESULTS="$RESULTS"
FINAL_ELAPSED=$(( $(now_ms) - FINAL_START ))

TOTAL_ELAPSED=$(( ($(date +%s%N) - TOTAL_START) / 1000000 ))

# Collect post-test stats
POST_STATS=""
if [ -n "$API_KEY" ]; then
    POST_STATS=$(curl -s --max-time 5 -H "Authorization: Bearer $API_KEY" "$BASE/v1/stats" 2>/dev/null)
else
    POST_STATS=$(curl -s --max-time 5 "$BASE/v1/stats" 2>/dev/null)
fi

echo ""
echo "============================================"
echo "  TOTALS: $PASS passed, $FAIL failed, $SKIP skipped"
echo "  Duration: ${TOTAL_ELAPSED}ms"
echo "============================================"

# ============================================================
# GENERATE REPORT
# ============================================================

cat > "$REPORT" << REPORT_EOF
# Professor_AI Test Report

- **Date**: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- **Host**: $HOSTNAME
- **Kernel**: $KERNEL
- **CPU**: $CPU
- **RAM**: $RAM_TOTAL
- **GPU**: $GPU
- **Target**: $BASE
- **Auth**: $([ -n "$API_KEY" ] && echo "enabled" || echo "disabled")

## Summary

| Metric | Value |
|--------|-------|
| Total tests | $((PASS + FAIL + SKIP)) |
| Passed | $PASS |
| Failed | $FAIL |
| Skipped | $SKIP |
| Total duration | ${TOTAL_ELAPSED}ms |

## Unit Tests

| Metric | Value |
|--------|-------|
| Passed | $UNIT_PASSED |
| Failed | $UNIT_FAILED |
| Duration | ${UNIT_ELAPSED}ms |

## Functional Tests

### Endpoint Tests

| Test | Result | Status | Latency |
|------|--------|--------|---------|
$(echo -e "$FUNC_RESULTS")

### Inference Tests

| Test | Result | Latency | Tokens | Throughput |
|------|--------|---------|--------|------------|
$(echo -e "$INF_RESULTS")

## Stress & Resilience Tests

### Input Validation

| Test | Result | Status | Latency |
|------|--------|--------|---------|
$(echo -e "$STRESS_RESULTS")

### Load & Concurrency

| Test | Result | Duration | Detail | Notes |
|------|--------|----------|--------|-------|
$(echo -e "$LOAD_RESULTS")

### Rapid-Fire Metrics

| Metric | Value |
|--------|-------|
| 50 health checks total | ${RAPID_HEALTH_MS}ms |
| Health avg latency | ${RAPID_HEALTH_AVG}ms |
| Health failures | $HEALTH_FAIL |
| 20 model lists total | ${RAPID_MODEL_MS}ms |
| Model list avg latency | ${RAPID_MODEL_AVG}ms |
| Model list failures | $MODEL_FAIL |

## Final Stability

| Test | Result | Latency | Tokens | Throughput |
|------|--------|---------|--------|------------|
$(echo -e "$FINAL_RESULTS")

## Server Stats (post-test)

\`\`\`json
$(echo "$POST_STATS" | python3 -m json.tool 2>/dev/null || echo "$POST_STATS")
\`\`\`

REPORT_EOF

echo ""
echo "Report written to: $REPORT"
[ "$FAIL" -eq 0 ]
