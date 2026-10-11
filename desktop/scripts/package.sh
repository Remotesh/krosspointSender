#!/usr/bin/env bash
# Copy the exe and the mingw64 runtime DLLs it needs into desktop/dist/.
# Run from Git Bash or MSYS2 with mingw64 on PATH.
set -e
cd "$(dirname "$0")/.."
BUILD=${1:-build}
OUT=dist
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$BUILD/krosspoint_sender.exe" "$OUT/"
# Resolve DLLs recursively with ntldd (mingw-w64-x86_64-ntldd) or fall back to objdump.
if command -v ntldd >/dev/null; then
  ntldd -R "$OUT/krosspoint_sender.exe" | grep -io "[a-z0-9_.+-]*\.dll => [^ ]*mingw64[^ ]*" | sed 's/.*=> //' | sort -u | while read -r d; do cp "$d" "$OUT/"; done
else
  todo=("$OUT/krosspoint_sender.exe"); seen=""
  while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    for d in $(objdump -p "$f" | awk '/DLL Name/{print $3}'); do
      p="/mingw64/bin/$d"; [ -f "$p" ] || p="/c/msys64/mingw64/bin/$d"
      if [ -f "$p" ] && [[ "$seen" != *"|$d|"* ]]; then seen="$seen|$d|"; cp "$p" "$OUT/"; todo+=("$p"); fi
    done
  done
fi
cp README.md "$OUT/README.md"
ls -la "$OUT" | awk '{print $5, $9}'
