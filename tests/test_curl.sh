#!/bin/bash
# test_curl.sh -- Integration tests for professord HTTP API
#
# Part of Professor_AI (professord)
# SPDX-License-Identifier: GPL-3.0-only
#
# Usage: bash tests/test_curl.sh [base_url] [api_key]

BASE="${1:-http://localhost:8080}"
API_KEY="${2:-}"
PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

# Auth header args (empty array if no key)
AUTH=()
if [ -n "$API_KEY" ]; then
    AUTH=(-H "Authorization: Bearer $API_KEY")
fi

# --- Non-inference tests ---

echo "=== Non-inference tests ==="

# Health (no auth required)
curl -s "$BASE/health" | grep -q '"status"' && pass "health" || fail "health"
curl -s "$BASE/v1/health" | grep -q '"status"' && pass "v1_health" || fail "v1_health"

# Models
curl -s "${AUTH[@]}" "$BASE/v1/models" | grep -q '"data"' && pass "models" || fail "models"

# Method not allowed
CODE=$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$BASE/v1/chat/completions")
[ "$CODE" = "405" ] && pass "method_405" || fail "method_405 (got $CODE)"

# Invalid JSON
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' -d 'not valid json')
[ "$CODE" = "400" ] && pass "bad_json" || fail "bad_json (got $CODE)"

# Bad role
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"villain","content":"hi"}]}')
[ "$CODE" = "400" ] && pass "bad_role" || fail "bad_role (got $CODE)"

# Unknown endpoint
CODE=$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$BASE/v1/nonexistent")
[ "$CODE" = "404" ] && pass "not_found" || fail "not_found (got $CODE)"

# --- Inference tests (require loaded model) ---

echo ""
echo "=== Inference tests ==="

# Chat completion
RESP=$(curl -s --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":8}')
echo "$RESP" | grep -q '"chat.completion"' && pass "chat" || fail "chat"

# Text completion
RESP=$(curl -s --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/completions" \
    -H 'Content-Type: application/json' \
    -d '{"prompt":"The capital of France is","max_tokens":8}')
echo "$RESP" | grep -q '"text_completion"' && pass "completion" || fail "completion"

# Streaming
RESP=$(curl -sN --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":8,"stream":true}')
echo "$RESP" | grep -q 'data: \[DONE\]' && pass "stream" || fail "stream"

# Admission control (503 when busy)
curl -s --max-time 120 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Count slowly"}],"max_tokens":64}' > /dev/null &
BG_PID=$!
sleep 0.3
CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "${AUTH[@]}" "$BASE/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":4}')
[ "$CODE" = "503" ] && pass "busy_503" || fail "busy_503 (got $CODE)"
wait "$BG_PID" 2>/dev/null

# --- Summary ---

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
