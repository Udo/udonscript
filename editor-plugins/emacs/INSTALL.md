# UdonScript Emacs Mode

## Manual install
1. Copy `udon-mode.el` somewhere on your `load-path` (e.g., `~/.emacs.d/lisp/`).
2. Add to your init file:
   ```elisp
   (add-to-list 'load-path "~/.emacs.d/lisp/")
   (require 'udon-mode)
   ```

## Quick install script
Run `install.sh` to copy the mode into `~/.emacs.d/lisp/` and load it:

```bash
cd "$(dirname "$0")"
./install.sh
```
