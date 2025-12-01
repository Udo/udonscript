#include "core/udonscript.h"
#include <iostream>

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	std::cout << "UdonScript Hello World\n";
	std::cout << "======================\n\n";

	// Create interpreter
	UdonInterpreter interp;

	// Simple hello world script
	const char* script = R"(
function greet(name) {
    print("Hello, " + name + "!")
    return "Greeting sent to " + name
}

function main() {
    print("Welcome to UdonScript!")
    var result = greet("World")
    print(result)
    
    // Test some math
    var x = 10
    var y = 20
    var sum = x + y
    print("Sum of " + to_string(x) + " and " + to_string(y) + " is: " + to_string(sum))
    
    // Test vectors
    var v = vec3(1.0, 2.0, 3.0)
    print("Vector: " + to_string(v))
    
    // Test some math functions
    var angle = 3.14159 / 4.0  // 45 degrees in radians
    var sine = sin(angle)
    var cosine = cos(angle)
    print("sin(45°) = " + to_string(sine))
    print("cos(45°) = " + to_string(cosine))
    
}
)";

	// Compile the script
	std::cout << "Compiling script...\n";
	CodeLocation compile_result = interp.compile(script);

	if (compile_result.has_error)
	{
		std::cerr << "Compilation error at line " << compile_result.line
				  << ", column " << compile_result.column << ":\n";
		std::cerr << compile_result.opt_error_message << "\n";
		return 1;
	}

	std::cout << "Compilation successful!\n\n";
	std::cout << "Running main()...\n";
	std::cout << "-------------------\n";

	// Run the main function
	UdonValue return_value;
	std::vector<UdonValue> args;
	std::unordered_map<std::string, UdonValue> named_args;

	CodeLocation run_result = interp.run("main", args, named_args, return_value);

	if (run_result.has_error)
	{
		std::cerr << "\nRuntime error at line " << run_result.line
				  << ", column " << run_result.column << ":\n";
		std::cerr << run_result.opt_error_message << "\n";
		return 1;
	}

	std::cout << "-------------------\n";
	std::cout << "\nProgram completed successfully!\n";

	return 0;
}
