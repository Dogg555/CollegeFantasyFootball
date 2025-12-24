#!/usr/bin/env bash

# Installs system dependencies for the College Fantasy Football project.
# Targets Debian/Ubuntu hosts. For other platforms, install equivalent packages.

set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "This helper currently supports apt-based systems (Debian/Ubuntu)." >&2
  echo "Please install CMake, a C++17 toolchain, Drogon, and OpenSSL manually." >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  libjsoncpp-dev \
  uuid-dev \
  zlib1g-dev \
  libssl-dev \
  libdrogon-dev

echo "Dependencies installed. You can now build the backend with CMake."
