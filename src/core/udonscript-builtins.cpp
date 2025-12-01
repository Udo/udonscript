#include "udonscript_internal.h"
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

		// Array constructor
		interp->register_function("array", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
		{
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();
			return true;
		});

		// Object literal helper: __object_literal(value_n, ..., value_1, key_1, ..., key_n, count)
		// Stack: [value_n, ..., value_1] (values in reverse order of parsing)
		// Args: [key_1, ..., key_n, count]
		interp->register_function("__object_literal", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty())
			{
				err.has_error = true;
				err.opt_error_message = "__object_literal: internal error - no arguments";
				return true;
			}

			// Last argument is the count
			if (positional.back().type != UdonValue::Type::S32)
			{
				err.has_error = true;
				err.opt_error_message = "__object_literal: internal error - invalid count";
				return true;
			}

			s32 count = positional.back().s32_value;

			// We should have: count values + count keys + 1 count = count*2 + 1 args
			if (positional.size() != static_cast<size_t>(count * 2 + 1))
			{
				err.has_error = true;
				err.opt_error_message = "__object_literal: internal error - arg count mismatch";
				return true;
			}

			// Create array
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();

			// Keys are at positions [count, count+1, ..., count*2-1]
			// Values are at positions [0, 1, ..., count-1]
			for (s32 i = 0; i < count; i++)
			{
				const UdonValue& key = positional[count + i];
				const UdonValue& value = positional[i];

				// Convert key to string using the helper
				std::string key_str = key_from_value(key);

				// Set property using the helper
				array_set(out, key_str, value);
			}

			return true;
		});

		// Print function
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

		// Array helper functions for foreach
		interp->register_function("array_keys", "arr:array", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty() || positional[0].type != UdonValue::Type::Array)
			{
				err.has_error = true;
				err.opt_error_message = "array_keys expects an array";
				return true;
			}

			// Create array of keys
			out.type = UdonValue::Type::Array;
			out.array_map = interp->allocate_array();

			int idx = 0;
			for (const auto& kv : positional[0].array_map->values)
			{
				std::string idx_str = std::to_string(idx);
				array_set(out, idx_str, make_string(kv.first));
				idx++;
			}

			return true;
		});

		interp->register_function("array_len", "arr:array", "s32", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty() || positional[0].type != UdonValue::Type::Array)
			{
				err.has_error = true;
				err.opt_error_message = "array_len expects an array";
				return true;
			}

			out = make_int(static_cast<s32>(positional[0].array_map->values.size()));
			return true;
		});

		interp->register_function("array_get", "arr:array, key:any", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() < 2 || positional[0].type != UdonValue::Type::Array)
			{
				err.has_error = true;
				err.opt_error_message = "array_get expects (array, key)";
				return true;
			}

			std::string key_str = key_from_value(positional[1]);
			if (!array_get(positional[0], key_str, out))
			{
				// Key not found, return None
				out = make_none();
			}

			return true;
		});

		// File I/O functions
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

		// Shell execution
		interp->register_function("shell", "parts:any...", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.empty())
			{
				err.has_error = true;
				err.opt_error_message = "shell expects at least one argument";
				return true;
			}

			// Concatenate all arguments with spaces
			std::ostringstream command_builder;
			for (size_t i = 0; i < positional.size(); i++)
			{
				if (i > 0)
					command_builder << " ";
				command_builder << value_to_string(positional[i]);
			}
			std::string command = command_builder.str();

			// Execute command and capture output
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
			// Remove trailing newline if present
			if (!output.empty() && output.back() == '\n')
				output.pop_back();

			out = make_string(output);
			return true;
		});

		// String escape functions
		interp->register_function("to_shellarg", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1)
			{
				err.has_error = true;
				err.opt_error_message = "to_shellarg expects (string)";
				return true;
			}
			std::string s = value_to_string(positional[0]);

			// Escape single quotes by replacing ' with '\'' and wrapping in single quotes
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

			// Escape HTML special characters
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

			// Escape single quotes for SQL by doubling them
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

		// Vector constructors
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

		// Math functions
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

		// String functions
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

		// Type conversion
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
