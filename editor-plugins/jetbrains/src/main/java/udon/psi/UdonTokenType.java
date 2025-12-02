package udon.psi;

import com.intellij.psi.tree.IElementType;
import udon.UdonLanguage;

public class UdonTokenType extends IElementType {
	public UdonTokenType(String debugName) {
		super(debugName, UdonLanguage.INSTANCE);
	}

	@Override
	public String toString() {
		return "UdonTokenType." + super.toString();
	}
}
