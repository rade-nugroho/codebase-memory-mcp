# Dokploy/Docker deployment image — builds codebase-memory-mcp from source
# with the embedded CBM Atlas UI, then serves:
#   - MCP over SSE via supergateway on :8000 (/sse, /message, /healthz)
#   - CBM Atlas UI on :9749 (basic-auth nginx front; cbm itself is loopback
#     on 9750 and validates its Host header)
#
# The UI variant compiles the graph-ui frontend (Vite) and embeds it into the
# binary via `make cbm-with-ui`, so the runtime stage is just the binary plus
# the node-based SSE gateway.
#
# Runtime env:
#   SUPERGATEWAY_API_KEY  bearer key required on the SSE endpoint
#   CBM_DOCS_REPO         git URL of a knowledge repo to clone + index at boot
#   CBM_DOCS_SSH_KEY      private deploy key for CBM_DOCS_REPO (optional)
#   CBM_KNOWLEDGE_DIR     clone target (default /data/knowledge)
#   CBM_UI_PASSWORD       basic-auth password for the Atlas UI on :9749
#                         (user "cbm"; falls back to SUPERGATEWAY_API_KEY)
#   CBM_ALLOWED_ROOT      confine index_repository to this root (set /data for
#                         a multi-tenant server)

FROM node:20-bookworm AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc g++ make zlib1g-dev pkg-config python3 git curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN CBM_NO_CCACHE=1 scripts/build.sh --with-ui

FROM node:20-bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git openssh-client nginx openssl \
 && rm -rf /var/lib/apt/lists/* \
 && npm install -g supergateway@4.1.0
COPY --from=build /src/build/c/codebase-memory-mcp /usr/local/bin/codebase-memory-mcp
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY docker-ui.nginx.conf /etc/nginx/conf.d/cbm-ui.conf
RUN chmod +x /usr/local/bin/docker-entrypoint.sh \
 && rm -f /etc/nginx/sites-enabled/default
ENV CBM_CACHE_DIR=/data
EXPOSE 8000 9749
CMD ["docker-entrypoint.sh"]
