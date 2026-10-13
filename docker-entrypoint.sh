#!/bin/sh
# Container entrypoint: refresh + index the knowledge repo, serve Atlas UI
# behind an authenticated nginx front on :9749, and expose MCP over SSE via
# supergateway on :8000.
set -e

KNOWLEDGE_DIR="${CBM_KNOWLEDGE_DIR:-/data/knowledge}"

if [ -n "$CBM_DOCS_REPO" ]; then
    if [ -n "$CBM_DOCS_SSH_KEY_B64" ]; then
        mkdir -p /root/.ssh
        printf '%s' "$CBM_DOCS_SSH_KEY_B64" | base64 -d > /root/.ssh/id_ed25519
        chmod 600 /root/.ssh/id_ed25519
        ssh-keyscan github.com >> /root/.ssh/known_hosts 2>/dev/null || true
        export GIT_SSH_COMMAND="ssh -i /root/.ssh/id_ed25519"
    fi
    if [ -d "$KNOWLEDGE_DIR/.git" ]; then
        git -C "$KNOWLEDGE_DIR" pull --ff-only || true
    else
        git clone --depth 1 "$CBM_DOCS_REPO" "$KNOWLEDGE_DIR" || true
    fi
    codebase-memory-mcp cli index_repository "{\"repo_path\":\"$KNOWLEDGE_DIR\"}" || true
fi

# Atlas UI: the UI is served by the cbm daemon, which is session-managed — it
# retires when its last session disconnects. Holding an initialized stdio
# session open forever keeps the daemon (and the UI on loopback 9750) alive.
# nginx fronts it on 9749 with basic auth (password = CBM_UI_PASSWORD, falling
# back to SUPERGATEWAY_API_KEY).
UI_PASS="${CBM_UI_PASSWORD:-${SUPERGATEWAY_API_KEY:-cbm}}"
printf 'cbm:%s\n' "$(openssl passwd -apr1 "$UI_PASS")" > /etc/nginx/cbm-ui.htpasswd
{ printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"ui-keepalive","version":"1"}}}'; \
  sleep infinity; } | codebase-memory-mcp &
nginx

exec supergateway --stdio codebase-memory-mcp --port 8000 \
    --ssePath /sse --messagePath /message --cors --healthEndpoint /healthz
