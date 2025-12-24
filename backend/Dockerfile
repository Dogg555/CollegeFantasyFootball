# Backend container for local testing
FROM debian:bookworm as build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libssl-dev \
    libdrogon-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN cmake -S . -B build -DDROGON_FOUND=ON \
    && cmake --build build --target college_ff_server -- -j$(nproc)

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y libssl3 && rm -rf /var/lib/apt/lists/*
WORKDIR /srv
COPY --from=build /app/build/college_ff_server /srv/college_ff_server
EXPOSE 8080

ENV PORT=8080
CMD ["/srv/college_ff_server"]
