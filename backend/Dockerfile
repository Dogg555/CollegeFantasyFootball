# Backend container for local testing and Render deployment
FROM debian:bookworm AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS=1
ENV CMAKE_BUILD_PARALLEL_LEVEL=${BUILD_JOBS}

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    build-essential \
    cmake \
    git \
    pkg-config \
    libjsoncpp-dev \
    libcrypt-dev \
    uuid-dev \
    zlib1g-dev \
    libssl-dev \
    libpq-dev \
    libbrotli-dev \
    libsqlite3-dev \
    libpqxx-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
 && update-ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN git config --global http.version HTTP/1.1 \
 && git clone --branch v1.9.7 --depth 1 https://github.com/drogonframework/drogon.git /tmp/drogon \
 && cd /tmp/drogon \
 && git submodule update --init --recursive --depth 1 \
 && cmake -S /tmp/drogon -B /tmp/drogon/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG" \
        -DBUILD_CTL=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_ORM=OFF \
        -DBUILD_DOC=OFF \
        -DBUILD_BROTLI=OFF \
        -DBUILD_YAML_CONFIG=OFF \
        -DBUILD_TESTING=OFF \
 && cmake --build /tmp/drogon/build --target install --parallel "${BUILD_JOBS}" \
 && ldconfig \
 && rm -rf /tmp/drogon

RUN git clone --branch 1.10.5 --depth 1 https://github.com/libcpr/cpr.git /tmp/cpr \
 && cmake -S /tmp/cpr -B /tmp/cpr/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG" \
        -DCPR_BUILD_TESTS=OFF \
        -DCPR_BUILD_EXAMPLES=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCPR_FORCE_USE_SYSTEM_CURL=ON \
 && cmake --build /tmp/cpr/build --target install --parallel "${BUILD_JOBS}" \
 && ldconfig \
 && rm -rf /tmp/cpr

WORKDIR /app
COPY . /app

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O0 -g0 -DNDEBUG" \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
        -DCMAKE_PREFIX_PATH=/usr/local \
 && cmake --build build --target college_ff_server --parallel "${BUILD_JOBS}" --verbose

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    postgresql-client \
    libssl3 \
    libjsoncpp25 \
    libcrypt1 \
    libuuid1 \
    zlib1g \
    libpq5 \
    libbrotli1 \
    libsqlite3-0 \
    libpqxx-6.4 \
    libcurl4 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 10001 cff \
 && useradd --system --uid 10001 --gid cff --home-dir /srv --shell /usr/sbin/nologin cff

WORKDIR /srv
COPY --from=build --chown=cff:cff /app/build/college_ff_server /srv/college_ff_server
COPY --from=build --chown=cff:cff /app/db /srv/db

USER 10001:10001
EXPOSE 8080
ENV PORT=8080
CMD ["/srv/college_ff_server"]
