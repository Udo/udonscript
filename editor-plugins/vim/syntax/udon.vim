" Vim syntax file for UdonScript

if exists("b:current_syntax")
  finish
endif

syn keyword udonKeyword function return var if else while for foreach in break continue switch case default on
syn keyword udonBoolean true false
syn keyword udonType vec2 vec3 vec4 int float Int Float s32 f32 string String bool Bool array Array any Any

syn match udonNumber /\v\<\d+(\.\d+)?\>/

" Strings (allow escapes and multiline)
syn region udonString start=/"/ skip=/\\\\./ end=/"/ contains=udonEscape
syn region udonString start=/'/ skip=/\\\\./ end=/'/ contains=udonEscape
syn match udonEscape /\\[nrt0bff"'\\]/

" Comments
syn match udonLineComment "//.*$"
syn region udonBlockComment start=/\/\*/ end=/\*\//

" Template processors: $name(...)
syn match udonTemplate /^\s*\$[A-Za-z_][A-Za-z0-9_]*\ze(/
syn region udonTemplateBody start=/\$/ end=/)/ contains=udonString,udonLineComment,udonBlockComment,@Spell

hi def link udonKeyword Keyword
hi def link udonBoolean Boolean
hi def link udonType Type
hi def link udonNumber Number
hi def link udonString String
hi def link udonEscape SpecialChar
hi def link udonLineComment Comment
hi def link udonBlockComment Comment
hi def link udonTemplate Identifier
hi def link udonTemplateBody Special

let b:current_syntax = "udon"
