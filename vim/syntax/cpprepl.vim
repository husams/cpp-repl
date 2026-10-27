" Vim syntax file
" Language: cpp-repl script (C++ plus %commands, see src/commands.hpp)
" Filenames: *.repl

if exists("b:current_syntax")
  finish
endif

" Include each language in its own cluster. In particular, C++ contains=TOP
" and ALLBUT rules must not pick up REPL commands or shell syntax.
syn include @cppReplCpp syntax/cpp.vim
unlet! b:current_syntax

" Double-backtick blocks run through /bin/sh -c.
syn include @cppReplShell syntax/sh.vim
unlet! b:current_syntax

" Start a C++ context on every ordinary line; strings, comments and braces
" can continue it over subsequent lines. Command lines take priority below.
syn region cppReplCppLine transparent start="^" end="$" contains=@cppReplCpp

" A line whose first non-blank character is '%'. Shell and cpp` blocks may
" continue it over several lines.
syn region cppReplCommandLine keepend start="^\s*\ze%" end="$"
      \ contains=cppReplUnknownCommand,cppReplCommand,cppReplKeyword,cppReplValue,cppReplString,
      \ cppReplShellBlock,cppReplCppBlock,cType,cppType,cStorageClass,cppStorageClass,
      \ cppModifier,cppExceptions

syn match cppReplUnknownCommand contained "^\s*\zs%[-A-Za-z_0-9]*"
syn match cppReplCommand contained
      \ "^\s*\zs%\%(lib\|libs\|libinfo\|paths\|add-path\|add\|reload\|compile-opts\|add-compile-opt\|set-compile-opts\|use-compile-opts\|class\|methods\|fields\|info\|type\|demangle\|define\|symbols\|mock\|pre\|post\|restore\|hooks\|clear\|help\|quit\)\ze\%(\s\|$\)"

" Bang commands contain a single shell line, with literal shell quoting.
syn region cppReplShellLine keepend start="^\s*\ze!" end="$"
      \ contains=@cppReplShell,cppReplCommand
syn match cppReplCommand contained "^\s*\zs!"

" Argument words; standalone only, so paths such as lib/foo.so stay plain.
syn match cppReplKeyword contained
      \ "\%(^\|\s\)\zs\%(in\|all\|undefined\|lib\|name\|access\|include-parents\)\ze\%(\s\|$\)"
syn match cppReplValue contained
      \ "\%(^\|\s\)\zs\%(public\|private\|protected\|true\|false\)\ze\%(\s\|$\)"

syn region cppReplString contained oneline start=+"+ skip=+\\.+ end=+"+ contains=cppReplShellBlock
syn region cppReplString contained oneline start=+'+ end=+'+ contains=cppReplShellBlock

" The parser ends shell substitutions at the next pair, even inside quotes.
syn region cppReplShellBlock contained keepend extend matchgroup=cppReplShellDelim start="``" end="``"
      \ contains=@cppReplShell

" cpp`...` bodies of %define, %mock, %pre and %post. Backticks inside C++
" strings and comments do not close the block.
syn region cppReplCppBlock contained extend matchgroup=cppReplCppDelim
      \ start="\%(^\|\s\)\zscpp`" end="`" contains=@cppReplCpp

" Dedicated defaults keep commands, argument keywords and literal values
" distinct even in themes that give Statement and Keyword the same colour.
" Users can override these groups in their colourscheme or vimrc.
if &background ==# 'light'
  hi def cppReplCommand    guifg=#006c82 ctermfg=23 gui=bold cterm=bold
  hi def cppReplKeyword    guifg=#7c3bbd ctermfg=91 gui=bold cterm=bold
  hi def cppReplValue      guifg=#9a4600 ctermfg=130
  hi def cppReplString     guifg=#2f6f20 ctermfg=28
  hi def cppReplShellDelim guifg=#795c00 ctermfg=94 gui=bold cterm=bold
  hi def cppReplCppDelim   guifg=#2455a4 ctermfg=25 gui=bold cterm=bold
else
  hi def cppReplCommand    guifg=#89dceb ctermfg=117 gui=bold cterm=bold
  hi def cppReplKeyword    guifg=#cba6f7 ctermfg=183 gui=bold cterm=bold
  hi def cppReplValue      guifg=#fab387 ctermfg=215
  hi def cppReplString     guifg=#a6e3a1 ctermfg=150
  hi def cppReplShellDelim guifg=#f9e2af ctermfg=223 gui=bold cterm=bold
  hi def cppReplCppDelim   guifg=#89b4fa ctermfg=111 gui=bold cterm=bold
endif
hi def link cppReplUnknownCommand Error

" Opening a long block in the middle must retain the enclosing language.
syn sync fromstart

let b:current_syntax = "cpprepl"
