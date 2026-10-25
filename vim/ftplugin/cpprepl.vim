" cpp-repl scripts edit like C++.
if exists("b:did_ftplugin")
  finish
endif
runtime! ftplugin/cpp.vim ftplugin/cpp_*.vim ftplugin/cpp/*.vim
let b:did_ftplugin = 1
