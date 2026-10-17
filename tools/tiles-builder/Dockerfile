# syntax=docker/dockerfile:1.7
#
# rfmap/tiles-builder — builds the self-hosted map data for FPV Radio Map.
#
#   docker build -t rfmap/tiles-builder tools/tiles-builder
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/data:/data" rfmap/tiles-builder all --bbox 35.15,48.50,35.25,48.56 --region europe/monaco
#
# Every tool version is pinned:
#   * base image by version tag (Dependabot proposes newer ones),
#   * Ubuntu packages by exact version, resolved from a dated Ubuntu archive
#     snapshot (snapshot.ubuntu.com), so the same versions stay installable,
#   * Planetiler, go-pmtiles and the glyph fonts by release/commit + sha256.
# To upgrade: change the ARG values below, rebuild, and re-run the smoke test
# (tests/smoke.sh). See README.md "Upgrading pinned tools".
#
# Behind a TLS-intercepting proxy, pass the proxy CA as an optional build secret:
#   docker build --secret id=extra_ca,src=/path/ca.crt ...
# Without the secret nothing changes.

FROM ubuntu:24.04

ARG UBUNTU_SNAPSHOT=20261015T000000Z
ARG CA_CERTIFICATES_VERSION=20260601~24.04.1
ARG GDAL_VERSION=3.8.4+dfsg-3ubuntu3
ARG OPENJDK_VERSION=21.0.12.1+1-1~24.04.4
ARG CURL_VERSION=8.5.0-2ubuntu10.16
ARG GIT_VERSION=1:2.43.0-1ubuntu7.3
ARG PYTHON3_VERSION=3.12.3-0ubuntu2.1
ARG SQLITE3_VERSION=3.45.1-1ubuntu2.8

ARG PLANETILER_VERSION=0.10.2
ARG PLANETILER_SHA256=f310bd0413e2e4512b27f4046d418664e8e1d3bf31603c2a70e23de06c167e4d
ARG PMTILES_VERSION=1.31.0
ARG PMTILES_SHA256_AMD64=e80bf5716f45b5330bc1e3037e12f254da89fce23c58b611897fe4a413aee635
ARG PMTILES_SHA256_ARM64=33fdb8d1b00c46a11877d1b86220581b7d92da16366ebcd79cdd550241c0b86f
# protomaps/basemaps-assets: pre-built MapLibre glyph PBFs (Noto Sans, SIL OFL 1.1)
ARG FONTS_COMMIT=028c18f713baecad011301ff7a69acc39bcc2ae7

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

# 1) CA certificates (needed to reach the https snapshot archive), then
# 2) everything else at exact versions from the dated snapshot.
RUN --mount=type=secret,id=extra_ca,required=false \
    set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends "ca-certificates=${CA_CERTIFICATES_VERSION}"; \
    if [ -s /run/secrets/extra_ca ]; then \
      cp /run/secrets/extra_ca /usr/local/share/ca-certificates/build-extra-ca.crt; update-ca-certificates; \
    fi; \
    apt-get update --snapshot "${UBUNTU_SNAPSHOT}"; \
    apt-get install -y --no-install-recommends --snapshot "${UBUNTU_SNAPSHOT}" \
      "gdal-bin=${GDAL_VERSION}" \
      "python3-gdal=${GDAL_VERSION}" \
      "openjdk-21-jre-headless=${OPENJDK_VERSION}" \
      "curl=${CURL_VERSION}" \
      "git=${GIT_VERSION}" \
      "python3=${PYTHON3_VERSION}" \
      "sqlite3=${SQLITE3_VERSION}"; \
    rm -f /usr/local/share/ca-certificates/build-extra-ca.crt; update-ca-certificates --fresh >/dev/null; \
    rm -rf /var/lib/apt/lists/*

# Planetiler (OpenMapTiles profile is built in) and go-pmtiles, verified by sha256.
RUN --mount=type=secret,id=extra_ca,required=false \
    set -eux; \
    if [ -s /run/secrets/extra_ca ]; then export CURL_CA_BUNDLE=/run/secrets/extra_ca GIT_SSL_CAINFO=/run/secrets/extra_ca; fi; \
    mkdir -p /opt/planetiler /opt/fonts; \
    curl -fsSL --proto =https --proto-redir =https -o /opt/planetiler/planetiler.jar \
      "https://github.com/onthegomap/planetiler/releases/download/v${PLANETILER_VERSION}/planetiler.jar"; \
    echo "${PLANETILER_SHA256}  /opt/planetiler/planetiler.jar" | sha256sum -c -; \
    arch="$(dpkg --print-architecture)"; \
    case "$arch" in \
      amd64) pm_arch=x86_64; pm_sha="${PMTILES_SHA256_AMD64}";; \
      arm64) pm_arch=arm64;  pm_sha="${PMTILES_SHA256_ARM64}";; \
      *) echo "unsupported arch $arch"; exit 1;; \
    esac; \
    curl -fsSL --proto =https --proto-redir =https -o /tmp/pmtiles.tgz \
      "https://github.com/protomaps/go-pmtiles/releases/download/v${PMTILES_VERSION}/go-pmtiles_${PMTILES_VERSION}_Linux_${pm_arch}.tar.gz"; \
    echo "${pm_sha}  /tmp/pmtiles.tgz" | sha256sum -c -; \
    tar -xzf /tmp/pmtiles.tgz -C /usr/local/bin pmtiles; rm /tmp/pmtiles.tgz; \
    pmtiles version; \
    git init -q /tmp/fonts; cd /tmp/fonts; \
    git remote add origin https://github.com/protomaps/basemaps-assets.git; \
    git config core.sparseCheckout true; \
    printf 'fonts/Noto Sans Regular/\nfonts/Noto Sans Medium/\nfonts/Noto Sans Italic/\nfonts/OFL.txt\n' > .git/info/sparse-checkout; \
    git fetch -q --depth 1 --filter=blob:none origin "${FONTS_COMMIT}"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "${FONTS_COMMIT}"; \
    cp -r fonts/* /opt/fonts/; cd /; rm -rf /tmp/fonts; \
    ls /opt/fonts

ENV RFMAP_PLANETILER_JAR=/opt/planetiler/planetiler.jar \
    RFMAP_FONTS_DIR=/opt/fonts \
    RFMAP_PLANETILER_VERSION=${PLANETILER_VERSION} \
    RFMAP_PMTILES_VERSION=${PMTILES_VERSION} \
    RFMAP_FONTS_COMMIT=${FONTS_COMMIT} \
    RFMAP_DATA=/data \
    PYTHONPATH=/opt/rfmap

COPY rfmap_tiles /opt/rfmap/rfmap_tiles
COPY styles /opt/rfmap/styles
COPY bin/rfmap-tiles /usr/local/bin/rfmap-tiles

# Not root: run it as yourself (docker run --user "$(id -u):$(id -g)") so the files in /data are
# yours; without --user it runs as the image's unprivileged "ubuntu" user (uid 1000).
USER ubuntu
WORKDIR /data
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/rfmap-tiles"]
CMD ["--help"]
