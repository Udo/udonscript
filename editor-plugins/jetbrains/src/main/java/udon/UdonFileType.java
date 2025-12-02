package udon;

import com.intellij.openapi.fileTypes.LanguageFileType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.*;

public class UdonFileType extends LanguageFileType {
	public static final UdonFileType INSTANCE = new UdonFileType();
	private UdonFileType() {
		super(UdonLanguage.INSTANCE);
	}

	@Override public @NotNull String getName() { return "UdonScript"; }
	@Override public @NotNull String getDescription() { return "UdonScript file"; }
	@Override public @NotNull String getDefaultExtension() { return "udon"; }
	@Override public @Nullable Icon getIcon() { return null; }
}
