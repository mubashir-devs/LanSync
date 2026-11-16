#!/usr/bin/env bash
# Installs LanSync's build dependencies on Debian/Ubuntu using apt (needs sudo).
# Mirrors vcpkg.json; vcpkg is NOT required on Linux.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build git curl zip unzip tar pkg-config \
  qt6-base-dev qt6-declarative-dev qt6-tools-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-window qml6-module-qtquick-templates qml6-module-qtquick-dialogs \
  qml6-module-qtqml-workerscript \
  libboost-dev libboost-system-dev libssl-dev libsqlite3-dev libspdlog-dev \
  nlohmann-json3-dev libgtest-dev libgmock-dev libgl1-mesa-dev libxkbcommon-dev
echo "Done. Next: cmake --preset dev && cmake --build --preset dev && ctest --preset dev"
