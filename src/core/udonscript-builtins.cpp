#include "udonscript-internal.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <cstdio>
#include <sys/stat.h>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iterator>
#include <unordered_set>
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#include <unordered_set>

using namespace udon_script_helpers;

static bool parse_bool_string(const std::string& s, bool& out)
{
	std::string lowered;
	lowered.reserve(s.size());
	for (char c : s)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1")
	{
		out = true;
		return true;
	}
	if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0")
	{
		out = false;
		return true;
	}
	return false;
}

static bool parse_number_string(const std::string& s, double& out, bool& is_int)
{
	char* end = nullptr;
	out = std::strtod(s.c_str(), &end);
	if (end == s.c_str() || *end != '\0')
		return false;
	const bool has_dot = s.find('.') != std::string::npos;
	const bool has_exp = s.find('e') != std::string::npos || s.find('E') != std::string::npos;
	is_int = !(has_dot || has_exp);
	return true;
}

static std::string trim_string(const std::string& s, bool left, bool right)
{
	size_t start = 0;
	size_t end = s.size();
	if (left)
	{
		while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
			++start;
	}
	if (right)
	{
		while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
			--end;
	}
	return s.substr(start, end - start);
}

static std::string json_escape(const std::string& s)
{
	std::string out;
	for (char c : s)
	{
		switch (c)
		{
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				out.push_back(c);
				break;
		}
	}
	return out;
}

static std::string to_json(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::String:
			return "\"" + json_escape(v.string_value) + "\"";
		case UdonValue::Type::S32:
			return std::to_string(v.s32_value);
		case UdonValue::Type::F32:
		{
			std::ostringstream ss;
			ss << v.f32_value;
			return ss.str();
		}
		case UdonValue::Type::Bool:
			return v.s32_value ? "true" : "false";
		case UdonValue::Type::Array:
		{
			if (!v.array_map)
				return "null";
			std::ostringstream ss;
			ss << "{";
			bool first = true;
			for (const auto& kv : v.array_map->values)
			{
				if (!first)
					ss << ",";
				first = false;
				ss << "\"" << json_escape(kv.first) << "\":" << to_json(kv.second);
			}
			ss << "}";
			return ss.str();
		}
		case UdonValue::Type::None:
		default:
			return "null";
	}
}

static std::string url_encode(const std::string& s)
{
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex;
	for (unsigned char c : s)
	{
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			escaped << c;
		else if (c == ' ')
			escaped << '+';
		else
			escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
	}
	return escaped.str();
}

static std::string url_decode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '+')
			out.push_back(' ');
		else if (s[i] == '%' && i + 2 < s.size())
		{
			std::string hex = s.substr(i + 1, 2);
			char ch = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
			out.push_back(ch);
			i += 2;
		}
		else
			out.push_back(s[i]);
	}
	return out;
}

static const std::string b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string& in)
{
	std::string out;
	int val = 0;
	int valb = -6;
	for (unsigned char c : in)
	{
		val = (val << 8) + c;
		valb += 8;
		while (valb >= 0)
		{
			out.push_back(b64_chars[(val >> valb) & 0x3F]);
			valb -= 6;
		}
	}
	if (valb > -6)
		out.push_back(b64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
	while (out.size() % 4)
		out.push_back('=');
	return out;
}

static std::string base64_decode(const std::string& in)
{
	std::vector<int> T(256, -1);
	for (int i = 0; i < 64; i++)
		T[b64_chars[i]] = i;
	std::string out;
	int val = 0;
	int valb = -8;
	for (unsigned char c : in)
	{
		if (T[c] == -1)
			break;
		val = (val << 6) + T[c];
		valb += 6;
		if (valb >= 0)
		{
			out.push_back(char((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	return out;
}

static UdonValue parse_form_data(const std::string& s, UdonInterpreter* interp)
{
	UdonValue out;
	out.type = UdonValue::Type::Array;
	out.array_map = interp->allocate_array();
	size_t pos = 0;
	while (pos < s.size())
	{
		size_t amp = s.find('&', pos);
		std::string pair = (amp == std::string::npos) ? s.substr(pos) : s.substr(pos, amp - pos);
		size_t eq = pair.find('=');
		std::string key = (eq == std::string::npos) ? pair : pair.substr(0, eq);
		std::string val = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
		key = url_decode(key);
		val = url_decode(val);
		array_set(out, key, make_string(val));
		if (amp == std::string::npos)
			break;
		pos = amp + 1;
	}
	return out;
}

struct JsonParser
{
	const std::string& s;
	size_t pos = 0;

	JsonParser(const std::string& str) : s(str) {}

	void skip_ws()
	{
		while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
			++pos;
	}

	bool parse_value(UdonValue& out)
	{
		skip_ws();
		if (pos >= s.size())
			return false;
		char c = s[pos];
		if (c == '"')
			return parse_string(out);
		if (c == '{')
			return parse_object(out);
		if (c == '[')
			return parse_array(out);
		if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+')
			return parse_number(out);
		if (s.compare(pos, 4, "true") == 0)
		{
			pos += 4;
			out = make_bool(true);
			return true;
		}
		if (s.compare(pos, 5, "false") == 0)
		{
			pos += 5;
			out = make_bool(false);
			return true;
		}
		if (s.compare(pos, 4, "null") == 0)
		{
			pos += 4;
			out = make_none();
			return true;
		}
		return false;
	}

	bool parse_string(UdonValue& out)
	{
		if (s[pos] != '"')
			return false;
		++pos;
		std::string val;
		while (pos < s.size())
		{
			char c = s[pos++];
			if (c == '"')
				break;
			if (c == '\\' && pos < s.size())
			{
				char esc = s[pos++];
				switch (esc)
				{
					case 'n':
						val.push_back('\n');
						break;
					case 'r':
						val.push_back('\r');
						break;
					case 't':
						val.push_back('\t');
						break;
					case '\\':
						val.push_back('\\');
						break;
					case '"':
						val.push_back('"');
						break;
					default:
						val.push_back(esc);
						break;
				}
			}
			else
			{
				val.push_back(c);
			}
		}
		out = make_string(val);
		return true;
	}

	bool parse_number(UdonValue& out)
	{
		size_t start = pos;
		if (s[pos] == '+' || s[pos] == '-')
			++pos;
		while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.'))
			++pos;
		std::string num = s.substr(start, pos - start);
		double d = std::atof(num.c_str());
		if (num.find('.') == std::string::npos)
			out = make_int(static_cast<s32>(d));
		else
			out = make_float(static_cast<f32>(d));
		return true;
	}

	bool parse_array(UdonValue& out)
	{
		if (s[pos] != '[')
			return false;
		++pos;
		out = make_array();
		int idx = 0;
		skip_ws();
		if (pos < s.size() && s[pos] == ']')
		{
			++pos;
			return true;
		}
		while (pos < s.size())
		{
			UdonValue val;
			if (!parse_value(val))
				return false;
			array_set(out, std::to_string(idx++), val);
			skip_ws();
			if (pos < s.size() && s[pos] == ',')
			{
				++pos;
				continue;
			}
			if (pos < s.size() && s[pos] == ']')
			{
				++pos;
				return true;
			}
			return false;
		}
		return false;
	}

	bool parse_object(UdonValue& out)
	{
		if (s[pos] != '{')
			return false;
		++pos;
		out = make_array();
		skip_ws();
		if (pos < s.size() && s[pos] == '}')
		{
			++pos;
			return true;
		}
		while (pos < s.size())
		{
			UdonValue key;
			if (!parse_string(key))
				return false;
			skip_ws();
			if (pos >= s.size() || s[pos] != ':')
				return false;
			++pos;
			UdonValue val;
			if (!parse_value(val))
				return false;
			array_set(out, key.string_value, val);
			skip_ws();
			if (pos < s.size() && s[pos] == ',')
			{
				++pos;
				skip_ws();
				continue;
			}
			if (pos < s.size() && s[pos] == '}')
			{
				++pos;
				return true;
			}
			return false;
		}
		return false;
	}
};

namespace udon_script_builtins
{

	void register_builtins(UdonInterpreter* interp)
	{
		auto unary = [interp](const std::string& name, double (*fn)(double))
		{
			interp->register_function(name, "x:number", "number", [fn, name](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
			{
				if (positional.size() != 1 || !is_numeric(positional[0]))
				{
					err.has_error = true;
					err.opt_error_message = name + " expects 1 numeric argument";
					return true;
				}
				out = wrap_number_unary(fn(as_number(positional[0])), positional[0]);
				return true;
			});
		};

		auto binary = [interp](const std::string& name, double (*fn)(double, double))
		{
			interp->register_function(name, "a:number, b:number", "number", [fn, name](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
			{
				if (positional.size() != 2 || !is_numeric(positional[0]) || !is_numeric(positional[1]))
				{
					err.has_error = true;
					err.opt_error_message = name + " expects 2 numeric arguments";
					return true;
				}
				out = wrap_number(fn(as_number(positional[0]), as_number(positional[1])), positional[0], positional[1]);
				return true;
			});
		};

		interp->register_function("array", "values:any...", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
		{
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();
			int idx = 0;
			for (const auto& v : positional)
			{
				array_set(out, std::to_string(idx++), v);
			}
			return true;
		});

		interp->register_function("__object_literal", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty())
			{
				err.has_error = true;
				err.opt_error_message = "__object_literal: internal error - no arguments";
				return true;
			}

			if (positional.back().type != UdonValue::Type::S32)
			{
				err.has_error = true;
				err.opt_error_message = "__object_literal: internal error - invalid count";
				return true;
			}

			s32 count = positional.back().s32_value;

			if (positional.size() != static_cast<size_t>(count * 2 + 1))
			{
				err.has_error = true;
				err.opt_error_message = "__object_literal: internal error - arg count mismatch";
				return true;
			}

			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();

			for (s32 i = 0; i < count; i++)
			{
				const UdonValue& key = positional[count + i];
				const UdonValue& value = positional[i];

				std::string key_str = key_from_value(key);

				array_set(out, key_str, value);
			}

			return true;
		});

		interp->register_function("print", "values:any...", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
		{
			std::ostringstream ss;
			bool first = true;
			for (const auto& v : positional)
			{
				if (!first)
					ss << " ";
				first = false;
				ss << value_to_string(v);
			}
			std::cout << ss.str() << std::endl;
			out = make_none();
			return true;
		});

		auto register_alias = [interp](const std::string& alias, const std::string& target)
		{
			auto it = interp->builtins.find(target);
			if (it != interp->builtins.end())
				interp->builtins[alias] = it->second;
		};

		interp->register_function("keys", "arr:any", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty())
			{
				err.has_error = true;
				err.opt_error_message = "keys expects an array";
				return true;
			}

			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();

			int idx = 0;
			if (positional[0].type == UdonValue::Type::Array && positional[0].array_map)
			{
				for (const auto& kv : positional[0].array_map->values)
				{
					std::string idx_str = std::to_string(idx);
					array_set(out, idx_str, make_string(kv.first));
					idx++;
				}
			}
			else if (positional[0].type == UdonValue::Type::String)
			{
				for (size_t i = 0; i < positional[0].string_value.size(); ++i)
					array_set(out, std::to_string(idx++), make_string(std::to_string(i)));
			}
			else
			{
				err.has_error = true;
				err.opt_error_message = "keys expects an array";
			}

			return true;
		});
		register_alias("array_keys", "keys");

		interp->register_function("array_get", "arr:any, key:any", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() < 2)
			{
				err.has_error = true;
				err.opt_error_message = "array_get expects (array, key)";
				return true;
			}

			std::string key_str = key_from_value(positional[1]);
			if (positional[0].type == UdonValue::Type::Array)
			{
				if (!array_get(positional[0], key_str, out))
					out = make_none();
			}
			else if (positional[0].type == UdonValue::Type::String)
			{
				try
				{
					int idx = std::stoi(key_str);
					if (idx >= 0 && static_cast<size_t>(idx) < positional[0].string_value.size())
						out = make_string(std::string(1, positional[0].string_value[static_cast<size_t>(idx)]));
					else
						out = make_none();
				}
				catch (...)
				{
					out = make_none();
				}
			}
			else
			{
				out = make_none();
			}

			return true;
		});

		interp->register_function("load_from_file", "path:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "load_from_file expects (path)";
				return true;
			}
			std::string path = value_to_string(positional[0]);
			std::ifstream file(path, std::ios::binary);
			if (!file)
			{
				err.has_error = true;
				err.opt_error_message = "Could not read file: " + path;
				return true;
			}
			std::ostringstream ss;
			ss << file.rdbuf();
			out = make_string(ss.str());
			return true;
		});

		interp->register_function("save_to_file", "path:string, data:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "save_to_file expects (path, data)";
				return true;
			}
			std::string path = value_to_string(positional[0]);
			std::ofstream file(path, std::ios::binary);
			if (!file)
			{
				err.has_error = true;
				err.opt_error_message = "Could not write file: " + path;
				return true;
			}
			file << value_to_string(positional[1]);
			file.close();
			out = make_none();
			return true;
		});

		interp->register_function("read_entire_file", "path:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "read_entire_file expects (path)";
				return true;
			}
			std::string path = value_to_string(positional[0]);
			std::ifstream file(path, std::ios::binary);
			if (!file)
			{
				err.has_error = true;
				err.opt_error_message = "Could not read file: " + path;
				return true;
			}
			std::ostringstream ss;
			ss << file.rdbuf();
			out = make_string(ss.str());
			return true;
		});

		interp->register_function("write_entire_file", "path:string, data:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "write_entire_file expects (path, data)";
				return true;
			}
			std::string path = value_to_string(positional[0]);
			std::ofstream file(path, std::ios::binary);
			if (!file)
			{
				err.has_error = true;
				err.opt_error_message = "Could not write file: " + path;
				return true;
			}
			file << value_to_string(positional[1]);
			file.close();
			out = make_none();
			return true;
		});

		interp->register_function("dl_open", "path:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
#if !defined(__unix__) && !defined(__APPLE__)
			err.has_error = true;
			err.opt_error_message = "dl_open is only supported on POSIX platforms";
			return true;
#else
			if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
			{
				err.has_error = true;
				err.opt_error_message = "dl_open expects a single string path";
				return true;
			}
			std::string path = positional[0].string_value;
			void* handle = dlopen(path.c_str(), RTLD_NOW);
			if (!handle)
			{
				err.has_error = true;
				err.opt_error_message = std::string("dl_open failed: ") + dlerror();
				return true;
			}
			s32 handle_id = interp->register_dl_handle(handle);

			out = make_array();
			array_set(out, "_handle", make_int(handle_id));

			auto make_handler = [&](const std::string& tag) -> UdonValue
			{
				UdonValue fn{};
				fn.type = UdonValue::Type::Function;
				fn.function = interp->allocate_function();
				fn.function->handler = tag;
				fn.function->handler_data = handle_id;
				return fn;
			};

			array_set(out, "call", make_handler("dl_call"));
			array_set(out, "close", make_handler("dl_close"));
			return true;
#endif
		});

		interp->register_function("file_size", "path:string", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "file_size expects (path)";
				return true;
			}
			std::string path = value_to_string(positional[0]);
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file)
			{
				err.has_error = true;
				err.opt_error_message = "Could not access file: " + path;
				return true;
			}
			std::streampos size = file.tellg();
			out = make_int(static_cast<s32>(size));
			return true;
		});

		interp->register_function("file_time", "path:string", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "file_time expects (path)";
				return true;
			}
			std::string path = value_to_string(positional[0]);
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
			{
				err.has_error = true;
				err.opt_error_message = "Could not access file: " + path;
				return true;
			}
			out = make_int(static_cast<s32>(st.st_mtime));
			return true;
		});

		interp->register_function("import", "path:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
			{
				err.has_error = true;
				err.opt_error_message = "import expects a single string path";
				return true;
			}

			std::string path = positional[0].string_value;
			std::ifstream file(path, std::ios::binary);
			if (!file)
			{
				err.has_error = true;
				err.opt_error_message = "import: could not open '" + path + "'";
				return true;
			}
			std::ostringstream ss;
			ss << file.rdbuf();
			std::string source = ss.str();

			std::unique_ptr<UdonInterpreter> sub = std::make_unique<UdonInterpreter>();
			sub->builtins = interp->builtins; // share host-registered builtins

			CodeLocation compile_res = sub->compile(source);
			if (compile_res.has_error)
			{
				err = compile_res;
				return true;
			}

			s32 sub_id = interp->register_imported_interpreter(std::move(sub));

			out = make_array();
			UdonInterpreter* sub_ref = interp->get_imported_interpreter(sub_id);
			if (sub_ref)
			{
				for (const auto& kv : sub_ref->globals)
				{
					array_set(out, kv.first, kv.second);
				}
				for (const auto& kv : sub_ref->instructions)
				{
					const std::string& name = kv.first;
					if (name.rfind("__", 0) == 0)
						continue;
					UdonValue fn_val;
					fn_val.type = UdonValue::Type::Function;
					fn_val.function = interp->allocate_function();
					fn_val.function->handler = "import_forward";
					fn_val.function->handler_data = sub_id;
					fn_val.function->template_body = name; // store target function name
					array_set(out, name, fn_val);
				}
			}
			return true;
		});

		interp->register_function("shell", "parts:any...", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty())
			{
				err.has_error = true;
				err.opt_error_message = "shell expects at least one argument";
				return true;
			}

			std::ostringstream command_builder;
			for (size_t i = 0; i < positional.size(); i++)
			{
				if (i > 0)
					command_builder << " ";
				command_builder << value_to_string(positional[i]);
			}
			std::string command = command_builder.str();

			FILE* pipe = popen(command.c_str(), "r");
			if (!pipe)
			{
				err.has_error = true;
				err.opt_error_message = "Failed to execute command: " + command;
				return true;
			}

			std::ostringstream result;
			char buffer[256];
			while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
			{
				result << buffer;
			}

			int exit_code = pclose(pipe);
			(void)exit_code; // Ignore exit code for now

			std::string output = result.str();
			if (!output.empty() && output.back() == '\n')
				output.pop_back();

			out = make_string(output);
			return true;
		});

		interp->register_function("to_shellarg", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_shellarg expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);

			std::string escaped = "'";
			for (char c : s)
			{
				if (c == '\'')
					escaped += "'\\''";
				else
					escaped += c;
			}
			escaped += "'";

			out = make_string(escaped);
			return true;
		});

		interp->register_function("to_htmlsafe", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_htmlsafe expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);

			std::string escaped;
			for (char c : s)
			{
				switch (c)
				{
					case '&':
						escaped += "&amp;";
						break;
					case '<':
						escaped += "&lt;";
						break;
					case '>':
						escaped += "&gt;";
						break;
					case '"':
						escaped += "&quot;";
						break;
					case '\'':
						escaped += "&#39;";
						break;
					default:
						escaped += c;
						break;
				}
			}

			out = make_string(escaped);
			return true;
		});

		interp->register_function("to_sqlarg", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_sqlarg expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);

			std::string escaped;
			for (char c : s)
			{
				if (c == '\'')
					escaped += "''";
				else
					escaped += c;
			}

			out = make_string(escaped);
			return true;
		});

		interp->register_function("split", "s:string, delim:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "split expects (string, delim)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::string delim = value_to_string(positional[1]);
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();
			if (delim.empty())
			{
				for (size_t i = 0; i < s.size(); ++i)
					array_set(out, std::to_string(i), make_string(std::string(1, s[i])));
				return true;
			}
			size_t idx = 0;
			size_t pos = 0;
			while (true)
			{
				size_t next = s.find(delim, pos);
				std::string chunk = (next == std::string::npos) ? s.substr(pos) : s.substr(pos, next - pos);
				array_set(out, std::to_string(idx++), make_string(chunk));
				if (next == std::string::npos)
					break;
				pos = next + delim.size();
			}
			return true;
		});

		interp->register_function("glyphs", "s:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "glyphs expects (string)";
				return true;
			}
			const std::string& s = value_to_string(positional[0]);
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();
			size_t idx = 0;
			for (size_t i = 0; i < s.size();)
			{
				unsigned char c = static_cast<unsigned char>(s[i]);
				size_t len = 1;
				if ((c & 0x80) == 0)
					len = 1;
				else if ((c & 0xE0) == 0xC0)
					len = 2;
				else if ((c & 0xF0) == 0xE0)
					len = 3;
				else if ((c & 0xF8) == 0xF0)
					len = 4;
				if (i + len > s.size())
					len = 1;
				std::string glyph = s.substr(i, len);
				array_set(out, std::to_string(idx++), make_string(glyph));
				i += len;
			}
			return true;
		});

		interp->register_function("join", "arr:array, delim:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2 || positional[0].type != UdonValue::Type::Array)
			{
				err.has_error = true;
				err.opt_error_message = "join expects (array, delim)";
				return true;
			}
			std::string delim = value_to_string(positional[1]);
			std::vector<std::pair<int, std::string>> elems;
			for (const auto& kv : positional[0].array_map->values)
			{
				int idx = 0;
				try
				{
					idx = std::stoi(kv.first);
				}
				catch (...)
				{
					continue;
				}
				elems.emplace_back(idx, value_to_string(kv.second));
			}
			std::sort(elems.begin(), elems.end(), [](auto& a, auto& b)
			{ return a.first < b.first; });
			std::ostringstream ss;
			for (size_t i = 0; i < elems.size(); ++i)
			{
				if (i)
					ss << delim;
				ss << elems[i].second;
			}
			out = make_string(ss.str());
			return true;
		});

		interp->register_function("chr", "code:s32", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "chr expects (code)";
				return true;
			}
			int code = static_cast<int>(as_number(positional[0]));
			if (code < 0 || code > 255)
				code = code & 0xFF;
			std::string s(1, static_cast<char>(code));
			out = make_string(s);
			return true;
		});

		interp->register_function("vec2", "x:number, y:number", "vec2", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "vec2 expects (x, y)";
				return true;
			}
			out = make_vec2(static_cast<f32>(as_number(positional[0])), static_cast<f32>(as_number(positional[1])));
			return true;
		});

		interp->register_function("vec3", "x:number, y:number, z:number", "vec3", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 3)
			{
				err.has_error = true;
				err.opt_error_message = "vec3 expects (x, y, z)";
				return true;
			}
			out = make_vec3(static_cast<f32>(as_number(positional[0])), static_cast<f32>(as_number(positional[1])), static_cast<f32>(as_number(positional[2])));
			return true;
		});

		interp->register_function("vec4", "x:number, y:number, z:number, w:number", "vec4", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 4)
			{
				err.has_error = true;
				err.opt_error_message = "vec4 expects (x, y, z, w)";
				return true;
			}
			out = make_vec4(static_cast<f32>(as_number(positional[0])), static_cast<f32>(as_number(positional[1])), static_cast<f32>(as_number(positional[2])), static_cast<f32>(as_number(positional[3])));
			return true;
		});

		unary("sqrt", std::sqrt);
		unary("abs", std::fabs);
		unary("sin", std::sin);
		unary("cos", std::cos);
		unary("tan", std::tan);
		unary("asin", std::asin);
		unary("acos", std::acos);
		unary("atan", std::atan);
		unary("floor", std::floor);
		unary("ceil", std::ceil);
		unary("round", std::round);
		unary("exp", std::exp);
		unary("log", std::log);
		unary("log10", std::log10);

		binary("pow", std::pow);
		binary("atan2", std::atan2);
		binary("min", [](double a, double b)
		{ return a < b ? a : b; });
		binary("max", [](double a, double b)
		{ return a > b ? a : b; });

		interp->register_function("len", "value:any", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "len expects 1 argument";
				return true;
			}
			const auto& v = positional[0];
			if (v.type == UdonValue::Type::String)
				out = make_int(static_cast<s32>(v.string_value.size()));
			else if (v.type == UdonValue::Type::Array && v.array_map)
				out = make_int(static_cast<s32>(v.array_map->values.size()));
			else
				out = make_int(0);
			return true;
		});
		register_alias("array_len", "len");

		interp->register_function("$html", "template:string", "function", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
			{
				err.has_error = true;
				err.opt_error_message = "$html expects a single string template";
				return true;
			}

			auto* fn_obj = interp->allocate_function();
			fn_obj->handler = "html_template";
			fn_obj->template_body = positional[0].string_value;

			out.type = UdonValue::Type::Function;
			out.function = fn_obj;
			return true;
		});

		interp->register_function("substr", "s:string, start:s32, count:s32", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() < 2 || positional.size() > 3)
			{
				err.has_error = true;
				err.opt_error_message = "substr expects (string, start, [count])";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			s32 start = static_cast<s32>(as_number(positional[1]));
			s32 count = positional.size() == 3 ? static_cast<s32>(as_number(positional[2])) : static_cast<s32>(s.size());
			if (start < 0 || start >= static_cast<s32>(s.size()))
			{
				out = make_string("");
				return true;
			}
			out = make_string(s.substr(start, count));
			return true;
		});

		interp->register_function("replace", "s:string, old:string, new:string, count:s32", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() < 3 || positional.size() > 4)
			{
				err.has_error = true;
				err.opt_error_message = "replace expects (string, old, new, [count])";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::string from = value_to_string(positional[1]);
			std::string to = value_to_string(positional[2]);
			int count = -1;
			if (positional.size() == 4)
				count = static_cast<int>(as_number(positional[3]));
			if (from.empty())
			{
				out = make_string(s);
				return true;
			}
			size_t pos = 0;
			int replaced = 0;
			while ((count < 0 || replaced < count))
			{
				pos = s.find(from, pos);
				if (pos == std::string::npos)
					break;
				s.replace(pos, from.size(), to);
				pos += to.size();
				++replaced;
			}
			out = make_string(s);
			return true;
		});

		interp->register_function("starts_with", "s:string, prefix:string", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "starts_with expects (string, prefix)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::string pref = value_to_string(positional[1]);
			bool res = s.size() >= pref.size() && s.compare(0, pref.size(), pref) == 0;
			out = make_bool(res);
			return true;
		});

		interp->register_function("ends_with", "s:string, suffix:string", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "ends_with expects (string, suffix)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::string suf = value_to_string(positional[1]);
			bool res = s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
			out = make_bool(res);
			return true;
		});

		interp->register_function("find", "s:string, needle:string, start:s32", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() < 2 || positional.size() > 3)
			{
				err.has_error = true;
				err.opt_error_message = "find expects (string, needle, [start])";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::string needle = value_to_string(positional[1]);
			size_t start = 0;
			if (positional.size() == 3)
			{
				int st = static_cast<int>(as_number(positional[2]));
				if (st > 0)
					start = static_cast<size_t>(st);
			}
			size_t pos = s.find(needle, start);
			if (pos == std::string::npos)
				out = make_int(-1);
			else
				out = make_int(static_cast<s32>(pos));
			return true;
		});

		interp->register_function("ord", "s:string", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "ord expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			if (s.empty())
			{
				out = make_int(0);
				return true;
			}
			out = make_int(static_cast<s32>(static_cast<unsigned char>(s[0])));
			return true;
		});

		interp->register_function("contains", "hay:any, needle:any", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2)
			{
				err.has_error = true;
				err.opt_error_message = "contains expects (haystack, needle)";
				return true;
			}
			const auto& hay = positional[0];
			const auto& needle = positional[1];
			bool found = false;
			if (hay.type == UdonValue::Type::String)
			{
				found = value_to_string(hay).find(value_to_string(needle)) != std::string::npos;
			}
			else if (hay.type == UdonValue::Type::Array && hay.array_map)
			{
				for (const auto& kv : hay.array_map->values)
				{
					UdonValue tmp;
					if (equal_values(kv.second, needle, tmp) && tmp.s32_value)
					{
						found = true;
						break;
					}
				}
			}
			out = make_bool(found);
			return true;
		});

		interp->register_function("to_upper", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_upper expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
			{ return std::toupper(c); });
			out = make_string(s);
			return true;
		});

		interp->register_function("to_lower", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_lower expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
			{ return std::tolower(c); });
			out = make_string(s);
			return true;
		});

		interp->register_function("trim", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "trim expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);
			out = make_string(trim_string(s, true, true));
			return true;
		});

		interp->register_function("to_s32", "value:any", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_s32 expects 1 argument";
				return true;
			}
			if (is_numeric(positional[0]))
				out = make_int(static_cast<s32>(as_number(positional[0])));
			else if (positional[0].type == UdonValue::Type::String)
			{
				double d;
				bool is_int;
				if (parse_number_string(positional[0].string_value, d, is_int))
					out = make_int(static_cast<s32>(d));
				else
					out = make_int(0);
			}
			else
				out = make_int(0);
			return true;
		});

		interp->register_function("to_f32", "value:any", "f32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_f32 expects 1 argument";
				return true;
			}
			if (is_numeric(positional[0]))
				out = make_float(static_cast<f32>(as_number(positional[0])));
			else if (positional[0].type == UdonValue::Type::String)
			{
				double d;
				bool is_int;
				if (parse_number_string(positional[0].string_value, d, is_int))
					out = make_float(static_cast<f32>(d));
				else
					out = make_float(0.0f);
			}
			else
				out = make_float(0.0f);
			return true;
		});

		interp->register_function("to_string", "value:any", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_string expects 1 argument";
				return true;
			}
			out = make_string(value_to_string(positional[0]));
			return true;
		});

		interp->register_function("to_bool", "value:any", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_bool expects 1 argument";
				return true;
			}
			if (positional[0].type == UdonValue::Type::String)
			{
				bool b;
				if (parse_bool_string(positional[0].string_value, b))
					out = make_bool(b);
				else
					out = make_bool(is_truthy(positional[0]));
			}
			else
				out = make_bool(is_truthy(positional[0]));
			return true;
		});

		interp->register_function("typeof", "value:any", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "typeof expects 1 argument";
				return true;
			}
			out = make_string(value_type_name(positional[0]));
			return true;
		});

		interp->register_function("range", "start:s32, stop:s32, step:s32", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty() || positional.size() > 3)
			{
				err.has_error = true;
				err.opt_error_message = "range expects (stop) or (start, stop, [step])";
				return true;
			}
			s32 start = 0;
			s32 stop = 0;
			s32 step = 1;
			if (positional.size() == 1)
			{
				stop = positional[0].s32_value;
			}
			else
			{
				start = positional[0].s32_value;
				stop = positional[1].s32_value;
				if (positional.size() == 3)
					step = positional[2].s32_value;
			}
			if (step == 0)
				step = 1;
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();
			int idx = 0;
			if (step > 0)
			{
				for (s32 v = start; v < stop; v += step)
					array_set(out, std::to_string(idx++), make_int(v));
			}
			else
			{
				for (s32 v = start; v > stop; v += step)
					array_set(out, std::to_string(idx++), make_int(v));
			}
			return true;
		});

		static std::mt19937 rng(static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count()));
		interp->register_function("rand", "", "f32", [](UdonInterpreter*, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
		{
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);
			out = make_float(dist(rng));
			return true;
		});

		interp->register_function("push", "arr:array, value:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2 || positional[0].type != UdonValue::Type::Array)
			{
				err.has_error = true;
				err.opt_error_message = "push expects (array, value)";
				return true;
			}
			int idx = static_cast<int>(positional[0].array_map->values.size());
			array_set(const_cast<UdonValue&>(positional[0]), std::to_string(idx), positional[1]);
			out = make_none();
			return true;
		});

		interp->register_function("pop", "arr:array, key:any", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty() || positional[0].type != UdonValue::Type::Array)
			{
				err.has_error = true;
				err.opt_error_message = "pop expects (array, [key])";
				return true;
			}
			UdonValue arr = positional[0];
			std::string key;
			if (positional.size() >= 2)
				key = key_from_value(positional[1]);
			else
			{
				int max_idx = -1;
				for (const auto& kv : arr.array_map->values)
				{
					try
					{
						int k = std::stoi(kv.first);
						if (k > max_idx)
							max_idx = k;
					}
					catch (...)
					{
					}
				}
				if (max_idx >= 0)
					key = std::to_string(max_idx);
			}
			if (key.empty())
			{
				out = make_none();
				return true;
			}
			auto it = arr.array_map->values.find(key);
			if (it == arr.array_map->values.end())
			{
				out = make_none();
				return true;
			}
			out = it->second;
			arr.array_map->values.erase(it);
			return true;
		});

		interp->register_function("time", "", "s32", [](UdonInterpreter*, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
		{
			using namespace std::chrono;
			auto now = system_clock::now();
			auto secs = duration_cast<seconds>(now.time_since_epoch()).count();
			out = make_int(static_cast<s32>(secs));
			return true;
		});

		interp->register_function("to_json", "value:any", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_json expects (value)";
				return true;
			}
			out = make_string(to_json(positional[0]));
			return true;
		});

		interp->register_function("parse_json", "s:string", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "parse_json expects (string)";
				return true;
			}
			JsonParser parser(value_to_string(positional[0]));
			if (!parser.parse_value(out))
			{
				err.has_error = true;
				err.opt_error_message = "Failed to parse JSON";
			}
			return true;
		});
		interp->register_function("uri_encode", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "uri_encode expects (string)";
				return true;
			}
			out = make_string(url_encode(value_to_string(positional[0])));
			return true;
		});

		interp->register_function("uri_decode", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "uri_decode expects (string)";
				return true;
			}
			out = make_string(url_decode(value_to_string(positional[0])));
			return true;
		});

		interp->register_function("base64_encode", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "base64_encode expects (string)";
				return true;
			}
			out = make_string(base64_encode(value_to_string(positional[0])));
			return true;
		});

		interp->register_function("base64_decode", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "base64_decode expects (string)";
				return true;
			}
			out = make_string(base64_decode(value_to_string(positional[0])));
			return true;
		});

		interp->register_function("parse_formdata", "s:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "parse_formdata expects (string)";
				return true;
			}
			out = parse_form_data(value_to_string(positional[0]), interp);
			return true;
		});
	}

	bool handle_builtin(UdonInterpreter* interp,
		const std::string& name,
		const std::vector<UdonValue>& positional,
		const std::unordered_map<std::string, UdonValue>& named,
		UdonValue& out,
		CodeLocation& err)
	{
		auto it = interp->builtins.find(name);
		if (it == interp->builtins.end())
			return false;
		return it->second.function(interp, positional, named, out, err);
	}

} // namespace udon_script_builtins
