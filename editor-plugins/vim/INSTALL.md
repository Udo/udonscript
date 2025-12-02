# UdonScript Vim/Neovim Syntax

## Manual install
Copy the contents of this folder into your Vim runtime:

```bash
mkdir -p ~/.vim/ftdetect ~/.vim/syntax
cp ftdetect/udon.vim ~/.vim/ftdetect/
cp syntax/udon.vim ~/.vim/syntax/
```

For Neovim, use `~/.config/nvim` instead of `~/.vim`:

```bash
mkdir -p ~/.config/nvim/ftdetect ~/.config/nvim/syntax
cp ftdetect/udon.vim ~/.config/nvim/ftdetect/
cp syntax/udon.vim ~/.config/nvim/syntax/
```

Restart Vim/Neovim and opening `*.udon` files should highlight automatically.
