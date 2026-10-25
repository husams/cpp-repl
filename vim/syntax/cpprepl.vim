" Vim syntax file
" Language: cpp-repl script (C++ plus %commands, see src/commands.hpp)
" Filenames: *.repl

if exists("b:current_syntax")
  finish
endif

" Everything that is not a %command is C++.
runtime! syntax/cpp.vim
unlet! b:current_syntax

" Double-backtick blocks run through /bin/sh -c.
syn include @cppReplShell syntax/sh.vim
unlet! b:current_syntax

" A line whose first non-blank character is '%'. Shell and cpp` blocks may
" continue it over several lines.
syn region cppReplCommandLine start="^\s*\ze%" end="$"
      \ contains=cppReplUnknownCommand,cppReplCommand,cppReplKeyword,cppReplString,
      \ cppReplShellBlock,cppReplCppBlock,cType,cppType,cStorageClass,cppModifier,cNumber

syn match cppReplUnknownCommand contained "%[-A-Za-z_0-9]*"
syn match cppReplCommand contained
      \ "%\%(lib\|libs\|libinfo\|paths\|add-path\|add\|reload\|compile-opts\|add-compile-opt\|set-compile-opts\|use-compile-opts\|class\|methods\|fields\|info\|syminfo\|demangle\|define\|symbols\|mock\|pre\|post\|restore\|hooks\|clear\|help\|quit\)\ze\%(\s\|$\)"

" Argument words; standalone only, so paths such as lib/foo.so stay plain.
syn match cppReplKeyword contained
      \ "\%(^\|\s\)\zs\%(in\|all\|undefined\|lib\|name\|access\|include-parents\|public\|private\|protected\|true\|false\)\ze\%(\s\|$\)"

syn region cppReplString contained start=+"+ skip=+\\.+ end=+"+ contains=cppReplShellBlock
syn region cppReplString contained start=+'+ end=+'+

syn region cppReplShellBlock contained matchgroup=cppReplShellDelim start="``" end="``"
      \ contains=@cppReplShell

" cpp`...` bodies of %define, %mock, %pre and %post. Backticks inside C++
" strings and comments do not close the block.
syn region cppReplCppBlock contained matchgroup=cppReplCppDelim
      \ start="\%(^\|\s\)\zscpp`" end="`" contains=TOP,cppReplCommandLine

hi def link cppReplCommand        Statement
hi def link cppReplUnknownCommand Error
hi def link cppReplKeyword        Keyword
hi def link cppReplString         String
hi def link cppReplShellDelim     PreProc
hi def link cppReplCppDelim       Special

let b:current_syntax = "cpprepl"
