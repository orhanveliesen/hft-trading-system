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
    clang-format \
    lcov \
    git \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Metadata for GHCR
LABEL org.opencontainers.image.source=https://github.com/orhanveliesen/hft-trading-system
LABEL org.opencontainers.image.description="HFT Trading System Builder Image"
LABEL org.opencontainers.image.licenses=MIT
