#!/bin/sh
# Marlin installer for Linux and macOS.
#
#   curl -fsSL https://github.com/MehmetEminKaymaz/marlin/releases/latest/download/install.sh | sh
#
# Downloads the compiler package for this machine from the GitHub release,
# checks its SHA-256, unpacks it into ~/.marlin (replacing an older version),
# adds ~/.marlin/bin to PATH in your shell profile and runs `marlin doctor`.
# Running it again updates Marlin.
#
# Options (environment variables):
#   MARLIN_VERSION=0.1.0       install this version instead of the latest
#   MARLIN_HOME=/path          install directory (default: ~/.marlin)
#   MARLIN_NO_MODIFY_PATH=1    do not touch shell profiles
#   MARLIN_INSTALL_FROM=file   install this package archive (for testing)
#
# Uninstall:  curl -fsSL .../install.sh | sh -s -- --uninstall
#
# (Messages are in English: the installer is the first thing a new user sees.)

set -eu

REPO="MehmetEminKaymaz/marlin"
HOME_DIR="${MARLIN_HOME:-$HOME/.marlin}"
PROFILE_LINE="export PATH=\"$HOME_DIR/bin:\$PATH\"  # marlin"

say()  { printf '%s\n' "$*"; }
fail() { printf 'marlin install: %s\n' "$*" >&2; exit 1; }

profiles() {
    # Profiles that a login / interactive shell reads.
    for f in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
        [ -f "$f" ] && printf '%s\n' "$f"
    done
    return 0
}

uninstall() {
    rm -rf "$HOME_DIR"
    for f in $(profiles); do
        if grep -q '# marlin$' "$f"; then
            grep -v '# marlin$' "$f" > "$f.marlin-tmp" && mv "$f.marlin-tmp" "$f"
            say "removed PATH entry from $f"
        fi
    done
    say "Marlin removed ($HOME_DIR)."
    exit 0
}

[ "${1:-}" = "--uninstall" ] && uninstall

# --- platform -----------------------------------------------------------------
case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=macos ;;
    *) fail "unsupported system $(uname -s); on Windows use install.ps1" ;;
esac
case "$(uname -m)" in
    x86_64|amd64)  arch=x86_64 ;;
    arm64|aarch64) arch=arm64 ;;
    *) fail "unsupported processor $(uname -m)" ;;
esac
asset="marlin-$os-$arch.tar.gz"

if [ -n "${MARLIN_VERSION:-}" ]; then
    base="https://github.com/$REPO/releases/download/v${MARLIN_VERSION#v}"
else
    base="https://github.com/$REPO/releases/latest/download"
fi

download() { # url file
    if command -v curl > /dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$2" "$1"
    elif command -v wget > /dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        fail "curl or wget is required"
    fi
}

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# --- download and verify ------------------------------------------------------
if [ -n "${MARLIN_INSTALL_FROM:-}" ]; then
    say "Installing from $MARLIN_INSTALL_FROM"
    cp "$MARLIN_INSTALL_FROM" "$tmp/$asset"
else
    say "Downloading $asset ..."
    download "$base/$asset" "$tmp/$asset" || fail "download failed: $base/$asset"
    download "$base/SHA256SUMS" "$tmp/SHA256SUMS" || fail "download failed: $base/SHA256SUMS"
    expected="$(grep " $asset\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)"
    [ -n "$expected" ] || fail "$asset is not listed in SHA256SUMS"
    [ "$(sha256 "$tmp/$asset")" = "$expected" ] || fail "checksum mismatch for $asset; try again"
fi

# --- unpack -------------------------------------------------------------------
# The archive has one top-level directory (marlin-<version>-<os>-<arch>/).
mkdir "$tmp/new"
tar -xzf "$tmp/$asset" -C "$tmp/new" --strip-components=1 || fail "could not unpack $asset"
[ -x "$tmp/new/bin/marlin" ] || fail "unexpected package layout"

mkdir -p "$(dirname "$HOME_DIR")"
rm -rf "$HOME_DIR.old"
[ -d "$HOME_DIR" ] && mv "$HOME_DIR" "$HOME_DIR.old"
mv "$tmp/new" "$HOME_DIR"
rm -rf "$HOME_DIR.old"
# (Files fetched with curl are not quarantined, but an archive copied in from
# a browser download would be.)
[ "$os" = macos ] && xattr -dr com.apple.quarantine "$HOME_DIR" 2> /dev/null || true

say "Installed $("$HOME_DIR/bin/marlin" --version) into $HOME_DIR"

# --- PATH ---------------------------------------------------------------------
case ":$PATH:" in
    *":$HOME_DIR/bin:"*) on_path=1 ;;
    *) on_path=0 ;;
esac
if [ "$on_path" = 0 ] && [ -z "${MARLIN_NO_MODIFY_PATH:-}" ]; then
    added=""
    targets="$(profiles)"
    # macOS uses zsh by default; make sure it gets the entry even without a .zshrc.
    if [ -z "$targets" ]; then
        if [ "$os" = macos ]; then targets="$HOME/.zshrc"; else targets="$HOME/.profile"; fi
    fi
    for f in $targets; do
        if ! grep -q '# marlin$' "$f" 2> /dev/null; then
            printf '\n%s\n' "$PROFILE_LINE" >> "$f"
            added="$added $f"
        fi
    done
    if command -v fish > /dev/null 2>&1; then
        fish -c "fish_add_path -U '$HOME_DIR/bin'" 2> /dev/null && added="$added fish"
    fi
    [ -n "$added" ] && say "Added $HOME_DIR/bin to PATH in:$added"
    say "Open a new terminal (or run: export PATH=\"$HOME_DIR/bin:\$PATH\") to use 'marlin'."
fi

# --- check --------------------------------------------------------------------
say ""
"$HOME_DIR/bin/marlin" doctor || true
