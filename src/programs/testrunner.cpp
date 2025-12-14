#include "core/udonscript.h"
#include "core/udonscript2.h"
#include "core/helpers.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <future>
#include <chrono>
#include <thread>
#include <cstring>

struct TestCase
{
	std::string name;
	std::string script_path;
	std::string expected_output;
	bool should_fail;
};

std::string load_file(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return "";
	std::ostringstream ss;
	ss << file.rdbuf();
	return ss.str();
}

bool file_exists(const std::string& path)
{
	struct stat buffer;
	return (stat(path.c_str(), &buffer) == 0);
}

std::vector<std::string> list_files(const std::string& directory, const std::string& extension)
{
	std::vector<std::string> files;
	DIR* dir = opendir(directory.c_str());
	if (!dir)
		return files;

	struct dirent* entry;
	while ((entry = readdir(dir)) != nullptr)
	{
		std::string name = entry->d_name;
		if (name.size() > extension.size() &&
			name.substr(name.size() - extension.size()) == extension)
		{
			files.push_back(name);
		}
	}
	closedir(dir);

	std::sort(files.begin(), files.end());
	return files;
}

bool run_test(const TestCase& test, bool dump_us2, std::string& actual_output, std::string& error_msg, std::chrono::milliseconds timeout)
{
	auto task = [&]() -> bool
	{
		std::ostringstream captured;
		std::streambuf* old_cout = std::cout.rdbuf(captured.rdbuf());

		UdonInterpreter interp;

		std::string script = load_file(test.script_path);
		if (script.empty())
		{
			std::cout.rdbuf(old_cout);
			error_msg = "Failed to load script";
			return false;
		}

		CodeLocation compile_result = interp.compile(script);

		if (compile_result.has_error)
		{
			std::cout.rdbuf(old_cout);
			if (test.should_fail)
			{
				actual_output = "COMPILE_ERROR";
				return true;
			}
			error_msg = "Compilation error: " + compile_result.opt_error_message;
			return false;
		}

		UdonValue return_value;
		CodeLocation run_result = interp.run_us2("main", {}, return_value);

		if (dump_us2)
		{
			std::cout.rdbuf(old_cout);
			UdonInterpreter2 vm;
			CodeLocation err{};
			if (vm.load_from_host(&interp, err))
			{
				auto it = vm.functions.find("main");
				if (it != vm.functions.end())
					std::cout << dump_us2_function(it->second) << "\n";
			}
			std::cout.rdbuf(captured.rdbuf());
		}

		std::cout.rdbuf(old_cout);

		if (run_result.has_error)
		{
			if (test.should_fail)
			{
				actual_output = "RUNTIME_ERROR";
				return true;
			}
			error_msg = "Runtime error: " + run_result.opt_error_message;
			return false;
		}

		actual_output = captured.str();

		while (!actual_output.empty() && (actual_output.back() == '\n' || actual_output.back() == '\r' || actual_output.back() == ' '))
			actual_output.pop_back();

		return true;
	};

	std::packaged_task<bool()> pt(task);
	auto fut = pt.get_future();
	std::thread t(std::move(pt));
	if (fut.wait_for(timeout) == std::future_status::timeout)
	{
		error_msg = "Timeout";
		t.detach();
		return false;
	}
	bool ok = fut.get();
	t.join();
	return ok;
}

int main(int argc, char* argv[])
{
	std::string test_dir = "scripts/testsuite";
	bool dump_us2 = false;
	std::chrono::milliseconds timeout(5000);

	for (int i = 1; i < argc; ++i)
	{
		std::string arg = argv[i];
		if (arg == "--dump-us2")
		{
			dump_us2 = true;
			continue;
		}
		if (arg.rfind("--timeout=", 0) == 0)
		{
			int ms = std::stoi(arg.substr(strlen("--timeout=")));
			if (ms > 0)
				timeout = std::chrono::milliseconds(ms);
			continue;
		}
		test_dir = arg;
	}

	std::ofstream report_file("tmp/testsuite.report");

	std::cout << "UdonScript Test Runner\n";
	std::cout << "======================\n";
	std::cout << "Test directory: " << test_dir << "\n\n";
	std::cout << "VM: us2\n";
	if (dump_us2)
		std::cout << "Dumping US2 disassembly for main() when available\n";
	std::cout << "\n";

	std::vector<std::string> test_files = list_files(test_dir, ".udon");

	if (test_files.empty())
	{
		std::cerr << "No test files found in " << test_dir << "\n";
		return 1;
	}

	std::vector<TestCase> tests;

	for (const auto& filename : test_files)
	{
		TestCase test;
		test.name = filename.substr(0, filename.size() - 5); // Remove .udon
		test.script_path = test_dir + "/" + filename;
		test.should_fail = (test.name.find("fail_") == 0);

		std::string expected_path = test_dir + "/" + test.name + ".expected";
		if (file_exists(expected_path))
		{
			test.expected_output = load_file(expected_path);
			while (!test.expected_output.empty() &&
				   (test.expected_output.back() == '\n' ||
					   test.expected_output.back() == '\r' ||
					   test.expected_output.back() == ' '))
				test.expected_output.pop_back();
		}

		tests.push_back(test);
	}

	int passed = 0;
	int failed = 0;
	std::vector<std::string> failed_tests;

	for (const auto& test : tests)
	{
		std::string actual_output;
		std::string error_msg;

		bool ran_ok = run_test(test, dump_us2, actual_output, error_msg, timeout);

		if (!ran_ok)
		{
			std::cout << "[FAIL] " << test.name << "\n";
			report_file << "=== " << test.name << " ===\n";
			report_file << "ERROR: " << error_msg << "\n\n";
			failed++;
			failed_tests.push_back(test.name);
		}
		else if (test.expected_output.empty())
		{
			std::cout << "[PASS] " << test.name << "\n";
			passed++;
		}
		else if (actual_output == test.expected_output)
		{
			std::cout << "[PASS] " << test.name << "\n";
			passed++;
		}
		else
		{
			std::cout << "[FAIL] " << test.name << "\n";
			report_file << "=== " << test.name << " ===\n";
			report_file << "Expected:\n"
						<< test.expected_output << "\n\n";
			report_file << "Got:\n"
						<< actual_output << "\n\n";
			failed++;
			failed_tests.push_back(test.name);
		}
	}

	std::cout << "\n";
	std::cout << "======================\n";
	std::cout << "Results: " << passed << " passed, " << failed << " failed out of " << tests.size() << " tests\n";

	if (!failed_tests.empty())
	{
		std::cout << "\nFailed tests:\n";
		for (const auto& name : failed_tests)
			std::cout << "  - " << name << "\n";
		std::cout << "\nSee tmp/testsuite.report for details\n";
	}

	report_file.close();

	return (failed == 0) ? 0 : 1;
}
#include <future>
#include <chrono>
