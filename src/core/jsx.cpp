#include "jsx.hpp"
#include "helpers.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

struct JsxAttribute
{
	enum class Kind
	{
		Static,
		Expression,
		Spread,
		Boolean
	};

	Kind kind = Kind::Static;
	std::string name;
	std::string value;
};

struct JsxNode
{
	enum class Type
	{
		Text,
		Expression,
		Element
	};

	Type type = Type::Text;
	std::string text; // for text and expression payloads
	std::string tag; // empty tag denotes fragment
	std::vector<JsxAttribute> attributes;
	std::vector<JsxNode> children;
	bool self_closing = false;
};

struct JsxTemplate
{
	JsxNode root;
};

std::string trim(const std::string& in)
{
	size_t start = 0;
	while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])))
		++start;
	size_t end = in.size();
	while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])))
		--end;
	return in.substr(start, end - start);
}

std::string html_escape(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	for (char c : in)
	{
		switch (c)
		{
			case '&':
				out += "&amp;";
				break;
			case '<':
				out += "&lt;";
				break;
			case '>':
				out += "&gt;";
				break;
			case '"':
				out += "&quot;";
				break;
			case '\'':
				out += "&#39;";
				break;
			default:
				out.push_back(c);
				break;
		}
	}
	return out;
}

std::string decode_escapes(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i)
	{
		char c = in[i];
		if (c == '\\' && i + 1 < in.size())
		{
			char n = in[++i];
			switch (n)
			{
				case 'n':
					out.push_back('\n');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case '"':
					out.push_back('"');
					break;
				case '\'':
					out.push_back('\'');
					break;
				case '\\':
					out.push_back('\\');
					break;
				default:
					out.push_back(n);
					break;
			}
			continue;
		}
		out.push_back(c);
	}
	return out;
}

bool is_name_char(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.';
}

bool parse_int_key(const std::string& s, s64& out)
{
	char* end = nullptr;
	out = std::strtoll(s.c_str(), &end, 10);
	return end && *end == '\0';
}

using ValueMap = std::unordered_map<std::string, UdonValue>;

std::vector<std::pair<std::string, UdonValue>> ordered_entries(const ValueMap& values)
{
	std::vector<std::pair<std::string, UdonValue>> ordered;
	ordered.reserve(values.size());
	for (const auto& kv : values)
		ordered.push_back(kv);

	std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
	{
		s64 ai = 0;
		s64 bi = 0;
		const bool a_num = parse_int_key(a.first, ai);
		const bool b_num = parse_int_key(b.first, bi);
		if (a_num && b_num)
			return ai < bi;
		if (a_num != b_num)
			return a_num;
		return a.first < b.first;
	});
	return ordered;
}

struct JsxParser
{
	explicit JsxParser(const std::string& src) : source(src), pos(0) {}

	bool parse(JsxTemplate& out, std::string& err)
	{
		JsxNode root;
		root.type = JsxNode::Type::Element;
		if (!parse_children(root, ""))
		{
			err = error;
			return false;
		}
		out.root = std::move(root);
		return true;
	}

	bool parse_children(JsxNode& parent, const std::string& closing_tag)
	{
		while (pos < source.size())
		{
			if (source[pos] == '<')
			{
				if (pos + 1 < source.size() && source[pos + 1] == '/')
				{
					std::string closed = read_closing_tag();
					if (!error.empty())
						return false;
					if (closed != closing_tag)
					{
						if (closing_tag.empty())
							error = "Unexpected closing tag </" + closed + ">";
						else
							error = "Mismatched closing tag </" + closed + "> expected </" + closing_tag + ">";
						return false;
					}
					return true;
				}

				JsxNode child;
				if (!parse_element(child))
					return false;
				parent.children.push_back(std::move(child));
				continue;
			}
			if (source[pos] == '{')
			{
				std::string expr = read_braced();
				if (!error.empty())
					return false;
				JsxNode expr_node;
				expr_node.type = JsxNode::Type::Expression;
				expr_node.text = trim(expr);
				parent.children.push_back(std::move(expr_node));
				continue;
			}

			JsxNode text_node;
			parse_text(text_node);
			if (!text_node.text.empty())
				parent.children.push_back(std::move(text_node));
		}

		if (!closing_tag.empty())
		{
			error = "Unclosed tag <" + closing_tag + ">";
			return false;
		}

		return true;
	}

	void parse_text(JsxNode& node)
	{
		const size_t start = pos;
		while (pos < source.size() && source[pos] != '<' && source[pos] != '{')
			++pos;
		node.type = JsxNode::Type::Text;
		node.text = source.substr(start, pos - start);
	}

	bool parse_element(JsxNode& node)
	{
		++pos; // '<'
		skip_ws();
		if (pos >= source.size())
		{
			error = "Unterminated tag";
			return false;
		}

		if (source[pos] == '>')
		{
			++pos; // fragment <>
			node.type = JsxNode::Type::Element;
			node.tag.clear();
			node.self_closing = false;
			return parse_children(node, "");
		}

		const size_t name_start = pos;
		while (pos < source.size() && is_name_char(source[pos]))
			++pos;
		if (name_start == pos)
		{
			error = "Expected tag name";
			return false;
		}

		node.type = JsxNode::Type::Element;
		node.tag = source.substr(name_start, pos - name_start);
		node.self_closing = false;

		skip_ws();
		while (pos < source.size() && source[pos] != '>' && !(source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '>'))
		{
			JsxAttribute attr;
			if (!parse_attribute(attr))
				return false;
			if (!attr.name.empty() || attr.kind == JsxAttribute::Kind::Spread)
				node.attributes.push_back(std::move(attr));
			skip_ws();
		}

		if (pos >= source.size())
		{
			error = "Unterminated tag <" + node.tag + ">";
			return false;
		}

		if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '>')
		{
			node.self_closing = true;
			pos += 2;
			return true;
		}

		++pos; // '>'
		return parse_children(node, node.tag);
	}

	bool parse_attribute(JsxAttribute& out)
	{
		if (source[pos] == '{')
		{
			std::string expr = trim(read_braced());
			if (!error.empty())
				return false;
			if (expr.size() >= 3 && expr.substr(0, 3) == "...")
			{
				out.kind = JsxAttribute::Kind::Spread;
				out.value = trim(expr.substr(3));
				if (out.value.empty())
					error = "Spread attribute requires an expression";
				return error.empty();
			}
			error = "Unexpected bare expression in attribute list (did you mean `{...props}`?)";
			return false;
		}

		const size_t name_start = pos;
		while (pos < source.size() && is_name_char(source[pos]))
			++pos;
		if (name_start == pos)
		{
			error = "Expected attribute name";
			return false;
		}

		out.name = source.substr(name_start, pos - name_start);
		out.kind = JsxAttribute::Kind::Boolean;
		skip_ws();

		if (pos < source.size() && source[pos] == '=')
		{
			++pos;
			skip_ws();
			if (pos >= source.size())
			{
				error = "Expected attribute value after '='";
				return false;
			}

			if (source[pos] == '"' || source[pos] == '\'')
			{
				const char quote = source[pos++];
				out.kind = JsxAttribute::Kind::Static;
				out.value = read_quoted(quote);
				return error.empty();
			}
			if (source[pos] == '{')
			{
				out.kind = JsxAttribute::Kind::Expression;
				out.value = trim(read_braced());
				return error.empty();
			}

			const size_t val_start = pos;
			while (pos < source.size() && !std::isspace(static_cast<unsigned char>(source[pos])) && source[pos] != '>' && source[pos] != '/')
				++pos;
			out.kind = JsxAttribute::Kind::Static;
			out.value = source.substr(val_start, pos - val_start);
		}

		return true;
	}

	std::string read_quoted(char quote)
	{
		std::string out;
		while (pos < source.size())
		{
			char c = source[pos++];
			if (c == '\\' && pos < source.size())
			{
				char next = source[pos++];
				switch (next)
				{
					case 'n':
						out.push_back('\n');
						break;
					case 't':
						out.push_back('\t');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case '\\':
						out.push_back('\\');
						break;
					case '"':
						out.push_back('"');
						break;
					case '\'':
						out.push_back('\'');
						break;
					default:
						out.push_back(next);
						break;
				}
				continue;
			}
			if (c == quote)
				return out;
			out.push_back(c);
		}
		error = "Unterminated quoted attribute";
		return out;
	}

	std::string read_braced()
	{
		if (source[pos] != '{')
		{
			error = "Internal parser error: expected '{'";
			return "";
		}
		++pos; // skip '{'
		const size_t start = pos;
		int depth = 1;
		bool in_string = false;
		char string_ch = 0;
		while (pos < source.size())
		{
			char c = source[pos++];
			if (in_string)
			{
				if (c == '\\' && pos < source.size())
				{
					++pos;
					continue;
				}
				if (c == string_ch)
					in_string = false;
				continue;
			}
			if (c == '"' || c == '\'')
			{
				in_string = true;
				string_ch = c;
				continue;
			}
			if (c == '{')
				++depth;
			else if (c == '}')
			{
				--depth;
				if (depth == 0)
				{
					const size_t end = pos - 1;
					return source.substr(start, end - start);
				}
			}
		}
		error = "Unterminated expression";
		return "";
	}

	std::string read_closing_tag()
	{
		pos += 2; // "</"
		skip_ws();
		const size_t name_start = pos;
		while (pos < source.size() && is_name_char(source[pos]))
			++pos;
		std::string name = source.substr(name_start, pos - name_start);
		skip_ws();
		if (pos >= source.size() || source[pos] != '>')
		{
			error = "Unterminated closing tag";
			return "";
		}
		++pos;
		return name;
	}

	void skip_ws()
	{
		while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos])))
			++pos;
	}

	std::string source;
	size_t pos = 0;
	std::string error;
};

using PropMap = std::unordered_map<std::string, UdonValue>;

std::vector<std::string> split_path(const std::string& expr)
{
	std::vector<std::string> parts;
	std::string current;
	for (char c : expr)
	{
		if (c == '.' || c == ':')
		{
			if (!current.empty())
				parts.push_back(current);
			current.clear();
		}
		else
			current.push_back(c);
	}
	if (!current.empty())
		parts.push_back(current);
	return parts;
}

bool parse_literal_value(const std::string& expr, UdonValue& out)
{
	const std::string trimmed = trim(expr);
	if (trimmed.empty())
		return false;

	if ((trimmed.front() == '"' && trimmed.back() == '"') || (trimmed.front() == '\'' && trimmed.back() == '\''))
	{
		out = make_string(decode_escapes(trimmed.substr(1, trimmed.size() - 2)));
		return true;
	}

	std::string lowered;
	lowered.reserve(trimmed.size());
	for (char c : trimmed)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	if (lowered == "true")
	{
		out = make_bool(true);
		return true;
	}
	if (lowered == "false")
	{
		out = make_bool(false);
		return true;
	}
	if (lowered == "none")
	{
		out = make_none();
		return true;
	}

	char* end = nullptr;
	const double val = std::strtod(trimmed.c_str(), &end);
	if (end && end != trimmed.c_str() && *end == '\0')
	{
		const bool is_int = trimmed.find('.') == std::string::npos && trimmed.find('e') == std::string::npos && trimmed.find('E') == std::string::npos;
		if (is_int)
			out = make_int(static_cast<s64>(val));
		else
			out = make_float(static_cast<f64>(val));
		return true;
	}

	return false;
}

bool resolve_prop_path(const PropMap& props, const std::string& expr, UdonValue& out)
{
	const auto segments = split_path(expr);
	if (segments.empty())
		return false;

	const PropMap* current = &props;
	for (size_t i = 0; i < segments.size(); ++i)
	{
		auto it = current->find(segments[i]);
		if (it == current->end())
			return false;
		const UdonValue& value = it->second;
		if (i + 1 == segments.size())
		{
			out = value;
			return true;
		}
		if (value.type == UdonValue::Type::Array && value.array_map)
		{
			current = &value.array_map->values;
			continue;
		}
		return false;
	}

	return false;
}

UdonValue resolve_expression(const std::string& expr, const PropMap& props)
{
	UdonValue literal;
	if (parse_literal_value(expr, literal))
		return literal;

	UdonValue resolved;
	if (resolve_prop_path(props, expr, resolved))
		return resolved;

	return make_none();
}

std::string render_style_string(const UdonValue& v)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return value_to_string(v);
	auto ordered = ordered_entries(v.array_map->values);
	std::ostringstream ss;
	bool first = true;
	for (const auto& kv : ordered)
	{
		if (!first)
			ss << "; ";
		ss << kv.first << ": " << value_to_string(kv.second);
		first = false;
	}
	return ss.str();
}

std::string render_value_plain(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::None:
			return "";
		case UdonValue::Type::Bool:
			return v.int_value ? "true" : "false";
		case UdonValue::Type::Array:
		{
			if (!v.array_map)
				return "";
			auto ordered = ordered_entries(v.array_map->values);
			std::ostringstream ss;
			bool first = true;
			for (const auto& kv : ordered)
			{
				if (!first)
					ss << " ";
				ss << render_value_plain(kv.second);
				first = false;
			}
			return ss.str();
		}
		default:
			return value_to_string(v);
	}
}

std::string render_value_for_text(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::None:
		case UdonValue::Type::Bool:
			return "";
		case UdonValue::Type::Array:
		{
			if (!v.array_map)
				return "";
			std::ostringstream ss;
			auto ordered = ordered_entries(v.array_map->values);
			for (const auto& kv : ordered)
				ss << render_value_for_text(kv.second);
			return ss.str();
		}
		default:
			return html_escape(value_to_string(v));
	}
}

struct RenderContext
{
	const ValueMap& components;
	const ValueMap& options;
	UdonInterpreter* interp = nullptr;
	CodeLocation* err = nullptr;
};

struct AttrEval
{
	std::string name;
	UdonValue value;
	bool raw = false;
};

std::vector<AttrEval> evaluate_attributes(const std::vector<JsxAttribute>& attrs, const PropMap& props)
{
	std::vector<AttrEval> evaluated;
	evaluated.reserve(attrs.size());

	auto push_attr = [&](const std::string& name, const UdonValue& val, bool raw)
	{
		AttrEval e;
		e.name = name;
		e.value = val;
		e.raw = raw;
		evaluated.push_back(std::move(e));
	};

	for (const auto& attr : attrs)
	{
		switch (attr.kind)
		{
			case JsxAttribute::Kind::Static:
				push_attr(attr.name, make_string(attr.value), true);
				break;
			case JsxAttribute::Kind::Boolean:
				push_attr(attr.name, make_bool(true), true);
				break;
			case JsxAttribute::Kind::Expression:
				push_attr(attr.name, resolve_expression(attr.value, props), false);
				break;
			case JsxAttribute::Kind::Spread:
			{
				UdonValue spread_val = resolve_expression(attr.value, props);
				if (spread_val.type == UdonValue::Type::Array && spread_val.array_map)
				{
					auto ordered = ordered_entries(spread_val.array_map->values);
					for (const auto& kv : ordered)
						push_attr(kv.first, kv.second, false);
				}
				break;
			}
		}
	}

	return evaluated;
}

std::string render_attributes(const std::vector<JsxAttribute>& attrs, const PropMap& props)
{
	auto evaluated = evaluate_attributes(attrs, props);
	std::unordered_map<std::string, size_t> last_index;
	for (size_t i = 0; i < evaluated.size(); ++i)
		last_index[evaluated[i].name] = i;

	std::ostringstream ss;
	for (size_t i = 0; i < evaluated.size(); ++i)
	{
		if (last_index[evaluated[i].name] != i)
			continue;

		const AttrEval& e = evaluated[i];
		if (e.value.type == UdonValue::Type::None)
			continue;

		if (e.value.type == UdonValue::Type::Bool)
		{
			if (e.value.int_value)
				ss << " " << e.name;
			continue;
		}

		std::string raw_value = (e.name == "style") ? render_style_string(e.value) : render_value_plain(e.value);
		if (!e.raw)
			raw_value = html_escape(raw_value);
		ss << " " << e.name << "=\"" << raw_value << "\"";
	}

	return ss.str();
}

std::string render_node(const JsxNode& node,
	const PropMap& props,
	const RenderContext& ctx);

std::string render_children(const std::vector<JsxNode>& children,
	const PropMap& props,
	const RenderContext& ctx)
{
	std::ostringstream ss;
	for (const auto& child : children)
	{
		ss << render_node(child, props, ctx);
		if (ctx.err && ctx.err->has_error)
			return "";
	}
	return ss.str();
}

UdonValue make_object_value(UdonInterpreter* interp, const ValueMap& map)
{
	UdonValue v{};
	v.type = UdonValue::Type::Array;
	v.array_map = interp ? interp->allocate_array() : nullptr;
	if (v.array_map)
	{
		for (const auto& kv : map)
			v.array_map->values[kv.first] = kv.second;
	}
	return v;
}

std::string render_node(const JsxNode& node,
	const PropMap& props,
	const RenderContext& ctx)
{
	switch (node.type)
	{
		case JsxNode::Type::Text:
			return node.text;
		case JsxNode::Type::Expression:
			return render_value_for_text(resolve_expression(node.text, props));
		case JsxNode::Type::Element:
		{
			if (node.tag.empty())
			{
				std::ostringstream ss;
				for (const auto& child : node.children)
					ss << render_node(child, props, ctx);
				return ss.str();
			}

			{
				auto comp_it = ctx.components.find(node.tag);
				if (comp_it != ctx.components.end())
				{
					const UdonValue& comp_val = comp_it->second;
					if (comp_val.type != UdonValue::Type::Function || !comp_val.function)
					{
						if (ctx.err)
						{
							ctx.err->has_error = true;
							ctx.err->opt_error_message = "Component '" + node.tag + "' is not callable";
						}
						return "";
					}

					auto evaluated = evaluate_attributes(node.attributes, props);
					ValueMap attr_map;
					std::unordered_map<std::string, size_t> last_index;
					for (size_t i = 0; i < evaluated.size(); ++i)
						last_index[evaluated[i].name] = i;
					for (size_t i = 0; i < evaluated.size(); ++i)
					{
						if (last_index[evaluated[i].name] != i)
							continue;
						const AttrEval& e = evaluated[i];
						if (e.value.type == UdonValue::Type::None)
							continue;
						attr_map[e.name] = e.value;
					}

					std::string children_html = render_children(node.children, props, ctx);
					if (ctx.err && ctx.err->has_error)
						return "";

					std::vector<UdonValue> args;
					args.push_back(make_object_value(ctx.interp, attr_map));
					args.push_back(make_string(children_html));
					args.push_back(make_object_value(ctx.interp, ctx.options));

					UdonValue component_out;
					CodeLocation call_err = ctx.interp ? ctx.interp->invoke_function(comp_val, args, {}, component_out) : CodeLocation{};
					if (ctx.err)
						*ctx.err = call_err;
					if (call_err.has_error)
						return "";
					if (component_out.type == UdonValue::Type::String)
						return component_out.string_value;
					return value_to_string(component_out);
				}
			}

			std::ostringstream ss;
			ss << "<" << node.tag << render_attributes(node.attributes, props);
			if (node.self_closing && node.children.empty())
			{
				ss << "/>";
				return ss.str();
			}
			ss << ">";
			for (const auto& child : node.children)
			{
				ss << render_node(child, props, ctx);
				if (ctx.err && ctx.err->has_error)
					return "";
			}
			ss << "</" << node.tag << ">";
			return ss.str();
		}
	}
	return "";
}

std::shared_ptr<JsxTemplate> jsx_compile(const std::string& source, std::string& error)
{
	auto tmpl = std::make_shared<JsxTemplate>();
	JsxParser parser(source);
	if (!parser.parse(*tmpl, error))
		return nullptr;
	return tmpl;
}

std::string jsx_render(const JsxTemplate& tmpl,
	const std::unordered_map<std::string, UdonValue>& props,
	const std::unordered_map<std::string, UdonValue>& components,
	const std::unordered_map<std::string, UdonValue>& options,
	UdonInterpreter* interp,
	CodeLocation& err)
{
	RenderContext ctx{ components, options, interp, &err };
	err.has_error = false;
	return render_node(tmpl.root, props, ctx);
}
