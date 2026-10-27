ARG KOBALT_EMULATOR_BASE=ubuntu:24.04
FROM ${KOBALT_EMULATOR_BASE}

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    binutils build-essential ca-certificates curl e2fsprogs git libarchive-dev \
    nettle-dev pkg-config xz-utils zlib1g-dev zstd python3 python3-venv python3-pip \
    ninja-build patch libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev libpng-dev \
    openssh-client socat rsync flex bison gettext \
    && rm -rf /var/lib/apt/lists/*

ENV XDG_CACHE_HOME=/work/build/kindle-emulator/cache
ENV PIP_CACHE_DIR=/work/build/kindle-emulator/cache/pip
ENV PYTHONPYCACHEPREFIX=/work/build/kindle-emulator/cache/pycache
WORKDIR /emu
