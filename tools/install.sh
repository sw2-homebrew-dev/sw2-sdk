#!/bin/sh
# Install a prebuilt sw2-sdk release (Linux or macOS).
#
#   curl -fsSL https://github.com/sw2-homebrew-dev/sw2-sdk/releases/latest/download/install.sh | sh
#
# Environment
#   SW2_VERSION   release tag to install (default: latest)
#   SW2_HOME      install folder        (default: ~/.local/share/sw2-sdk)
#   SW2_BIN       folder for the `sw2` link (default: ~/.local/bin)
#   SW2_SDK_URL   tarball URL or local path (overrides SW2_VERSION; for mirrors/tests)
#
# The tarball is checked against its published .sha256 before anything is
# replaced; the previous install is kept until the new one is in place.
set -eu

REPO=https://github.com/sw2-homebrew-dev/sw2-sdk
HOME_DIR=${SW2_HOME:-$HOME/.local/share/sw2-sdk}
BIN_DIR=${SW2_BIN:-$HOME/.local/bin}
if [ -n "${SW2_SDK_URL:-}" ]; then
  URL=$SW2_SDK_URL
elif [ -n "${SW2_VERSION:-}" ]; then
  URL=$REPO/releases/download/$SW2_VERSION/sw2-sdk.tar.gz
else
  URL=$REPO/releases/latest/download/sw2-sdk.tar.gz
fi

say() { printf 'sw2-install: %s\n' "$*"; }
die() { say "$*" >&2; exit 1; }
fetch() {
  case $1 in
    http://*|https://*) curl -fsSL "$1" -o "$2" ;;
    file://*) cp "${1#file://}" "$2" ;;
    *) cp "$1" "$2" ;;
  esac
}
sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi
}

command -v tar >/dev/null || die "tar is required"
command -v curl >/dev/null || case $URL in http*) die "curl is required";; esac

WORK=$(mktemp -d "${TMPDIR:-/tmp}/sw2-install.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

say "downloading $URL"
fetch "$URL" "$WORK/sw2-sdk.tar.gz" || die "download failed"
fetch "$URL.sha256" "$WORK/sw2-sdk.tar.gz.sha256" || die "checksum download failed"
want=$(cut -d' ' -f1 < "$WORK/sw2-sdk.tar.gz.sha256")
got=$(sha256 "$WORK/sw2-sdk.tar.gz" | cut -d' ' -f1)
[ -n "$want" ] && [ "$want" = "$got" ] || die "sha256 mismatch: expected $want, got $got"
say "sha256 ok ($got)"

mkdir -p "$WORK/unpack"
tar xzf "$WORK/sw2-sdk.tar.gz" -C "$WORK/unpack"
[ -x "$WORK/unpack/sw2-sdk/sw2" ] || die "archive does not contain sw2-sdk/sw2"

mkdir -p "$(dirname "$HOME_DIR")" "$BIN_DIR"
if [ -e "$HOME_DIR" ]; then
  rm -rf "$HOME_DIR.old"
  mv "$HOME_DIR" "$HOME_DIR.old"
fi
mv "$WORK/unpack/sw2-sdk" "$HOME_DIR"
rm -rf "$HOME_DIR.old"
ln -sf "$HOME_DIR/sw2" "$BIN_DIR/sw2"

VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$HOME_DIR/BUNDLE.json" 2>/dev/null | head -1)
say "installed sw2-sdk ${VERSION:-} to $HOME_DIR"
say "linked $BIN_DIR/sw2"
case ":$PATH:" in *":$BIN_DIR:"*) ;; *) say "add to your shell profile: export PATH=\"$BIN_DIR:\$PATH\"";; esac
say "add to your shell profile: export SW2_SDK=\"$HOME_DIR\""
say "next: sw2 doctor   (installs nothing; lists the toolchain for your OS)"
