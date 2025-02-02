" Vim syntax file
" Language: NESFab
" Maintainer: Pubby

if exists("b:current_syntax")
  finish
endif

syn keyword nesfabKeyword if else for while do break continue return fn 
    \ ct nmi mode goto label file struct vars data omni ready fence 
    \ default switch case asm charmap chrrom true false audio system stows employs preserves state

syntax match nesfabId "\l\k*"
syntax match nesfabType "\u\k*"

syntax match nesfabGroup "/\k\+"

" Integer with - + or nothing in front
syn match nesfabNumber '\d\+'
syn match nesfabNumberHex '\$\x\+'
syn match nesfabNumberBin '%[01]\+'

" Comment
syn match nesfabCommentL "//.*$"
syn match nesfabComment "/\*\_.\{-}\*/"

" String
syn region nesfabString start='"' end='"' contained

let b:current_syntax = "nesfab"

hi def link nesfabNumber     Constant
hi def link nesfabNumberHex  Constant
hi def link nesfabNumberBin  Constant
hi def link nesfabCommentL   Comment
hi def link nesfabComment    Comment
hi def link nesfabString     String
hi def link nesfabKeyword    Statement
hi def link nesfabGroup      Identifier
hi def link nesfabType       Type
