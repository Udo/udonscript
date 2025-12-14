#pragma once

#include "udonscript.h"
#include "tokenizer.hpp"

class Parser2
{
  public:
	Parser2(UdonInterpreter& interp,
		const std::vector<Token>& tokens,
		const std::unordered_set<std::string>& chunk_globals);

	CodeLocation parse();

  private:
	UdonInterpreter& interp;
	const std::vector<Token>& tokens;
	const std::unordered_set<std::string>& chunk_globals;
};
