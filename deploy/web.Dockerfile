# Web container: builds the Angular client and serves it with nginx, plus /tiles (PMTiles with
# HTTP Range) and a reverse proxy for /api to rfmap-server. Build from the repository root:
#   docker build -f deploy/web.Dockerfile -t rfmap-web .
#
# Base images are pinned by digest (multi-arch index), read from Docker Hub on 2026-10-17.
# They are written into FROM directly so Dependabot can update them.

FROM node:24-bookworm-slim@sha256:a5f4366e834695d5f363b920c104953a535d3a9864d1bdf74566a0ebe01daee5 AS build
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
# Behind a TLS-intercepting proxy pass its CA: --secret id=build_ca,src=/path/ca.crt
RUN --mount=type=secret,id=build_ca,required=false \
    if [ -s /run/secrets/build_ca ]; then export NODE_EXTRA_CA_CERTS=/run/secrets/build_ca; fi \
    && npm ci --no-audit --no-fund
# sync-schemas copies ../schemas into the client, so the browser validates with the same schemas.
COPY schemas/ /src/schemas/
COPY web/ ./
RUN npm run build

FROM nginx:1.28-alpine@sha256:a8b39bd9cf0f83869a2162827a0caf6137ddf759d50a171451b335cecc87d236
COPY deploy/nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /src/web/dist/fpv-app/browser/ /usr/share/nginx/html/
# Tile archives are mounted here read-only (tools/tiles-builder output).
VOLUME ["/tiles"]
EXPOSE 80
