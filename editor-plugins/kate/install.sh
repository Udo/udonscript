#!/usr/bin/env bash
set -euo pipefail

TARGET="${HOME}/.local/share/org.kde.syntax-highlighting/syntax"
mkdir -p "$TARGET"
cp "$(dirname "$0")/udonscript.xml" "$TARGET/"

echo "Installed UdonScript syntax to $TARGET"
