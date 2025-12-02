package udon;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.lang.ASTNode;
import org.jetbrains.annotations.NotNull;

public class UdonPsiElement extends ASTWrapperPsiElement {
	public UdonPsiElement(@NotNull ASTNode node) {
		super(node);
	}
}
