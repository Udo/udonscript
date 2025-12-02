package udon;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import udon.psi.UdonTypes;

import static com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey;

public class UdonSyntaxHighlighter extends SyntaxHighlighterBase {
	public static final TextAttributesKey COMMENT = createTextAttributesKey("UDON_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT);
	public static final TextAttributesKey STRING = createTextAttributesKey("UDON_STRING", DefaultLanguageHighlighterColors.STRING);
	public static final TextAttributesKey NUMBER = createTextAttributesKey("UDON_NUMBER", DefaultLanguageHighlighterColors.NUMBER);
	public static final TextAttributesKey IDENT = createTextAttributesKey("UDON_IDENT", DefaultLanguageHighlighterColors.IDENTIFIER);
	public static final TextAttributesKey TEMPLATE = createTextAttributesKey("UDON_TEMPLATE", DefaultLanguageHighlighterColors.KEYWORD);

	@Override public @NotNull Lexer getHighlightingLexer() { return new UdonLexerAdapter(); }

	@Override
	public TextAttributesKey @NotNull [] getTokenHighlights(IElementType tokenType) {
		if (tokenType == UdonTypes.COMMENT) return pack(COMMENT);
		if (tokenType == UdonTypes.STRING) return pack(STRING);
		if (tokenType == UdonTypes.NUMBER) return pack(NUMBER);
		if (tokenType == UdonTypes.TEMPLATE) return pack(TEMPLATE);
		return pack(IDENT);
	}
}
