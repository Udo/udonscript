package udon;

import com.intellij.lexer.FlexAdapter;

public class UdonLexerAdapter extends FlexAdapter {
	public UdonLexerAdapter() {
		super(new _UdonLexer());
	}
}
