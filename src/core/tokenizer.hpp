#pragma once

#include "udonscript.h"
#include <string>
#include <unordered_map>
#include <vector>

std::vector<Token> tokenize_source(const std::string& source_code,
	std::unordered_map<std::string, std::vector<std::string>>& context_info);
