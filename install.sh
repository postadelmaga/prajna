#!/bin/sh
# Build the ChatAI plasmoid (engine + QML plugin + package) and install it
# for the current user into ~/.local. Pass a prefix to install elsewhere,
# e.g. `sudo ./install.sh /usr`.
set -eu
cd "$(dirname "$0")"
PREFIX="${1:-$HOME/.local}"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build build -j"$(nproc)"
cmake --install build
echo
echo "Installed into $PREFIX/share/plasma/plasmoids/ChatAI-Plasmoid"
echo "If the widget was already in use, restart Plasma:"
echo "  systemctl --user restart plasma-plasmashell"
