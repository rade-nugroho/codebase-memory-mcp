#!/bin/sh
# Container entrypoint: refresh + index the knowledge repo, serve Atlas UI
# behind an authenticated nginx front on :9749, and expose MCP over SSE via
# supergateway on :8000.
set -e

KNOWLEDGE_DIR="${CBM_KNOWLEDGE_DIR:-/data/knowledge}"

if [ -n "$CBM_DOCS_REPO" ]; then
    if [ -n "$CBM_DOCS_SSH_KEY" ]; then
        mkdir -p /root/.ssh
        printf '%s\n' "$CBM_DOCS_SSH_KEY" > /root/.ssh/id_ed25519
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

# Atlas UI: cbm serves loopback on 9750; nginx fronts it on 9749 with basic
# auth (password = CBM_UI_PASSWORD, falling back to SUPERGATEWAY_API_KEY).
UI_PASS="${CBM_UI_PASSWORD:-${SUPERGATEWAY_API_KEY:-cbm}}"
printf 'cbm:%s\n' "$(openssl passwd -apr1 "$UI_PASS")" > /etc/nginx/cbm-ui.htpasswd
sleep infinity | codebase-memory-mcp --ui=true --port=9750 &
nginx

exec supergateway --stdio codebase-memory-mcp --port 8000 \
    --ssePath /sse --messagePath /message --cors --healthEndpoint /healthz
