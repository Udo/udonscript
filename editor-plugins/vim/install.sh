#!/usr/bin/env bash
set -euo pipefail

TARGET="${XDG_CONFIG_HOME:-$HOME/.vim}"
if [ -d "${XDG_CONFIG_HOME:-}" ]; then
	TARGET="$HOME/.config/nvim"
fi

mkdir -p "$TARGET/ftdetect" "$TARGET/syntax"
cp "$(dirname "$0")/ftdetect/udon.vim" "$TARGET/ftdetect/"
cp "$(dirname "$0")/syntax/udon.vim" "$TARGET/syntax/"

echo "Installed UdonScript syntax to $TARGET"
