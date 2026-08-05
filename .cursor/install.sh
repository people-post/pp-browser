#!/usr/bin/env bash
#
# Cloud Agent install step for pp-browser (Frame).
#
# Idempotent: installs the Linux system packages the desktop shell needs
# (X11/GL windowing, PulseAudio + ALSA voice, VA-API video, D-Bus
# notifications), then configures and builds the project so a fresh agent
# starts with a warm build tree. Safe to re-run.
set -euo pipefail

cd "$(dirname "$0")/.."

# --- System packages (mirrors .github/workflows/build.yml + BUILD.md) ---
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build perl pkg-config ccache \
  libx11-dev libxext-dev libxcursor-dev libxinerama-dev libxi-dev \
  libxrandr-dev libxfixes-dev libxrender-dev libxss-dev \
  libgl-dev libgl1-mesa-dri libglu1-mesa \
  libdbus-1-dev libssl-dev libpulse-dev libasound2-dev libva-dev \
  xvfb x11-utils

# --- Configure ---
# The default `c++`/`cc` alternatives on this base image point at clang, which
# lacks a working libstdc++ include/link setup. Pin GCC explicitly (this also
# matches the compiler CI uses on ubuntu-24.04 runners). Use absolute paths so
# reconfiguring an existing build directory does not misresolve the tool name.
cc_path="$(command -v gcc)"
cxx_path="$(command -v g++)"
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DPP_BROWSER_COMPILER_CACHE=ON \
  -DCMAKE_C_COMPILER="${cc_path}" \
  -DCMAKE_CXX_COMPILER="${cxx_path}"

# --- Build (app + headless pp-node + host tests) ---
cmake --build build -j "$(nproc)"
