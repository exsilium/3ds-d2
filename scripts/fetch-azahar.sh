#!/usr/bin/env bash
# A portable Azahar for this project only, under build/azahar/<version>/: the
# release recorded in docs/compatibility.md, for this host (macOS, Linux, or
# Windows under Git Bash). A user/ folder next to the binary makes Azahar
# portable: its settings and emulated SD card stay in
# build/azahar/<version>/<dir>/user/, apart from any Azahar installed on the
# machine. scripts/run-game-azahar.sh uses this copy when it is there.
#
#   scripts/fetch-azahar.sh            fetch (once) and print the binary's path
#
# Each download is checked against packaging/azahar.sha256 when the file
# names it; a download it does not name is recorded there (first fetch).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=2126.2
BASE="https://github.com/azahar-emu/azahar/releases/download/$VERSION"
DEST="$ROOT/build/azahar/$VERSION"
SUMS="$ROOT/packaging/azahar.sha256"
die() { echo "fetch-azahar.sh: $*" >&2; exit 1; }

case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*) asset="azahar-windows-msvc-$VERSION.zip" ;;
Darwin) asset="azahar-macos-universal-$VERSION.zip" ;;
Linux) asset="azahar.AppImage" ;;
*) die "no Azahar release for $(uname -s)" ;;
esac

# The binary in what was unpacked, if it is there.
find_bin() {
    [ -d "$DEST" ] || return 0
    case "$asset" in
    *windows*) find "$DEST" -name azahar.exe -type f | head -1 ;;
    *macos*) find "$DEST" -path '*Azahar.app/Contents/MacOS/azahar' -type f | head -1 ;;
    *) [ -f "$DEST/azahar.AppImage" ] && echo "$DEST/azahar.AppImage" ;;
    esac
}

bin="$(find_bin)"
if [ -z "$bin" ]; then
    mkdir -p "$DEST"
    tmp="$DEST/$asset.part"
    echo "fetching $BASE/$asset" >&2
    # From inside the folder, by relative name: Windows' own curl.exe, the
    # first on Git Bash's PATH, does not take /c/... paths.
    (cd "$DEST" && curl -fL --retry 3 -o "$asset.part" "$BASE/$asset") || die "download failed"
    sum="$(sha256sum "$tmp" 2>/dev/null || shasum -a 256 "$tmp")"
    sum="${sum%% *}"
    want="$( [ -f "$SUMS" ] && awk -v a="$asset" '$2 == a {print $1}' "$SUMS" || true)"
    if [ -n "$want" ]; then
        [ "$sum" = "$want" ] || { rm -f "$tmp"; die "$asset: sha256 $sum, packaging/azahar.sha256 says $want"; }
    else
        echo "$sum  $asset" >> "$SUMS"
        echo "recorded $asset's sha256 in packaging/azahar.sha256 (commit it)" >&2
    fi
    case "$asset" in
    *.zip)
        # unzip's 1 is a warning (the Windows zip's backslash separators).
        if command -v unzip >/dev/null; then unzip -q -o "$tmp" -d "$DEST" || [ $? -eq 1 ]
        else (cd "$DEST" && tar -xf "$asset.part"); fi   # bsdtar (Windows' tar) reads zip
        rm -f "$tmp" ;;
    *) mv "$tmp" "$DEST/azahar.AppImage"; chmod +x "$DEST/azahar.AppImage" ;;
    esac
    bin="$(find_bin)"
    [ -n "$bin" ] || die "no azahar binary in $DEST after unpacking $asset"
fi
# Portable mode: user/ next to the binary (an AppImage: next to the file).
mkdir -p "$(dirname "$bin")/user"
# Its first settings: the Vulkan renderer. On OpenGL (Windows, AMD driver)
# Azahar locks up at the game's first video frame; Azahar fills in the rest.
cfg="$(dirname "$bin")/user/config/qt-config.ini"
if [ ! -f "$cfg" ]; then
    mkdir -p "$(dirname "$cfg")"
    printf '[Renderer]\ngraphics_api\\default=false\ngraphics_api=2\n' > "$cfg"
fi
echo "$bin"
