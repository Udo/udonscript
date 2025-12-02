package udon;

import com.intellij.lexer.FlexLexer;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import static com.intellij.psi.TokenType.BAD_CHARACTER;
import static udon.psi.UdonTypes.*;

%%

%public
%class _UdonLexer
%implements FlexLexer
%unicode
%function advance
%type IElementType
%eof{ return null; %}

WHITESPACE = [ \t\r\n]+
COMMENT = \/\/[^\r\n]* | \/\*[^*]*\*+([^/*][^*]*\*+)*\/
IDENT = [A-Za-z_][A-Za-z0-9_]*
NUMBER = [0-9]+(\.[0-9]+)?
STRING = \"([^\"\\n\\r]|\\.)*\"|\'([^\'\\n\\r]|\\.)*\'

%%

{WHITESPACE}   { return TokenType.WHITE_SPACE; }
{COMMENT}      { return COMMENT; }
{STRING}       { return STRING; }
{NUMBER}       { return NUMBER; }
{IDENT}        { return IDENT; }
\$[A-Za-z_][A-Za-z0-9_]* { return TEMPLATE; }
.             { return BAD_CHARACTER; }
