#pragma once

#include "udonscript.h"
#include <unordered_map>
#include <vector>
#include <string>

namespace udon_script_helpers
{
	UdonValue make_none();
	UdonValue make_int(s32 v);
	UdonValue make_float(f32 v);
	UdonValue make_bool(bool v);
	UdonValue make_string(const std::string& s);
	UdonValue make_vec2(f32 x, f32 y);
	UdonValue make_vec3(f32 x, f32 y, f32 z);
	UdonValue make_vec4(f32 x, f32 y, f32 z, f32 w);
	UdonValue make_array();
	void ensure_array(UdonValue& v);
	std::string key_from_value(const UdonValue& v);
	std::string value_to_string(const UdonValue& v);
	bool is_numeric(const UdonValue& v);
	bool is_vector(const UdonValue& v);
	int vector_dimension(const UdonValue& v);
	std::string value_type_name(const UdonValue& v);
	double as_number(const UdonValue& v);
	UdonValue wrap_number(double d, const UdonValue& lhs, const UdonValue& rhs);
	UdonValue wrap_number_unary(double d, const UdonValue& src);
	bool array_get(const UdonValue& v, const std::string& key, UdonValue& out);
	void array_set(UdonValue& v, const std::string& key, const UdonValue& value);
	UdonValue bool_value(bool v);
	bool equal_values(const UdonValue& a, const UdonValue& b, UdonValue& out);
	bool compare_values(const UdonValue& a, const UdonValue& b, UdonInstruction::OpCode op, UdonValue& out);
	bool is_truthy(const UdonValue& v);
	bool add_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
	bool sub_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
	bool mul_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
	bool div_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
} // namespace udon_script_helpers

namespace udon_script_builtins
{
	void register_builtins(UdonInterpreter* interp);
	bool handle_builtin(UdonInterpreter* interp,
		const std::string& name,
		const std::vector<UdonValue>& positional,
		const std::unordered_map<std::string, UdonValue>& named,
		UdonValue& out,
		CodeLocation& err);
} // namespace udon_script_builtins

extern thread_local UdonInterpreter* g_udon_current;
