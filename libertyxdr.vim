" filetype.vim: au! BufNewFile,BufRead *.lxdr setf libertyxdr
if exists("b:current_syntax")
	finish
endif

syn match libertyxdrError "[^[:space:]:;,(){}<>=]\+"
syn region libertyxdrBlockComment start=+/[*]+ end=+[*]/+
syn match libertyxdrComment "//.*"
syn match libertyxdrIdentifier "\<[[:alpha:]][[:alnum:]_]*\>"
syn match libertyxdrNumber "\<0\>\|\(-\|\<\)[1-9][[:digit:]]*\>"
syn keyword libertyxdrKeyword const enum struct union switch case
syn keyword libertyxdrType bool u8 u16 u32 u64 i8 i16 i32 i64 string void

let b:current_syntax = "libertyxdr"
hi def link libertyxdrError Error
hi def link libertyxdrBlockComment Comment
hi def link libertyxdrComment Comment
hi def link libertyxdrIdentifier Identifier
hi def link libertyxdrNumber Number
hi def link libertyxdrKeyword Statement
hi def link libertyxdrType Type
