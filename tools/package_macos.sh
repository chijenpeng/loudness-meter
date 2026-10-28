#!/bin/sh
# Builds nothing new beyond the AU and VST3 targets of build-release, stages ad-hoc signed copies
# and writes Loudness-Meter-<version>-macos-universal.zip plus its .sha256 under build-release/dist/.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-release"
DIST="$BUILD/dist"
VERSION=$(sed -n 's/^project(LoudnessMeter VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
BUNDLE_ID=$(sed -n 's/^[[:space:]]*BUNDLE_ID "\([^"]*\)".*/\1/p' "$ROOT/CMakeLists.txt")
MANUFACTURER=$(sed -n 's/^[[:space:]]*PLUGIN_MANUFACTURER_CODE \([^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt")
PLUGIN_CODE=$(sed -n 's/^[[:space:]]*PLUGIN_CODE \([^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt")
test -n "$VERSION" && test -n "$BUNDLE_ID" && test -n "$MANUFACTURER" && test -n "$PLUGIN_CODE" || {
    echo "Could not determine CMake project version/identity" >&2; exit 1;
}

# Refuse to package a tree that differs from a commit: the package records that commit.
git -C "$ROOT" rev-parse --verify HEAD >/dev/null 2>&1 || { echo "No source commit exists" >&2; exit 1; }
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo "Working tree is dirty; commit or stash changes before packaging" >&2; exit 1
fi
COMMIT=$(git -C "$ROOT" rev-parse HEAD)

cmake --build "$BUILD" --target LoudnessMeter_AU LoudnessMeter_VST3 -j3
NAME="Loudness-Meter-$VERSION-macos-universal"
STAGE="$DIST/$NAME"
ARCHIVE="$DIST/$NAME.zip"
AU="$BUILD/LoudnessMeter_artefacts/Release/AU/Loudness Meter.component"
VST3="$BUILD/LoudnessMeter_artefacts/Release/VST3/Loudness Meter.vst3"

for path in "$AU" "$VST3"; do
    test -d "$path" || { echo "Missing built plug-in: $path" >&2; exit 1; }
    plist="$path/Contents/Info.plist"
    for key in CFBundleVersion CFBundleShortVersionString; do
        actual=$(plutil -extract "$key" raw -o - "$plist")
        test "$actual" = "$VERSION" || { echo "Stale $key in $path: $actual (expected $VERSION)" >&2; exit 1; }
    done
    actual=$(plutil -extract CFBundleIdentifier raw -o - "$plist")
    test "$actual" = "$BUNDLE_ID" || { echo "Stale bundle identity in $path: $actual (expected $BUNDLE_ID)" >&2; exit 1; }
done
actual=$(plutil -extract AudioComponents.0.manufacturer raw -o - "$AU/Contents/Info.plist")
test "$actual" = "$MANUFACTURER" || { echo "Stale AU manufacturer: $actual" >&2; exit 1; }
actual=$(plutil -extract AudioComponents.0.subtype raw -o - "$AU/Contents/Info.plist")
test "$actual" = "$PLUGIN_CODE" || { echo "Stale AU subtype: $actual" >&2; exit 1; }
for path in "$AU/Contents/MacOS/Loudness Meter" "$VST3/Contents/MacOS/Loudness Meter"; do
    arches=$(lipo -archs "$path")
    printf '%s\n' "$arches" | grep -qw arm64 || { echo "Missing arm64 in $path; got $arches" >&2; exit 1; }
    printf '%s\n' "$arches" | grep -qw x86_64 || { echo "Missing x86_64 in $path; got $arches" >&2; exit 1; }
done

JUCE=$(CDPATH= cd -- "$ROOT/third_party/JUCE" 2>/dev/null && pwd -P || true)
if [ -z "$JUCE" ] || [ ! -f "$JUCE/LICENSE.md" ]; then
    JUCE=$(find "$BUILD/_deps" -maxdepth 1 -type d -name 'juce-src' 2>/dev/null | head -n 1)
fi
test -n "$JUCE" && test -f "$JUCE/LICENSE.md" || { echo "JUCE checkout not found" >&2; exit 1; }
SDK="$JUCE/modules/juce_audio_processors/format_types/VST3_SDK"

rm -rf "$STAGE"
mkdir -p "$STAGE/licenses/VST3-SDK"
ditto "$AU" "$STAGE/Loudness Meter.component"
ditto "$VST3" "$STAGE/Loudness Meter.vst3"
cp "$ROOT/README.md" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/LICENSE" "$STAGE/"
cp "$ROOT/Resources/fonts/Inter-LICENSE.txt" "$ROOT/Resources/fonts/JetBrainsMono-LICENSE.txt" "$STAGE/licenses/"
cp "$JUCE/LICENSE.md" "$STAGE/licenses/JUCE-LICENSE.md"
cp "$SDK/LICENSE.txt" "$STAGE/licenses/VST3-SDK/VST3-SDK-LICENSE.txt"
cp "$SDK/base/LICENSE.txt" "$STAGE/licenses/VST3-SDK/base-LICENSE.txt"
cp "$SDK/pluginterfaces/LICENSE.txt" "$STAGE/licenses/VST3-SDK/pluginterfaces-LICENSE.txt"
cp "$SDK/public.sdk/LICENSE.txt" "$STAGE/licenses/VST3-SDK/public-sdk-LICENSE.txt"
cp "$SDK/VST3_License_Agreement.pdf" "$STAGE/licenses/VST3-SDK/"
codesign --force --deep --sign - "$STAGE/Loudness Meter.component"
codesign --force --deep --sign - "$STAGE/Loudness Meter.vst3"
codesign --verify --deep --strict "$STAGE/Loudness Meter.component"
codesign --verify --deep --strict "$STAGE/Loudness Meter.vst3"

cat > "$STAGE/INSTALL.txt" <<TXT
Loudness Meter $VERSION - macOS universal (arm64 + x86_64)

Copy "Loudness Meter.component" to ~/Library/Audio/Plug-Ins/Components/ and "Loudness Meter.vst3" to
~/Library/Audio/Plug-Ins/VST3/, then rescan plug-ins in your host.

The bundles are ad-hoc signed, not Developer ID signed or notarized. If macOS blocks a bundle
downloaded from the internet, remove the quarantine flag after checking the SHA-256 file:
  xattr -dr com.apple.quarantine ~/"Library/Audio/Plug-Ins/VST3/Loudness Meter.vst3"
  xattr -dr com.apple.quarantine ~/"Library/Audio/Plug-Ins/Components/Loudness Meter.component"

See README.md for the controls and limits, THIRD_PARTY_NOTICES.md and licenses/ for dependency
notices, LICENSE for the project licence and BUILD-INFO.txt for build provenance.
TXT

cat > "$STAGE/BUILD-INFO.txt" <<TXT
Product: Loudness Meter
Version: $VERSION
Source commit: $COMMIT
Build: macOS arm64 and x86_64; AU and VST3
Signing: ad-hoc signatures verified; not Developer ID signed or notarized
Windows: not built by this package
Identity: $BUNDLE_ID; manufacturer $MANUFACTURER; plug-in code $PLUGIN_CODE
TXT

rm -f "$ARCHIVE" "$ARCHIVE.sha256"
ditto -c -k --norsrc --keepParent "$STAGE" "$ARCHIVE"
unzip -t "$ARCHIVE" >/dev/null
VERIFY="$DIST/.verify-$NAME"
rm -rf "$VERIFY"
mkdir -p "$VERIFY"
ditto -x -k "$ARCHIVE" "$VERIFY"
EXTRACTED="$VERIFY/$NAME"
for bundle in "Loudness Meter.component" "Loudness Meter.vst3"; do
    binary="$EXTRACTED/$bundle/Contents/MacOS/Loudness Meter"
    test -x "$binary" || { echo "Archived executable missing or not executable: $binary" >&2; exit 1; }
    arches=$(lipo -archs "$binary")
    printf '%s\n' "$arches" | grep -qw arm64 || { echo "Archived binary missing arm64: $binary" >&2; exit 1; }
    printf '%s\n' "$arches" | grep -qw x86_64 || { echo "Archived binary missing x86_64: $binary" >&2; exit 1; }
    codesign --verify --deep --strict "$EXTRACTED/$bundle"
done
for f in README.md INSTALL.txt BUILD-INFO.txt THIRD_PARTY_NOTICES.md LICENSE licenses/JUCE-LICENSE.md \
         licenses/Inter-LICENSE.txt licenses/JetBrainsMono-LICENSE.txt \
         licenses/VST3-SDK/VST3-SDK-LICENSE.txt licenses/VST3-SDK/VST3_License_Agreement.pdf; do
    test -s "$EXTRACTED/$f" || { echo "Archived file missing or empty: $f" >&2; exit 1; }
done
grep -q "^Source commit: $COMMIT\$" "$EXTRACTED/BUILD-INFO.txt"
(cd "$DIST" && shasum -a 256 "$NAME.zip" > "$NAME.zip.sha256")
rm -rf "$VERIFY"
printf 'Created %s\n' "$ARCHIVE"
