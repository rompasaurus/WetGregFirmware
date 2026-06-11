FROM ubuntu:24.04

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install the ARM cross-compilation toolchain and build tools
RUN apt-get update && apt-get install -y \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    cmake \
    ninja-build \
    python3 \
    git \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Clone the Pico SDK into the container
RUN git clone --recurse-submodules --depth 1 \
    https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk

ENV PICO_SDK_PATH=/opt/pico-sdk

WORKDIR /project

# Display variant (V2, V3, V3a, V4) — override via docker compose environment
ENV DISPLAY_VARIANT=V4

# Pico board type — "pico_w" for Pico W (RP2040), "pico2_w" for Pico 2 W (RP2350)
ENV PICO_BOARD=pico_w

# Default command: copy SDK import helper, configure + build
CMD ["sh", "-c", "cp /opt/pico-sdk/external/pico_sdk_import.cmake . 2>/dev/null; rm -rf build && mkdir build && cd build && cmake -G Ninja -DPICO_BOARD=${PICO_BOARD} -DDISPLAY_VARIANT=${DISPLAY_VARIANT} .. && ninja"]
