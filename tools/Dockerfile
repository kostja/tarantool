FROM ubuntu:20.04

ENV TZ=Europe/Moscow

RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt update && \
    apt upgrade -y && \
    apt install -y \
    git \
    build-essential \
    cmake \
    make \
    zlib1g-dev \
    libreadline-dev \
    libncurses5-dev \
    libssl-dev \
    libunwind-dev \
    libicu-dev \
    libyaml-dev \
    libcurl4-openssl-dev  \
    python3 \
    python3-yaml \
    python3-six \
    python3-gevent \
    python3-pip \
    python3-dev \
    python3-setuptools \
    luarocks \
    coreutils \
    sed \
    lcov \
    ruby \
    clang \
    llvm \
    llvm-dev \
    zlib1g-dev \
    autoconf \
    automake \
    libtool \
    ninja-build

RUN luarocks install luacheck
