FROM ubuntu:22.04

# Prevent interactive prompts during apt install
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    libwebsockets-dev \
    libglfw3-dev \
    libgl1-mesa-dev \
    libcurl4-openssl-dev \
    cmake \
    build-essential \
    lcov \
    git \
    pkg-config \
    wget \
    software-properties-common \
    gpg \
    && rm -rf /var/lib/apt/lists/*

# Install clang-format-16 from LLVM repository (supports C++23)
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - \
    && add-apt-repository "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-16 main" \
    && apt-get update \
    && apt-get install -y clang-format-16 \
    && ln -s /usr/bin/clang-format-16 /usr/bin/clang-format \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Metadata for GHCR
LABEL org.opencontainers.image.source=https://github.com/orhanveliesen/hft-trading-system
LABEL org.opencontainers.image.description="HFT Trading System Builder Image"
LABEL org.opencontainers.image.licenses=MIT
