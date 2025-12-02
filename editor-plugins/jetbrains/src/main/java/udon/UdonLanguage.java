package udon;

import com.intellij.lang.Language;

public class UdonLanguage extends Language {
	public static final UdonLanguage INSTANCE = new UdonLanguage();
	private UdonLanguage() {
		super("UdonScript");
	}
}
