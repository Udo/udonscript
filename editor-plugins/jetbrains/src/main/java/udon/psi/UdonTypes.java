package udon.psi;

import com.intellij.psi.tree.IElementType;
import udon.UdonLanguage;

public interface UdonTypes {
	IElementType COMMENT = new UdonTokenType("COMMENT");
	IElementType STRING = new UdonTokenType("STRING");
	IElementType NUMBER = new UdonTokenType("NUMBER");
	IElementType IDENT = new UdonTokenType("IDENT");
	IElementType TEMPLATE = new UdonTokenType("TEMPLATE");
}
