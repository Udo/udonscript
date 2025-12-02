#!/usr/bin/env bash
set -euo pipefail

TARGET="${HOME}/.config/geany/filedefs"
mkdir -p "$TARGET"
cp "$(dirname "$0")/filetypes.UdonScript" "$TARGET/"

echo "Installed UdonScript filetype to $TARGET"
