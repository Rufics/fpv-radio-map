# Web container: builds the Angular client and serves it with nginx, plus /tiles (PMTiles with
# HTTP Range) and a reverse proxy for /api to rfmap-server. Build from the repository root:
#   docker build -f deploy/web.Dockerfile -t rfmap-web .
#
# Base images are pinned by version tag; Dependabot proposes newer ones (docs/admin/deploy.md).

FROM node:24-bookworm-slim AS build
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
# Behind a TLS-intercepting proxy pass its CA: --secret id=build_ca,src=/path/ca.crt
RUN --mount=type=secret,id=build_ca,required=false \
    if [ -s /run/secrets/build_ca ]; then export NODE_EXTRA_CA_CERTS=/run/secrets/build_ca; fi \
    && npm ci --ignore-scripts --no-audit --no-fund
# sync-schemas copies ../schemas into the client, so the browser validates with the same schemas.
COPY schemas/ /src/schemas/
COPY web/ ./
RUN npm run build

FROM nginx:1.28-alpine
COPY deploy/nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /src/web/dist/fpv-app/browser/ /usr/share/nginx/html/
# nginx runs as its own unprivileged user, not root: it listens on 8080 and keeps its pid file in
# the cache directory, the only place it writes.
RUN sed -i -e '/^user /d' -e 's#^pid .*#pid /var/cache/nginx/nginx.pid;#' /etc/nginx/nginx.conf \
    && chown -R nginx:nginx /var/cache/nginx
USER nginx
# Tile archives are mounted here read-only (tools/tiles-builder output).
VOLUME ["/tiles"]
EXPOSE 8080 8081
