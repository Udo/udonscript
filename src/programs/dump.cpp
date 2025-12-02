#include "core/udonscript.h"
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "Usage: dump <script.udon>" << std::endl;
		return 1;
	}

	std::ifstream in(argv[1]);
	if (!in)
	{
		std::cerr << "Could not open file: " << argv[1] << std::endl;
		return 1;
	}

	std::stringstream buffer;
	buffer << in.rdbuf();
	const std::string source = buffer.str();

	UdonInterpreter interp;
	CodeLocation res = interp.compile(source);
	if (res.has_error)
	{
		std::cerr << "Compile error at line " << res.line << ": " << res.opt_error_message << std::endl;
		return 1;
	}

	std::cout << interp.dump_instructions();
	return 0;
}
