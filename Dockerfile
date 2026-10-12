# Dokploy/Docker deployment image — builds codebase-memory-mcp from source
# with the embedded CBM Atlas UI and serves it on port 9749.
#
# The UI variant compiles the graph-ui frontend (Vite) and embeds it into the
# binary via `make cbm-with-ui`, so the runtime stage is just the binary.

FROM node:20-bookworm AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc g++ make zlib1g-dev pkg-config python3 git curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN CBM_NO_CCACHE=1 scripts/build.sh --with-ui

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/c/codebase-memory-mcp /usr/local/bin/codebase-memory-mcp
ENV CBM_CACHE_DIR=/data
EXPOSE 9749
# The UI is served by the MCP process; keep stdin open so it stays up.
CMD ["sh", "-c", "sleep infinity | exec codebase-memory-mcp --ui=true --port=9749"]
