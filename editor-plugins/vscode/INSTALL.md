# UdonScript VS Code Extension

## Manual install
Open this folder in VS Code and run:

1. Command Palette → “Developer: Install Extension from Location…”
2. Choose this `editor-plugins/vscode` folder.

Alternatively, if you have `vsce` installed:

```bash
cd editor-plugins/vscode
vsce package
code --install-extension udonscript-*.vsix
```
