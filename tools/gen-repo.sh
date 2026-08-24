#!/bin/bash
# regenerate the whole .ob repo. run from the project root.
# output: /media/yunli396/Data/obs  (override with OUT=...)
set -u
OUT="${OUT:-/media/yunli396/Data/obs}"
WORK="${WORK:-$OUT/.work}"
DL="$WORK/dl"
TMP="$WORK/tmp"
mkdir -p "$DL" "$TMP"
export TMPDIR="$TMP"
OBESE="$(cd "$(dirname "$0")/.." && pwd)/obese"

TOOLS="make g++ gcc clang++ vim nano tmux git curl wget rsync jq tree htop ripgrep fd-find fzf unzip zip file cmake ninja-build pkg-config autoconf automake python3 perl openssh-client coreutils procps net-tools lsof strace rustc cargo"

declare -A SEEN
ORDER=()
n=0
resolve() {
  local pkg=$1
  [[ -n "${SEEN[$pkg]+x}" ]] && return
  SEEN[$pkg]=1; ORDER+=("$pkg")
  n=$((n+1))
  local deb=$(ls "$DL/${pkg}"_*.deb 2>/dev/null | grep -v -- '-dbg\|-doc\|-examples' | head -1)
  if [[ -z "$deb" ]]; then
    (cd "$DL" && apt-get download "$pkg" 2>/dev/null >/dev/null)
    deb=$(ls "$DL/${pkg}"_*.deb 2>/dev/null | grep -v -- '-dbg\|-doc\|-examples' | head -1)
  fi
  echo "[resolve $n] $pkg"
  [[ -z "$deb" ]] && return
  local deps=$(dpkg-deb -f "$deb" Depends 2>/dev/null)
  local part name
  while IFS= read -r part; do
    [[ -z "$part" ]] && continue
    name=$(echo "$part" | sed 's/(.*//;s/|.*//;s/ //g;s/:.*//')
    [[ -n "$name" ]] && resolve "$name"
  done < <(echo "$deps" | tr ',' '\n')
}

for t in $TOOLS; do resolve "$t"; done

echo "=== 闭包含包数: ${#ORDER[@]} ==="
printf '%s\n' "${ORDER[@]}" > "$WORK/pkgs.txt"

n=0
total=${#ORDER[@]}
while read -r pkg; do
  n=$((n+1))
  [[ -f "$OUT/$pkg.ob" ]] && { echo "[$n/$total] SKIP $pkg"; continue; }
  deb=$(ls "$DL/${pkg}"_*.deb 2>/dev/null | grep -v -- '-dbg\|-doc\|-examples' | head -1)
  [[ -z "$deb" ]] && { echo "[$n/$total] NO DEB $pkg"; continue; }
  echo "[$n/$total] converting $pkg ($(du -h "$deb"|cut -f1))"
  "$OBESE" deb2ob "$deb" --no-deps -o "$OUT/$pkg.ob" >/dev/null 2>&1
  [[ -f "$OUT/$pkg.ob" ]] && echo "[$n/$total] DONE $pkg" || echo "[$n/$total] FAIL $pkg"
done < "$WORK/pkgs.txt"
echo "=== ALL DONE ==="
