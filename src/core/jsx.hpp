#pragma once

#include "udonscript.h"
#include <memory>
#include <string>
#include <unordered_map>

struct JsxTemplate;

std::shared_ptr<JsxTemplate> jsx_compile(const std::string& source, std::string& error);
std::string jsx_render(const JsxTemplate& tmpl,
	const std::unordered_map<std::string, UdonValue>& props,
	const std::unordered_map<std::string, UdonValue>& components,
	const std::unordered_map<std::string, UdonValue>& options,
	UdonInterpreter* interp,
	CodeLocation& err);
