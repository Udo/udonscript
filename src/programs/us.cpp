#include "core/udonscript.h"
#include "core/udonscript_internal.h"
#include <iostream>
#include <fstream>
#include <sstream>

void print_usage(const char* program_name)
{
	std::cerr << "UdonScript Command Line Executor\n";
	std::cerr << "Usage: " << program_name << " <script_file> [entry_function]\n\n";
	std::cerr << "Arguments:\n";
	std::cerr << "  script_file      Path to the .udon script file to execute\n";
	std::cerr << "  entry_function   Function to call (default: main)\n\n";
	std::cerr << "Example:\n";
	std::cerr << "  " << program_name << " script.udon\n";
	std::cerr << "  " << program_name << " script.udon main\n";
	std::cerr << "  " << program_name << " script.udon init\n";
}

std::string load_file(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		return "";
	}

	std::ostringstream ss;
	ss << file.rdbuf();
	return ss.str();
}

int main(int argc, char* argv[])
{
	// Parse arguments
	if (argc < 2)
	{
		print_usage(argv[0]);
		return 1;
	}

	std::string script_file = argv[1];
	std::string entry_function = "main";

	if (argc >= 3)
	{
		entry_function = argv[2];
	}

	// Load script file
	std::string script_content = load_file(script_file);
	if (script_content.empty())
	{
		std::cerr << "Error: Could not read file '" << script_file << "'\n";
		std::cerr << "Make sure the file exists and is readable.\n";
		return 1;
	}

	// Create interpreter
	UdonInterpreter interp;

	// Compile script
	CodeLocation compile_result = interp.compile(script_content);

	if (compile_result.has_error)
	{
		std::cerr << "Compilation error in '" << script_file << "'\n";
		std::cerr << "  Line " << compile_result.line << ", Column " << compile_result.column << ":\n";
		std::cerr << "  " << compile_result.opt_error_message << "\n";
		return 1;
	}

	// Run entry function
	UdonValue return_value;
	CodeLocation run_result = interp.run(entry_function, {}, {}, return_value);

	if (run_result.has_error)
	{
		std::cerr << "Runtime error in '" << script_file << "'\n";
		std::cerr << "  Line " << run_result.line << ", Column " << run_result.column << ":\n";
		std::cerr << "  " << run_result.opt_error_message << "\n";
		return 1;
	}

	// Print return value if not None
	if (return_value.type != UdonValue::Type::None)
	{
		std::cout << "Return value: " << udon_script_helpers::value_to_string(return_value) << "\n";
	}

	return 0;
}
