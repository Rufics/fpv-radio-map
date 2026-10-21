# rfmap-server built with AddressSanitizer (with LeakSanitizer) and UndefinedBehaviorSanitizer, for
# `perf.py run --sanitize`. Same sources and libraries as deploy/server.Dockerfile, but with debug
# information and frame pointers so the reports name the lines. Build from the repository root:
#   docker build -f tools/perf/stand/server-sanitize.Dockerfile -t rfmap-server:perf-sanitize .
# It is 2-3 times slower and uses more memory: its numbers are only compared with other sanitized
# runs (the sanitizer collector takes part in the baseline).

FROM ubuntu:24.04 AS build
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

ARG RFMAP_VERSION=0.1.0
RUN --mount=type=secret,id=build_ca,required=false \
    if [ -s /run/secrets/build_ca ]; then export GIT_SSL_CAINFO=/run/secrets/build_ca; fi \
    && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
        -DRFMAP_BUILD_TESTS=OFF -DRFMAP_BUILD_SERVER=ON -DRFMAP_VERSION="${RFMAP_VERSION}" \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
    && cmake --build build --target rfmap-server rfmod-worker rfmod \
    && DESTDIR=/out cmake --install build/server \
    && install -D -m 0755 build/platform/rfmod/rfmod /out/usr/bin/rfmod

FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
# libasan8 and libubsan1 are the runtimes of GCC 13; binutils brings addr2line, which symbolizes.
RUN apt-get update && apt-get install -y --no-install-recommends \
        binutils ca-certificates libarchive13t64 libasan8 libcpp-httplib0.14t64 libgdal34t64 \
        libgeographiclib26 libpqxx-7.8t64 libsodium23 libubsan1 \
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
EXPOSE 8080
ENTRYPOINT ["/usr/bin/rfmap-server"]
