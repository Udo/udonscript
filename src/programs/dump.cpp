#include "core/udonscript.h"
#include "core/udonscript2.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv)
{
	std::string path;
	for (int i = 1; i < argc; ++i)
	{
		path = argv[i];
	}

	if (path.empty())
	{
		std::cerr << "Usage: dump <script.udon>" << std::endl;
		return 1;
	}

	std::ifstream in(path);
	if (!in)
	{
		std::cerr << "Could not open file: " << path << std::endl;
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

	UdonInterpreter2 vm;
	CodeLocation err{};
	if (!vm.load_from_host(&interp, err))
	{
		std::cerr << "Failed to translate to US2: " << err.opt_error_message << std::endl;
		return 1;
	}

	std::vector<std::string> names;
	names.reserve(vm.functions.size());
	for (const auto& kv : vm.functions)
		names.push_back(kv.first);
	std::sort(names.begin(), names.end());

	for (const auto& name : names)
	{
		auto it = vm.functions.find(name);
		if (it == vm.functions.end())
			continue;
		std::cout << dump_us2_function(it->second) << "\n";
	}

	return 0;
}
