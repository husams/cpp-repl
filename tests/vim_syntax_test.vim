" Run from any directory: vim -Nu NONE -n -es -S tests/vim_syntax_test.vim
let s:root = fnamemodify(expand('<sfile>:p'), ':h:h')
execute 'set runtimepath^=' . fnameescape(s:root . '/vim')
filetype plugin on
syntax enable

function! s:Load(lines)
  enew!
  call setline(1, a:lines)
  setfiletype cpprepl
endfunction

function! s:Group(line, text)
  let col = match(getline(a:line), '\V' . escape(a:text, '\')) + 1
  call assert_true(col > 0, 'Missing fixture text: ' . a:text)
  return synIDattr(synID(a:line, col, 1), 'name')
endfunction

function! s:Check(line, text, group)
  call assert_equal(a:group, s:Group(a:line, a:text),
        \ printf('line %d: %s', a:line, a:text))
endfunction

" Automatic filetype detection and distinct colours for command vocabulary.
enew!
file syntax-example.repl
doautocmd BufRead syntax-example.repl
call assert_equal('cpprepl', &filetype)
call assert_equal('cpprepl', b:current_syntax)
call s:Load(['  %fields Thing access private include-parents true',
      \ '%symbols all undefined lib "lib/demo.so" name ''public''',
      \ '%missing argument', '%syminfo name', '%add lib/public.cpp %help'])
call s:Check(1, '%fields', 'cppReplCommand')
call s:Check(1, 'access', 'cppReplKeyword')
call s:Check(1, 'private', 'cppReplValue')
call s:Check(1, 'include-parents', 'cppReplKeyword')
call s:Check(1, 'true', 'cppReplValue')
call s:Check(2, 'all', 'cppReplKeyword')
call s:Check(2, 'undefined', 'cppReplKeyword')
call s:Check(2, 'lib/demo.so', 'cppReplString')
call s:Check(2, 'public', 'cppReplString')
call s:Check(3, '%missing', 'cppReplUnknownCommand')
call s:Check(4, '%syminfo', 'cppReplUnknownCommand')
call assert_notequal('cppReplKeyword', s:Group(5, 'lib/public.cpp'))
call assert_notequal('cppReplCommand', s:Group(5, '%help'))
for s:mode in ['gui', 'cterm']
  let s:colours = map(['cppReplCommand', 'cppReplKeyword', 'cppReplValue', 'cppReplString'],
        \ 'synIDattr(synIDtrans(hlID(v:val)), "fg", s:mode)')
  call assert_equal(4, len(uniq(sort(copy(s:colours)))), s:mode . ' palette')
endfor

" Ordinary C++ retains the native lexer, including modern literals.
call s:Load(['constexpr int answer = 42;',
      \ 'if (true) { return 3.5; }',
      \ 'auto text = R"tag(%help ``shell`` `)tag";',
      \ '// %quit is a comment',
      \ 'int remainder = answer % 2;', '%help',
      \ 'int f() {', '  return 42;', '}', '%quit'])
call s:Check(1, 'constexpr', 'cppStorageClass')
call s:Check(1, 'int', 'cType')
call s:Check(1, '42', 'cppNumber')
call s:Check(2, 'if', 'cConditional')
call s:Check(2, 'true', 'cppBoolean')
call s:Check(2, 'return', 'cStatement')
call s:Check(2, '3.5', 'cppFloat')
call s:Check(3, '%help', 'cppRawString')
call s:Check(4, '%quit', 'cCommentL')
call assert_notequal('cppReplUnknownCommand', s:Group(5, '%'))
call s:Check(6, '%help', 'cppReplCommand')
call s:Check(8, 'return', 'cStatement')
call s:Check(8, '42', 'cppNumber')
call s:Check(10, '%quit', 'cppReplCommand')

" cpp` bodies use C++, including backticks in strings and comments.
call s:Load(['%mock int f(int x) const noexcept cpp`',
      \ '  const char* text = "a ` and ``shell``";',
      \ '  auto raw = R"tag(` %quit)tag";',
      \ '  /* `', '%help', '  */',
      \ '  // `',
      \ '  if (true) { return x % 2 + 42; }',
      \ '`', '%help',
      \ '%define value cpp`3.5`',
      \ '%pre f(int x) cpp``', '%quit', 'auto value = false;'])
call s:Check(1, 'noexcept', 'cppExceptions')
call s:Check(1, 'cpp`', 'cppReplCppDelim')
call assert_match('^c\%(Type\|StorageClass\)', s:Group(2, 'const'))
call s:Check(2, 'a `', 'cppString')
call s:Check(3, '%quit', 'cppRawString')
call s:Check(5, '%help', 'cComment')
call s:Check(7, '//', 'cCommentL')
call s:Check(8, 'if', 'cConditional')
call s:Check(8, 'true', 'cppBoolean')
call s:Check(8, '42', 'cppNumber')
call s:Check(9, '`', 'cppReplCppDelim')
call s:Check(10, '%help', 'cppReplCommand')
call s:Check(11, '3.5', 'cppFloat')
call s:Check(12, 'cpp`', 'cppReplCppDelim')
call s:Check(13, '%quit', 'cppReplCommand')
call s:Check(14, 'auto', 'cppType')
call s:Check(14, 'false', 'cppBoolean')

" Multiline shell, quoted substitutions, and a return to REPL/C++ syntax.
call s:Load(['%add ``',
      \ 'if [ -n "$HOME" ]; then',
      \ '  printf ''%s\n'' "$HOME/file.cpp" # comment',
      \ 'fi', '``', '%help', 'int answer = 42;',
      \ '%add "``printf ''%s'' "$HOME/file.cpp"``"',
      \ '%add ''``printf "$HOME/file.cpp"``''',
      \ '%add "``', 'printf "$HOME/file.cpp"', '``"', '%quit'])
call s:Check(1, '``', 'cppReplShellDelim')
call assert_match('^sh', s:Group(2, 'if'))
call assert_match('^sh', s:Group(2, '$HOME'))
call assert_match('^sh', s:Group(3, 'printf'))
call s:Check(3, '# comment', 'shComment')
call assert_match('^sh', s:Group(4, 'fi'))
call s:Check(5, '``', 'cppReplShellDelim')
call s:Check(6, '%help', 'cppReplCommand')
call s:Check(7, 'int', 'cType')
call s:Check(7, '42', 'cppNumber')
call assert_match('^sh', s:Group(8, 'printf'))
call assert_match('^sh', s:Group(9, '$HOME'))
call assert_match('^sh', s:Group(11, 'printf'))
call s:Check(12, '``', 'cppReplShellDelim')
call s:Check(13, '%quit', 'cppReplCommand')

" Shell delimiters are authoritative even inside an unfinished shell string.
call s:Load(['%add ``printf "unfinished``', '%help'])
call s:Check(2, '%help', 'cppReplCommand')

" An open block keeps its language while editing, including after blank lines.
call s:Load(['%mock f() cpp`', '', 'return false;'])
call s:Check(3, 'return', 'cStatement')
call s:Check(3, 'false', 'cppBoolean')
call s:Load(['%add ``', '', 'if true; then'])
call assert_match('^sh', s:Group(3, 'if'))

" Query deep inside a block first, as when reopening a file and jumping down.
call s:Load(['%mock f() cpp`'] + repeat(['// body'], 600) + ['return 42;', '`', '%help'])
call s:Check(602, 'return', 'cStatement')
call s:Check(604, '%help', 'cppReplCommand')

if !empty(v:errors)
  for s:error in v:errors
    echoerr s:error
  endfor
  cquit
endif
qa!
