" Since the liberty configuration format is nearly indistinguishable,
" this syntax highlight definition needs to be loaded with `set ft=libertyconf`
if exists("b:current_syntax")
	finish
endif

syn match libertyconfError "[^_[:alnum:][:space:]]\+"
syn match libertyconfComment "#.*"
syn match libertyconfSpecial "{\|}\|="
syn match libertyconfNumber "[+-]\=\<\d\+\>"
syn match libertyconfBoolean "\c\<\(true\|yes\|on\|false\|no\|off\)\>"
syn match libertyconfNull "null"
syn match libertyconfEscape display "\\\([xX]\x\{1,2}\|\o\{1,3}\|.\|$\)"
	\ contained
syn region libertyconfString start=+"+ skip=+\\\\\|\\"+ end=+"+
	\ contains=libertyconfEscape

let b:current_syntax = "libertyconf"
hi def link libertyconfError Error
hi def link libertyconfComment Comment
hi def link libertyconfSpecial Special
hi def link libertyconfNumber Number
hi def link libertyconfBoolean Boolean
hi def link libertyconfNull Constant
hi def link libertyconfEscape SpecialChar
hi def link libertyconfString String
