#!/bin/sh
# selects the runner's newest Xcode: Mullion builds with the macOS 26.4 SDK (README, Building)
set -e
sudo xcode-select -s "$(find /Applications -maxdepth 1 -name 'Xcode_*.app' | sort -V | tail -1)"
sdk=$(xcrun --sdk macosx --show-sdk-version)
echo "Xcode $(xcodebuild -version | head -1 | cut -d' ' -f2), macOS SDK $sdk"
[ "$(printf '%s\n' 26.4 "$sdk" | sort -V | head -1)" = 26.4 ] ||
  { echo "the runner's newest macOS SDK is $sdk, before 26.4" >&2; exit 1; }
# Xcode 26 ships without the Metal toolchain, which airconv's and nativemetal's .metal sources need
xcrun -f metal > /dev/null 2>&1 || xcodebuild -downloadComponent MetalToolchain
