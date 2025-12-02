#!/usr/bin/env bash
set -euo pipefail

TARGET="${HOME}/.config/sublime-text/Packages/UdonScript"
mkdir -p "$TARGET"
cp "$(dirname "$0")/UdonScript.sublime-syntax" "$TARGET/"

echo "Installed UdonScript syntax to $TARGET"
