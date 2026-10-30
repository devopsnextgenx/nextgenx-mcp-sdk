FROM ubuntu:25.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        git \
        jq \
        libclang-rt-dev \
        libssl-dev \
        nodejs \
        npm \
        openssl \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["bash"]
