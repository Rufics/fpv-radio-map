# rfmap-server image. Build from the repository root:
#   docker build -f deploy/server.Dockerfile --build-arg RFMAP_OWNER_PUBKEY="$(cat owner.pub)" -t rfmap-server .
# Without RFMAP_OWNER_PUBKEY the public development key is built in and the server warns at start.
#
# Base images are pinned by version tag; Dependabot proposes newer ones. Keep both stages on the
# same tag (docs/admin/deploy.md, "Updating base images").

# ---- build ----
FROM ubuntu:26.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake git \
        libarchive-dev libcpp-httplib-dev libgdal-dev libgeographiclib-dev \
        libpqxx-dev libsodium-dev ninja-build nlohmann-json3-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY core/ core/
COPY platform/ platform/
COPY modules/ modules/
COPY examples/ examples/
COPY server/ server/
COPY schemas/ schemas/
COPY data/library/ data/library/

ARG RFMAP_OWNER_PUBKEY=""
ARG RFMAP_VERSION=0.1.0
# Tests are built and run in CI, not in the image. CMake fetches json-schema-validator 2.4.1 from
# GitHub; behind a TLS-intercepting proxy pass its CA: --secret id=build_ca,src=/path/ca.crt
RUN --mount=type=secret,id=build_ca,required=false \
    if [ -s /run/secrets/build_ca ]; then export GIT_SSL_CAINFO=/run/secrets/build_ca; fi \
    && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DRFMAP_BUILD_TESTS=OFF -DRFMAP_BUILD_SERVER=ON \
        -DRFMAP_OWNER_PUBKEY="${RFMAP_OWNER_PUBKEY}" -DRFMAP_VERSION="${RFMAP_VERSION}" \
    && cmake --build build --target rfmap-server rfmod-worker rfmod \
    && DESTDIR=/out cmake --install build/server \
    && install -D -m 0755 build/platform/rfmod/rfmod /out/usr/bin/rfmod

# ---- runtime ----
FROM ubuntu:26.04
ARG DEBIAN_FRONTEND=noninteractive
# libpqxx is linked statically (server/CMakeLists.txt), so only libpq is needed at run time.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libarchive13t64 libcpp-httplib0.26 libgdal38 \
        libgeographiclib26 libpq5 libsodium23 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --home-dir /var/lib/rfmap --shell /usr/sbin/nologin rfmap \
    && install -d -o rfmap -g rfmap -m 0750 /var/lib/rfmap /var/lib/rfmap/modules \
    && install -d -m 0755 /etc/rfmap /data

COPY --from=build /out/ /
COPY server/config/server.example.json /etc/rfmap/rfmap.json

USER rfmap
WORKDIR /var/lib/rfmap
ENV RFMAP_CONFIG=/etc/rfmap/rfmap.json \
    RFMAP_SQL_DIR=/usr/share/rfmap/sql
# 8080 is the API; 9090 the operations listener (/healthz, /readyz, /metrics), which binds
# 127.0.0.1 unless ops.listen says otherwise (RFMAP__OPS__LISTEN=0.0.0.0 for Kubernetes probes).
EXPOSE 8080 9090
VOLUME ["/var/lib/rfmap"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s CMD ["/usr/bin/rfmap-server", "--health", "http://127.0.0.1:9090/readyz"]
ENTRYPOINT ["/usr/bin/rfmap-server"]
