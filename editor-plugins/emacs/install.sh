#!/usr/bin/env bash
set -euo pipefail

TARGET="${HOME}/.emacs.d/lisp"
mkdir -p "$TARGET"
cp "$(dirname "$0")/udon-mode.el" "$TARGET/"

if ! grep -q "udon-mode" "${HOME}/.emacs" 2>/dev/null; then
cat >> "${HOME}/.emacs" <<'EOF'
(add-to-list 'load-path "~/.emacs.d/lisp/")
(require 'udon-mode)
EOF
fi

echo "Installed udon-mode.el to $TARGET and ensured it is loaded from ~/.emacs"
