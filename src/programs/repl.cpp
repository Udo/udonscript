#include "core/udonscript.h"
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	UdonInterpreter interp;

	std::cout << "UdonScript REPL v1.0\n";
	std::cout << "Type 'exit' or 'quit' to exit, 'help' for help\n";
	std::cout << "==============================================\n\n";

	std::string accumulated_code;
	int line_number = 1;
	bool in_function = false;
	int brace_depth = 0;

	while (true)
	{
		// Show prompt
		if (accumulated_code.empty())
			std::cout << ">>> ";
		else
			std::cout << "... ";
		std::cout.flush();

		// Read line
		std::string line;
		if (!std::getline(std::cin, line))
			break;

		// Trim whitespace from line
		size_t start = line.find_first_not_of(" \t\r\n");
		size_t end = line.find_last_not_of(" \t\r\n");
		if (start != std::string::npos && end != std::string::npos)
			line = line.substr(start, end - start + 1);
		else
			line = "";

		// Check for special commands
		if (line == "exit" || line == "quit")
		{
			std::cout << "Goodbye!\n";
			break;
		}

		if (line == "help")
		{
			std::cout << "Commands:\n";
			std::cout << "  exit, quit - Exit the REPL\n";
			std::cout << "  help       - Show this help\n";
			std::cout << "  clear      - Clear accumulated input\n";
			std::cout << "\nUsage:\n";
			std::cout << "  - Enter expressions to evaluate them\n";
			std::cout << "  - Define functions that persist across REPL sessions\n";
			std::cout << "  - Multi-line input supported (unbalanced braces continue)\n";
			std::cout << "\nExamples:\n";
			std::cout << "  >>> print(\"Hello\")\n";
			std::cout << "  >>> print(42 + 8)\n";
			std::cout << "  >>> function add(a, b) { return a + b }\n";
			std::cout << "  >>> print(add(5, 3))\n";
			std::cout << "  >>> print(typeof(3.14))\n";
			std::cout << "\nNote: Local variables don't persist between statements.\n";
			std::cout << "      Use functions to maintain state.\n";
			continue;
		}

		if (line == "clear")
		{
			accumulated_code.clear();
			brace_depth = 0;
			in_function = false;
			std::cout << "Input cleared.\n";
			continue;
		}

		// Skip empty lines when not accumulating
		if (line.empty() && accumulated_code.empty())
			continue;

		// Accumulate the line
		if (!accumulated_code.empty())
			accumulated_code += "\n";
		accumulated_code += line;

		// Count braces to determine if we need more input
		for (char c : line)
		{
			if (c == '{')
				brace_depth++;
			else if (c == '}')
				brace_depth--;
		}

		// Check if this line starts a function definition
		if (line.find("function") == 0)
			in_function = true;

		// If we're inside braces or a function, continue accumulating
		if (brace_depth > 0 || (in_function && brace_depth >= 0))
			continue;

		in_function = false;

		// Try to execute the accumulated code
		std::string code_to_execute = accumulated_code;
		accumulated_code.clear();
		brace_depth = 0;

		// Check if it's a function definition
		size_t func_pos = code_to_execute.find("function");
		bool is_function = (func_pos != std::string::npos &&
							(func_pos == 0 || !std::isalnum(code_to_execute[func_pos - 1])));

		if (is_function)
		{
			// Compile function definition at top level
			CodeLocation result = interp.compile(code_to_execute);
			if (result.has_error)
			{
				std::cout << "Error: " << result.opt_error_message << "\n";
				if (result.line > 0)
					std::cout << "  at line " << result.line << ", column " << result.column << "\n";
			}
			else
			{
				std::cout << "OK\n";
			}
		}
		else
		{
			// Wrap in a function and execute
			std::string wrapper = "function __repl_eval_" + std::to_string(line_number) + "() {\n";
			wrapper += code_to_execute + "\n";
			wrapper += "}";

			CodeLocation compile_result = interp.compile(wrapper);
			if (compile_result.has_error)
			{
				std::cout << "Error: " << compile_result.opt_error_message << "\n";
				if (compile_result.line > 0)
					std::cout << "  at line " << compile_result.line << ", column " << compile_result.column << "\n";
			}
			else
			{
				// Execute the function
				UdonValue return_value;
				std::string func_name = "__repl_eval_" + std::to_string(line_number);
				CodeLocation run_result = interp.run(func_name, {}, {}, return_value);

				if (run_result.has_error)
				{
					std::cout << "Runtime error: " << run_result.opt_error_message << "\n";
					if (run_result.line > 0)
						std::cout << "  at line " << run_result.line << ", column " << run_result.column << "\n";
				}
			}

			line_number++;
		}
	}

	return 0;
}
