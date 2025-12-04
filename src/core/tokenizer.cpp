#include "tokenizer.hpp"
#include <cctype>

std::vector<Token> tokenize_source(const std::string& source_code,
	std::unordered_map<std::string, std::vector<std::string>>& context_info)
{
	std::vector<Token> tokens;
	u32 line = 1;
	u32 column = 1;

	size_t i = 0;
	const size_t len = source_code.size();

	auto push_token = [&](Token::Type type, const std::string& text, u32 l, u32 c)
	{
		Token t{};
		t.type = type;
		t.text = text;
		t.line = l;
		t.column = c;
		tokens.push_back(t);
	};

	auto is_ident_start = [](char c)
	{
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
	};

	auto is_ident_char = [](char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	};

	while (i < len)
	{
		char c = source_code[i];
		if (c == ' ' || c == '\t' || c == '\r')
		{
			i++;
			column++;
			continue;
		}
		if (c == '\n')
		{
			i++;
			line++;
			column = 1;
			continue;
		}

		if (c == '$' && i + 1 < len && is_ident_start(source_code[i + 1]))
		{
			u32 tok_line = line;
			u32 tok_col = column;
			i++;
			column++;
			size_t start = i;
			while (i < len && is_ident_char(source_code[i]))
			{
				i++;
				column++;
			}
			std::string proc = source_code.substr(start, i - start);
			while (i < len && (source_code[i] == ' ' || source_code[i] == '\t'))
			{
				i++;
				column++;
			}

			if (i >= len)
			{
				push_token(Token::Type::Unknown, "$" + proc, tok_line, tok_col);
				continue;
			}

			auto matching = [](char open) -> char
			{
				switch (open)
				{
					case '(':
						return ')';
					case '[':
						return ']';
					case '{':
						return '}';
					case '<':
						return '>';
					default:
						return 0;
				}
			};

			char open_ch = source_code[i];
			char close_ch = matching(open_ch);
			if (!close_ch)
			{
				push_token(Token::Type::Unknown, "$" + proc, tok_line, tok_col);
				continue;
			}
			i++;
			column++;

			size_t content_start = i;
			int depth = 1;
			bool in_quote = false;
			char quote_char = 0;
			while (i < len)
			{
				char ch = source_code[i];
				if (in_quote)
				{
					if (ch == '\\' && i + 1 < len)
					{
						i += 2;
						column += 2;
						continue;
					}
					if (ch == quote_char)
						in_quote = false;
					if (ch == '\n')
					{
						line++;
						column = 1;
					}
					else
					{
						column++;
					}
					i++;
					continue;
				}
				if (ch == '"' || ch == '\'')
				{
					in_quote = true;
					quote_char = ch;
					i++;
					column++;
					continue;
				}
				if (ch == open_ch)
					depth++;
				else if (ch == close_ch)
					depth--;

				if (depth == 0)
					break;

				if (ch == '\n')
				{
					line++;
					column = 1;
				}
				else
				{
					column++;
				}
				i++;
			}

			size_t content_end = i;
			std::string content = source_code.substr(content_start, content_end - content_start);
			if (i < len && source_code[i] == close_ch)
			{
				i++;
				column++;
			}

			Token t{};
			t.type = Token::Type::Template;
			t.text = "$" + proc;
			t.template_content = content;
			t.line = tok_line;
			t.column = tok_col;
			tokens.push_back(t);
			continue;
		}

		if ((c == '#' && column == 1) || (c == '/' && i + 1 < len && source_code[i + 1] == '/'))
		{
			i += 2;
			column += 2;
			size_t start = i - (c == '#' ? 1 : 0);
			while (i < len && source_code[i] != '\n')
			{
				i++;
				column++;
			}
			context_info["comment_lines"].push_back(source_code.substr(start, i - start));
			continue;
		}
		if (c == '/' && i + 1 < len && source_code[i + 1] == '*')
		{
			i += 2;
			column += 2;
			size_t start = i;
			while (i + 1 < len && !(source_code[i] == '*' && source_code[i + 1] == '/'))
			{
				if (source_code[i] == '\n')
				{
					line++;
					column = 1;
				}
				else
				{
					column++;
				}
				i++;
			}
			size_t end = i;
			context_info["comment_lines"].push_back(source_code.substr(start, end - start));
			i += 2;
			column += 2;
			continue;
		}

		if (std::isdigit(static_cast<unsigned char>(c)))
		{
			u32 tok_line = line;
			u32 tok_col = column;
			size_t start = i;
			while (i < len && (std::isdigit(static_cast<unsigned char>(source_code[i])) || source_code[i] == '.'))
			{
				i++;
				column++;
			}
			push_token(Token::Type::Number, source_code.substr(start, i - start), tok_line, tok_col);
			continue;
		}

		if (is_ident_start(c))
		{
			u32 tok_line = line;
			u32 tok_col = column;
			size_t start = i;
			while (i < len && is_ident_char(source_code[i]))
			{
				i++;
				column++;
			}
			std::string ident = source_code.substr(start, i - start);
			std::string lower = ident;
			for (auto& ch : lower)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if (lower == "function" || lower == "return" || lower == "var" || lower == "true" || lower == "false" || lower == "none" || lower == "if" || lower == "else" || lower == "while" || lower == "for" || lower == "foreach" || lower == "in" || lower == "break" || lower == "continue" || lower == "switch" || lower == "case" || lower == "default")
			{
				push_token(Token::Type::Keyword, lower, tok_line, tok_col);
			}
			else
			{
				push_token(Token::Type::Identifier, ident, tok_line, tok_col);
			}
			continue;
		}

		if (c == '"' || c == '\'')
		{
			u32 tok_line = line;
			u32 tok_col = column;
			char quote = c;
			i++;
			column++;
			std::string literal;
			while (i < len && source_code[i] != quote)
			{
				if (source_code[i] == '\\' && i + 1 < len)
				{
					char esc = source_code[i + 1];
					switch (esc)
					{
						case 'n':
							literal.push_back('\n');
							break;
						case 'r':
							literal.push_back('\r');
							break;
						case 't':
							literal.push_back('\t');
							break;
						case '0':
							literal.push_back('\0');
							break;
						case 'b':
							literal.push_back('\b');
							break;
						case 'f':
							literal.push_back('\f');
							break;
						case '\\':
							literal.push_back('\\');
							break;
						case '"':
							literal.push_back('"');
							break;
						case '\'':
							literal.push_back('\'');
							break;
						default:
							literal.push_back(esc);
							break;
					}
					i += 2;
					column += 2;
				}
				else if (source_code[i] == '\n')
				{
					literal.push_back('\n');
					i++;
					line++;
					column = 1;
				}
				else
				{
					literal.push_back(source_code[i]);
					i++;
					column++;
				}
			}
			if (i < len && source_code[i] == quote)
			{
				i++;
				column++;
			}
			push_token(Token::Type::String, literal, tok_line, tok_col);
			continue;
		}

		u32 tok_line = line;
		u32 tok_col = column;
		std::string sym(1, c);
		if (i + 1 < len)
		{
			char n = source_code[i + 1];
			if (c == '.' && i + 2 < len && source_code[i + 1] == '.' && source_code[i + 2] == '.')
			{
				sym = "...";
				i += 3;
				column += 3;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if (c == '.' && n == '.')
			{
				sym = "..";
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if ((c == '=' || c == '!' || c == '<' || c == '>') && n == '=')
			{
				sym = std::string() + c + "=";
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if (c == '-' && n == '>')
			{
				sym = "->";
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if ((c == '&' && n == '&') || (c == '|' && n == '|') || (c == '+' && n == '+') || (c == '-' && n == '-') || (c == '+' && n == '=') || (c == '-' && n == '=') || (c == '*' && n == '=') || (c == '/' && n == '='))
			{
				sym = std::string() + c + n;
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
		}
		push_token(Token::Type::Symbol, sym, tok_line, tok_col);
		i++;
		column++;
	}

	Token eof{};
	eof.type = Token::Type::EndOfFile;
	eof.line = line;
	eof.column = column;
	tokens.push_back(eof);
	return tokens;
}
