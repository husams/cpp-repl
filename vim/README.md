# Vim support for cpp-repl scripts

Filetype `cpprepl` for numbered startup scripts (`*.repl`): C++ highlighting
plus `%commands`, their argument keywords, quoted paths, double-backtick
shell blocks (highlighted as `sh`) and ``cpp`...` `` bodies (highlighted as
C++, multiline). Unknown `%commands` show as errors. The ftplugin reuses the
C++ settings (comments, indent, `commentstring`).

Commands are bold cyan, argument keywords are bold purple, literal values
(`true`, `false`, access levels) are orange, and quoted arguments are green.
Shell delimiters are yellow and C++ delimiters are blue. Defaults include
dark and light backgrounds, true colour and 256-colour terminals. Embedded
code uses Vim's native C++ and shell syntax and your theme's language colours;
strings and comments in C++ can contain backticks without ending the block.

Override the `cppReplCommand`, `cppReplKeyword`, `cppReplValue`,
`cppReplString`, `cppReplShellDelim` or `cppReplCppDelim` highlight groups
in your vimrc or colourscheme to customise the palette, for example:

```vim
highlight cppReplCommand guifg=#00bcd4 ctermfg=44 gui=bold cterm=bold
```

Install as a Vim package:

```sh
mkdir -p ~/.vim/pack/cpp-repl/start
cp -R vim ~/.vim/pack/cpp-repl/start/cpp-repl
```

Neovim: copy to `~/.local/share/nvim/site/pack/cpp-repl/start/cpp-repl`.

Run the highlighting regression checks from the repository root (no C++
build required):

```sh
vim -Nu NONE -n -es -S tests/vim_syntax_test.vim
```
