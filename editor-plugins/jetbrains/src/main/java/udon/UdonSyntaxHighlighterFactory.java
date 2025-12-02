package udon;

import com.intellij.openapi.fileTypes.SingleLazyInstanceSyntaxHighlighterFactory;
import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import org.jetbrains.annotations.NotNull;

public class UdonSyntaxHighlighterFactory extends SingleLazyInstanceSyntaxHighlighterFactory {
	@Override
	protected @NotNull SyntaxHighlighter createHighlighter() {
		return new UdonSyntaxHighlighter();
	}
}
