#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$ROOT/tools/toolchain.lock.json"
TARGET="$ROOT/tools/toolchains/llvm-mingw-linux"
VERSION="$(jq -r .llvm_version "$LOCK")"
ASSET="$(jq -r .codex_cloud_linux.asset "$LOCK")"
EXPECTED="$(jq -r .codex_cloud_linux.sha256 "$LOCK")"
REPO="$(jq -r .repository "$LOCK")"
TAG="$(jq -r .release_tag "$LOCK")"
missing=()
for cmd in curl jq tar xz sha256sum cmake ninja; do command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd"); done
if ((${#missing[@]})); then
  if [[ "$(id -u)" == "0" ]]; then APT=(apt-get);
  elif command -v sudo >/dev/null 2>&1; then APT=(sudo apt-get);
  else echo "Missing prerequisites (${missing[*]}) and no root/sudo apt access." >&2; exit 1; fi
  "${APT[@]}" update
  "${APT[@]}" install -y --no-install-recommends curl jq tar xz-utils coreutils cmake ninja-build
fi
API="$(mktemp)"
TMP="$(mktemp -d)"
trap 'rm -f "$API"; rm -rf "$TMP"' EXIT
curl --fail --location --retry 3 --silent --show-error -H "Accept: application/vnd.github+json" -H "User-Agent: PM-Lite-bootstrap" "https://api.github.com/repos/$REPO/releases/tags/$TAG" -o "$API"
test "$(jq -r .tag_name "$API")" = "$TAG"
test "$(jq -r --arg n "$ASSET" '[.assets[] | select(.name==$n)] | length' "$API")" = 1
URL="$(jq -r --arg n "$ASSET" '.assets[] | select(.name==$n) | .browser_download_url' "$API")"
DIGEST="$(jq -r --arg n "$ASSET" '.assets[] | select(.name==$n) | .digest' "$API")"
test "$DIGEST" = "sha256:$EXPECTED"
curl --fail --location --retry 3 --show-error "$URL" -o "$TMP/$ASSET"
echo "$EXPECTED  $TMP/$ASSET" | sha256sum --check -
tar -xJf "$TMP/$ASSET" -C "$TMP"
COMPILER="$(find "$TMP" -type f -path "*/bin/clang++" -print -quit)"
test -n "$COMPILER"
EXTRACTED="$(dirname "$(dirname "$COMPILER")")"
test -x "$EXTRACTED/bin/llvm-readobj"
test -d "$EXTRACTED/x86_64-w64-mingw32"
"$COMPILER" --version | grep -F "clang version $VERSION"
mkdir -p "$(dirname "$TARGET")"
if [[ -e "$TARGET" ]]; then mv "$TARGET" "$TARGET.previous.$(date -u +%Y%m%dT%H%M%SZ)"; fi
mv "$EXTRACTED" "$TARGET"
echo "LLVM-MinGW $VERSION installed and SHA-256 verified at tools/toolchains/llvm-mingw-linux."
