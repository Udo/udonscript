package udon;

import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.psi.FileViewProvider;
import org.jetbrains.annotations.NotNull;

import javax.swing.*;

public class UdonPsiFile extends PsiFileBase {
	public UdonPsiFile(@NotNull FileViewProvider viewProvider) {
		super(viewProvider, UdonLanguage.INSTANCE);
	}

	@Override public @NotNull FileType getFileType() { return UdonFileType.INSTANCE; }
	@Override public @NotNull String toString() { return "UdonScript File"; }
	@Override public Icon getIcon(int flags) { return super.getIcon(flags); }
}
