# Dokploy/Docker deployment image — builds codebase-memory-mcp from source
# with the embedded CBM Atlas UI, then serves:
#   - MCP over SSE via supergateway on :8000 (/sse, /message, /healthz)
#   - CBM Atlas UI on :9749
#
# The UI variant compiles the graph-ui frontend (Vite) and embeds it into the
# binary via `make cbm-with-ui`, so the runtime stage is just the binary plus
# the node-based SSE gateway.
#
# Set SUPERGATEWAY_API_KEY to require a bearer key on the SSE endpoint.

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
 && apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && npm install -g supergateway@4.1.0
COPY --from=build /src/build/c/codebase-memory-mcp /usr/local/bin/codebase-memory-mcp
ENV CBM_CACHE_DIR=/data
EXPOSE 8000 9749
# The Atlas UI is served by an MCP process whose stdin is held open; the SSE
# gateway spawns one stdio server per client connection (shared CBM daemon).
CMD ["sh", "-c", "sleep infinity | codebase-memory-mcp --ui=true --port=9749 & exec supergateway --stdio codebase-memory-mcp --port 8000 --ssePath /sse --messagePath /message --cors --healthEndpoint /healthz"]
