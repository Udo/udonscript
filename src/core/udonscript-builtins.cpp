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

		interp->register_function("array", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
		{
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();
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
