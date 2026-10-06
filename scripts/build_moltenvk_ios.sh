#!/bin/bash
# Build MoltenVK for iOS (arm64) and stage it where the app project expects it.
#
# Unreal Engine 4 games (Minecraft Dungeons) draw with Vulkan, and on Apple hardware Vulkan is MoltenVK. The Mac one from Homebrew is no use on a phone, so this
# builds KhronosGroup/MoltenVK from source: `fetchDependencies --ios` clones and builds SPIRV-Cross and SPIRV-Tools, `make ios` produces a dynamic MoltenVK.framework
# in an xcframework. project.yml embeds build/ios-arm64/lib/MoltenVK.xcframework; the native runtime opens it by path (husk_ue4_set_vulkan).
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${MOLTENVK_VERSION:-v1.4.2}"
SRC="$HUSK_ROOT/third_party/build/MoltenVK-${VERSION#v}"
OUT="$HUSK_ROOT/build/ios-arm64/lib"

if [ ! -d "$SRC" ]; then
    echo "==> cloning MoltenVK $VERSION"
    git clone --depth 1 --branch "$VERSION" https://github.com/KhronosGroup/MoltenVK "$SRC"
fi
cd "$SRC"
[ -d External/build/Release/SPIRVCross.xcframework ] || { echo "==> fetching dependencies"; ./fetchDependencies --ios; }
echo "==> building"
make ios
mkdir -p "$OUT"
rm -rf "$OUT/MoltenVK.xcframework"
cp -R Package/Release/MoltenVK/dynamic/MoltenVK.xcframework "$OUT/"
echo "==> $OUT/MoltenVK.xcframework"
