# Backend container for local testing
FROM debian:bookworm AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libssl-dev \
    libbrotli-dev \
    libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*

# Build Drogon from source (Bookworm doesn't ship libdrogon-dev)
RUN git clone --branch v1.9.7 --depth 1 --recurse-submodules \
      https://github.com/drogonframework/drogon.git /tmp/drogon \
 && cmake -S /tmp/drogon -B /tmp/drogon/build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /tmp/drogon/build --target install -- -j"$(nproc)" \
 && ldconfig \
 && rm -rf /tmp/drogon

WORKDIR /app
COPY . /app

# Build your server (Drogon installs into /usr/local by default)
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local \
 && cmake --build build --target college_ff_server -- -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libjsoncpp25 \
    libuuid1 \
    zlib1g \
    libbrotli1 \
    libsqlite3-0 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /srv
COPY --from=build /app/build/college_ff_server /srv/college_ff_server

EXPOSE 8080
ENV PORT=8080
CMD ["/srv/college_ff_server"]
