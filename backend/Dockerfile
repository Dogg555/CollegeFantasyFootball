# Backend container for local testing
FROM debian:bookworm AS build

ENV DEBIAN_FRONTEND=noninteractive
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
 && update-ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Build Drogon from source (Bookworm doesn't ship libdrogon-dev)
RUN git config --global http.version HTTP/1.1 \
 && git clone --branch v1.9.7 --depth 1 https://github.com/drogonframework/drogon.git /tmp/drogon \
 && cd /tmp/drogon \
 && git submodule update --init --recursive --depth 1 \
 && cmake -S /tmp/drogon -B /tmp/drogon/build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /tmp/drogon/build --target install -- -j"$(nproc)" \
 && ldconfig \
 && rm -rf /tmp/drogon

WORKDIR /app
COPY . /app

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local \
 && cmake --build build --target college_ff_server -- -j"$(nproc)"

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    libjsoncpp25 \
    libcrypt1 \
    libuuid1 \
    zlib1g \
    libpq5 \
    libbrotli1 \
    libsqlite3-0 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /srv
COPY --from=build /app/build/college_ff_server /srv/college_ff_server

EXPOSE 8080
ENV PORT=8080
CMD ["/srv/college_ff_server"]
