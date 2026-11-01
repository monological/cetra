" The indent and format rules the vim mode's layout oracle holds vim to (spec 14.9), sourced as
" 'indentexpr' and 'formatexpr'. gametest's vim-run probe installs the same two rules in C under
" CETRA_VIM_LAYOUT, as a host's indent and format hooks; change one and change the other.

" A line takes the indent of the line above it that is not blank, two columns more for every ( that
" line opens and two fewer for every ) it closes, never below 0; a line opening with ; keeps its
" own. It reads the line above as re-indented, so = must ask a line only after those above it.
function! CetraIndent()
  if getline(v:lnum) =~ '^\s*;'
    return -1
  endif
  let prev = prevnonblank(v:lnum - 1)
  if prev == 0
    return 0
  endif
  let line = getline(prev)
  return max([0, indent(prev) + 2 * (count(line, '(') - count(line, ')'))])
endfunction

" gq: the lines' words, split at blanks, laid out again at the first line's indent, as many to a
" line as fit in 24 bytes with the indent, the cursor on the last line's first character that is
" not a blank. No words leave one empty line. A text that may not change is left as it is: the
" vim mode refuses gq there without asking.
function! CetraFormat()
  if !&modifiable
    return 0
  endif
  let first = v:lnum
  let last = v:lnum + v:count - 1
  let ind = indent(first)
  let lines = []
  let cur = ''
  for word in split(join(getline(first, last), ' '))
    if cur == ''
      let cur = word
    elseif ind + strlen(cur) + 1 + strlen(word) <= 24
      let cur .= ' ' . word
    else
      call add(lines, cur)
      let cur = word
    endif
  endfor
  call add(lines, cur)
  call map(lines, {_, l -> l == '' ? '' : repeat(' ', ind) . l})
  let old = last - first + 1
  if len(lines) > old
    call append(last, repeat([''], len(lines) - old))
  endif
  call setline(first, lines)
  if len(lines) < old
    silent execute (first + len(lines)) . ',' . last . 'delete _'
  endif
  let at = first + len(lines) - 1
  let text = getline(at)
  let col = match(text, '\S')
  call cursor(at, col >= 0 ? col + 1 : max([1, strlen(text)]))
  return 0
endfunction

set indentexpr=CetraIndent() indentkeys=o,O formatexpr=CetraFormat()
