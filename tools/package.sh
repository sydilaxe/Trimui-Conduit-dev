#!/bin/sh
# Build release zips from an already-built ./conduit (aarch64 for devices).
set -e
cd "$(dirname "$0")/.."
VER="${1:-dev}"
rm -rf build/pkg && mkdir -p build/pkg dist

# MustardOS: /mnt/mmc/MUOS/application/Conduit/
M=build/pkg/muos/Conduit
mkdir -p "$M/recordings"
cp conduit pkg/muos/mux_launch.sh pkg/conduit_diag.sh "$M/"
cp README.md "$M/README.md"
(cd build/pkg/muos && zip -qr "../../../dist/Conduit-$VER-MustardOS.zip" Conduit)

# KNULLI: /userdata/roms/ports/{Conduit.sh,conduit/}
K=build/pkg/knulli
mkdir -p "$K/conduit/recordings"
cp conduit pkg/conduit_diag.sh "$K/conduit/"
cp README.md "$K/conduit/README.md"
cp pkg/knulli/Conduit.sh "$K/"
(cd "$K" && zip -qr "../../../dist/Conduit-$VER-KNULLI.zip" Conduit.sh conduit)

chmod +x "$M/mux_launch.sh" "$K/Conduit.sh"
ls -l dist
