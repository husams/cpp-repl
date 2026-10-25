# Vim support for cpp-repl scripts

Filetype `cpprepl` for numbered startup scripts (`*.repl`): C++ highlighting
plus `%commands`, their argument keywords, quoted paths, double-backtick
shell blocks (highlighted as `sh`) and `cpp\`...\`` bodies (highlighted as
C++, multiline). Unknown `%commands` show as errors. The ftplugin reuses the
C++ settings (comments, indent, `commentstring`).

Install as a Vim package:

```sh
mkdir -p ~/.vim/pack/cpp-repl/start
cp -R vim ~/.vim/pack/cpp-repl/start/cpp-repl
```

Neovim: copy to `~/.local/share/nvim/site/pack/cpp-repl/start/cpp-repl`.
