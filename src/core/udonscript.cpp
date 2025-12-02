#include "udonscript.h"
#include "helpers.h"
#include <cctype>
#include <iostream>
#include <sstream>
#include <stack>
#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <memory>
#include <functional>
#include <utility>
#include <chrono>
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#include "parser.h"

thread_local UdonInterpreter* g_udon_current = nullptr;

void register_builtins(UdonInterpreter* interp);

bool handle_builtin(UdonInterpreter* interp,
	const std::string& name,
	const std::vector<UdonValue>& positional,
	const std::unordered_map<std::string, UdonValue>& named,
	UdonValue& out,
	CodeLocation& err)
{
	auto it = interp->builtins.find(name);
	if (it == interp->builtins.end())
		return false;
	return it->second.function(interp, positional, named, out, err);
}

std::unordered_set<std::string> collect_top_level_globals(const std::vector<Token>& tokens)
{
	std::unordered_set<std::string> names;
	int depth = 0;
	for (size_t i = 0; i + 1 < tokens.size(); ++i)
	{
		const Token& t = tokens[i];
		if (t.type == Token::Type::Symbol)
		{
			if (t.text == "{")
				depth++;
			else if (t.text == "}")
				depth = std::max(0, depth - 1);
		}
		if (depth == 0 && t.type == Token::Type::Keyword && t.text == "var" && tokens[i + 1].type == Token::Type::Identifier)
		{
			names.insert(tokens[i + 1].text);
		}
	}
	return names;
}

bool pop_value(std::vector<UdonValue>& stack, UdonValue& out, CodeLocation& error)
{
	if (stack.empty())
	{
		error.has_error = true;
		error.opt_error_message = "Stack underflow";
		return false;
	}
	out = stack.back();
	stack.pop_back();
	return true;
}

static bool get_property_value(const UdonValue& obj, const std::string& name, UdonValue& out)
{
	if (obj.type == UdonValue::Type::Array)
	{
		if (!array_get(obj, name, out))
			out = make_none();
		return true;
	}

	out = make_none();
	return true;
}
static bool get_index_value(const UdonValue& obj, const UdonValue& index, UdonValue& out)
{
	if (obj.type == UdonValue::Type::Array)
	{
		if (!array_get(obj, key_from_value(index), out))
			out = make_none();
		return true;
	}
	if (obj.type == UdonValue::Type::String)
	{
		std::string s = obj.string_value;
		s32 idx = static_cast<s32>(as_number(index));
		if (idx >= 0 && static_cast<size_t>(idx) < s.size())
			out = make_string(std::string(1, s[static_cast<size_t>(idx)]));
		else
			out = make_none();
		return true;
	}
	if (index.type == UdonValue::Type::String)
	{
		return get_property_value(obj, index.string_value, out);
	}

	out = make_none();
	return true;
}

static CodeLocation execute_function(UdonInterpreter* interp,
	const std::vector<UdonInstruction>& code,
	const std::vector<std::string>& param_names,
	const std::string& variadic_param,
	UdonEnvironment* captured_env,
	size_t root_scope_size,
	const std::vector<s32>& param_slot_indices,
	s32 variadic_slot_index,
	std::vector<UdonValue> args,
	std::unordered_map<std::string, UdonValue> named_args,
	UdonValue& return_value)
{
	CodeLocation ok{};
	ok.has_error = false;
	ok.line = 0;
	ok.column = 0;
	const bool has_variadic = !variadic_param.empty();

	auto fail = [&](const std::string& msg) -> bool
	{
		ok.has_error = true;
		ok.opt_error_message = msg;
		return false;
	};

	for (const auto& kv : named_args)
	{
		if (std::find(param_names.begin(), param_names.end(), kv.first) == param_names.end())
		{
			ok.has_error = true;
			ok.opt_error_message = "Unknown named argument '" + kv.first + "'";
			return ok;
		}
	}

	if (!has_variadic && args.size() > param_names.size() && named_args.empty())
	{
		ok.has_error = true;
		ok.opt_error_message = "Too many positional arguments";
		return ok;
	}

	UdonEnvironment* current_env = interp->allocate_environment(root_scope_size, captured_env);
	std::vector<UdonEnvironment*> env_stack;
	env_stack.push_back(current_env);

	auto env_at_depth = [&](s32 depth) -> UdonEnvironment*
	{
		UdonEnvironment* env = current_env;
		for (s32 i = 0; i < depth && env; ++i)
			env = env->parent;
		return env;
	};

	auto load_slot = [&](s32 depth, s32 slot, UdonValue& out) -> bool
	{
		UdonEnvironment* env = env_at_depth(depth);
		if (!env || slot < 0 || static_cast<size_t>(slot) >= env->slots.size())
			return fail("Invalid variable access");
		out = env->slots[static_cast<size_t>(slot)];
		return true;
	};

	auto store_slot = [&](s32 depth, s32 slot, const UdonValue& v) -> bool
	{
		UdonEnvironment* env = env_at_depth(depth);
		if (!env || slot < 0 || static_cast<size_t>(slot) >= env->slots.size())
			return fail("Invalid variable store");
		env->slots[static_cast<size_t>(slot)] = v;
		return true;
	};

	auto param_slot_for = [&](size_t param_index) -> s32
	{
		if (param_index < param_slot_indices.size())
			return param_slot_indices[param_index];
		return static_cast<s32>(param_index);
	};

	size_t positional_index = 0;
	for (size_t i = 0; i < param_names.size(); ++i)
	{
		const std::string& name = param_names[i];
		if (has_variadic && name == variadic_param)
		{
			if (variadic_slot_index >= 0)
				store_slot(0, variadic_slot_index, make_none());
			continue;
		}
		auto nit = named_args.find(name);
		UdonValue param_value = make_none();
		if (nit != named_args.end())
		{
			param_value = nit->second;
		}
		else if (positional_index < args.size())
		{
			param_value = args[positional_index++];
		}
		if (!store_slot(0, param_slot_for(i), param_value))
			return ok;
	}

	if (has_variadic)
	{
		UdonValue vargs = make_array();
		for (size_t i = positional_index; i < args.size(); ++i)
			array_set(vargs, std::to_string(i - positional_index), args[i]);
		if (variadic_slot_index >= 0)
			store_slot(0, variadic_slot_index, vargs);
	}
	else if (!has_variadic && !named_args.empty() && args.size() > param_names.size())
	{
		ok.has_error = true;
		ok.opt_error_message = "Too many positional arguments";
		return ok;
	}

	std::vector<UdonValue> eval_stack;
	struct RootGuard
	{
		UdonInterpreter* interp_ptr;
		std::vector<UdonEnvironment*>* envs;
		std::vector<UdonValue>* values;
		RootGuard(UdonInterpreter* interp_in, std::vector<UdonEnvironment*>* env_ptr, std::vector<UdonValue>* val_ptr)
			: interp_ptr(interp_in), envs(env_ptr), values(val_ptr)
		{
			interp_ptr->active_env_roots.push_back(envs);
			interp_ptr->active_value_roots.push_back(values);
		}
		~RootGuard()
		{
			interp_ptr->active_env_roots.pop_back();
			interp_ptr->active_value_roots.pop_back();
		}
	} root_guard(interp, &env_stack, &eval_stack);

	auto pop_checked = [&](UdonValue& out) -> bool
	{
		return pop_value(eval_stack, out, ok);
	};

	auto pop_two = [&](UdonValue& lhs, UdonValue& rhs) -> bool
	{
		if (!pop_checked(rhs) || !pop_checked(lhs))
			return false;
		return true;
	};

	auto do_binary = [&](auto op_fn) -> bool
	{
		UdonValue rhs{};
		UdonValue lhs{};
		if (!pop_two(lhs, rhs))
			return false;
		UdonValue result{};
		if (!op_fn(lhs, rhs, result))
			return fail("Invalid operands for arithmetic");
		eval_stack.push_back(result);
		return true;
	};

	size_t ip = 0;
	u32 current_line = 0;
	u32 current_col = 0;
	size_t steps_since_gc = 0;
	const size_t gc_step_budget = 1'000'000;
	const auto gc_start_time = std::chrono::steady_clock::now();
	auto gc_elapsed_ms = [&]() -> u64
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - gc_start_time)
				.count());
	};
	auto push_gc_root_and_collect = [&](const UdonValue& v)
	{
		interp->stack.push_back(v);
		interp->collect_garbage(&env_stack, &eval_stack);
		interp->stack.pop_back();
	};
	auto maybe_collect_periodic = [&]()
	{
		++steps_since_gc;
		if (steps_since_gc >= gc_step_budget || gc_elapsed_ms() >= 100)
		{
			steps_since_gc = 0;
			interp->collect_garbage(&env_stack, &eval_stack, 10);
		}
	};

	while (ip < code.size())
	{
		const auto& instr = code[ip];
		current_line = instr.line;
		current_col = instr.column;
		ok.line = current_line;
		ok.column = current_col;
		switch (instr.opcode)
		{
			case UdonInstruction::OpCode::PUSH_LITERAL:
				if (!instr.operands.empty())
					eval_stack.push_back(instr.operands[0]);
				break;
			case UdonInstruction::OpCode::ENTER_SCOPE:
			{
				s32 slot_count = (!instr.operands.empty()) ? instr.operands[0].s32_value : 0;
				if (slot_count < 0)
					slot_count = 0;
				current_env = interp->allocate_environment(static_cast<size_t>(slot_count), current_env);
				env_stack.push_back(current_env);
				break;
			}
			case UdonInstruction::OpCode::EXIT_SCOPE:
			{
				if (!env_stack.empty())
					env_stack.pop_back();
				current_env = env_stack.empty() ? nullptr : env_stack.back();
				break;
			}
			case UdonInstruction::OpCode::LOAD_LOCAL:
			{
				const s32 depth = instr.operands.size() > 0 ? instr.operands[0].s32_value : 0;
				const s32 slot = instr.operands.size() > 1 ? instr.operands[1].s32_value : 0;
				UdonValue v{};
				if (!load_slot(depth, slot, v))
					return ok;
				eval_stack.push_back(v);
				break;
			}
			case UdonInstruction::OpCode::STORE_LOCAL:
			{
				UdonValue v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				const s32 depth = instr.operands.size() > 0 ? instr.operands[0].s32_value : 0;
				const s32 slot = instr.operands.size() > 1 ? instr.operands[1].s32_value : 0;
				if (!store_slot(depth, slot, v))
					return ok;
				break;
			}
			case UdonInstruction::OpCode::LOAD_GLOBAL:
			case UdonInstruction::OpCode::LOAD_VAR:
			{
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				auto git = interp->globals.find(name);
				if (git != interp->globals.end())
					eval_stack.push_back(git->second);
				else
					eval_stack.push_back(make_none());
				break;
			}
			case UdonInstruction::OpCode::STORE_GLOBAL:
			case UdonInstruction::OpCode::STORE_VAR:
			{
				UdonValue v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				interp->globals[name] = v;
				break;
			}
			case UdonInstruction::OpCode::ADD:
			case UdonInstruction::OpCode::SUB:
			case UdonInstruction::OpCode::MUL:
			case UdonInstruction::OpCode::DIV:
			case UdonInstruction::OpCode::MOD:
			{
				auto op = [&](const UdonValue& lhs, const UdonValue& rhs, UdonValue& out) -> bool
				{
					if (instr.opcode == UdonInstruction::OpCode::ADD)
						return add_values(lhs, rhs, out);
					if (instr.opcode == UdonInstruction::OpCode::SUB)
						return sub_values(lhs, rhs, out);
					if (instr.opcode == UdonInstruction::OpCode::MUL)
						return mul_values(lhs, rhs, out);
					if (instr.opcode == UdonInstruction::OpCode::DIV)
						return div_values(lhs, rhs, out);
					return mod_values(lhs, rhs, out);
				};
				if (!do_binary(op))
					return ok;
				break;
			}
			case UdonInstruction::OpCode::NEGATE:
			{
				UdonValue v{};
				if (!pop_checked(v))
					return ok;
				if (is_numeric(v))
				{
					if (v.type == UdonValue::Type::S32)
						v.s32_value = -v.s32_value;
					else
						v.f32_value = -v.f32_value;
				}
				else
				{
					fail("Cannot negate value");
					return ok;
				}
				eval_stack.push_back(v);
				break;
			}
			case UdonInstruction::OpCode::GET_PROP:
			{
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				UdonValue prop;
				bool success = false;
				if (name == "[index]")
				{
					UdonValue idx;
					UdonValue obj;
					if (!pop_value(eval_stack, idx, ok))
						return ok;
					if (!pop_value(eval_stack, obj, ok))
						return ok;
					success = get_index_value(obj, idx, prop);
				}
				else
				{
					UdonValue obj;
					if (!pop_value(eval_stack, obj, ok))
						return ok;
					success = get_property_value(obj, name, prop);
				}
				if (!success)
				{
					ok.has_error = true;
					ok.opt_error_message = "Invalid property access '" + name + "'";
					return ok;
				}
				eval_stack.push_back(prop);
				break;
			}
			case UdonInstruction::OpCode::STORE_PROP:
			{
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				UdonValue value;
				if (!pop_value(eval_stack, value, ok))
					return ok;

				if (name == "[index]")
				{
					UdonValue idx;
					UdonValue obj;
					if (!pop_value(eval_stack, idx, ok))
						return ok;
					if (!pop_value(eval_stack, obj, ok))
						return ok;

					if (obj.type != UdonValue::Type::Array)
					{
						fail("Cannot index non-array");
						return ok;
					}

					std::string key = key_from_value(idx);
					array_set(obj, key, value);
				}
				else
				{
					UdonValue obj;
					if (!pop_value(eval_stack, obj, ok))
						return ok;

					if (obj.type != UdonValue::Type::Array)
					{
						fail("Cannot set property on non-array/object");
						return ok;
					}

					array_set(obj, name, value);
				}
				break;
			}
			case UdonInstruction::OpCode::EQ:
			case UdonInstruction::OpCode::NEQ:
			case UdonInstruction::OpCode::LT:
			case UdonInstruction::OpCode::LTE:
			case UdonInstruction::OpCode::GT:
			case UdonInstruction::OpCode::GTE:
			{
				UdonValue rhs{};
				UdonValue lhs{};
				if (!pop_two(lhs, rhs))
					return ok;
				UdonValue result{};
				bool success = false;
				if (instr.opcode == UdonInstruction::OpCode::EQ || instr.opcode == UdonInstruction::OpCode::NEQ)
				{
					success = equal_values(lhs, rhs, result);
					if (instr.opcode == UdonInstruction::OpCode::NEQ)
					{
						result.s32_value = result.s32_value ? 0 : 1;
					}
				}
				else
				{
					success = compare_values(lhs, rhs, instr.opcode, result);
				}
				if (!success)
				{
					fail("Invalid operands for comparison");
					return ok;
				}
				eval_stack.push_back(result);
				break;
			}
			case UdonInstruction::OpCode::JUMP:
			{
				if (instr.operands.empty())
				{
					ok.has_error = true;
					ok.opt_error_message = "Malformed JUMP";
					return ok;
				}
				ip = static_cast<size_t>(instr.operands[0].s32_value);
				maybe_collect_periodic();
				continue;
			}
			case UdonInstruction::OpCode::JUMP_IF_FALSE:
			{
				UdonValue cond;
				if (!pop_value(eval_stack, cond, ok))
					return ok;
				if (!is_truthy(cond))
				{
					if (instr.operands.empty())
					{
						ok.has_error = true;
						ok.opt_error_message = "Malformed JUMP_IF_FALSE";
						return ok;
					}
					ip = static_cast<size_t>(instr.operands[0].s32_value);
					maybe_collect_periodic();
					continue;
				}
				break;
			}
			case UdonInstruction::OpCode::TO_BOOL:
			{
				UdonValue v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				eval_stack.push_back(make_bool(is_truthy(v)));
				break;
			}
			case UdonInstruction::OpCode::LOGICAL_NOT:
			{
				UdonValue v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				eval_stack.push_back(make_bool(!is_truthy(v)));
				break;
			}
			case UdonInstruction::OpCode::MAKE_CLOSURE:
			{
				if (instr.operands.empty())
				{
					ok.has_error = true;
					ok.opt_error_message = "Malformed MAKE_CLOSURE";
					return ok;
				}
				const std::string fn_name = instr.operands[0].string_value;
				auto* fn_obj = interp->allocate_function();
				fn_obj->function_name = fn_name;
				fn_obj->captured_env = current_env;
				fn_obj->code_ptr = &interp->instructions[fn_name];
				fn_obj->param_ptr = &interp->function_params[fn_name];
				auto ps_it = interp->function_param_slots.find(fn_name);
				if (ps_it != interp->function_param_slots.end())
					fn_obj->param_slots = ps_it->second;
				auto ss_it = interp->function_scope_sizes.find(fn_name);
				if (ss_it != interp->function_scope_sizes.end())
					fn_obj->root_scope_size = ss_it->second;
				auto vs_it = interp->function_variadic_slot.find(fn_name);
				fn_obj->variadic_slot = (vs_it != interp->function_variadic_slot.end()) ? vs_it->second : -1;
				auto var_it = interp->function_variadic.find(fn_name);
				if (var_it != interp->function_variadic.end())
					fn_obj->variadic_param = var_it->second;

				UdonValue v{};
				v.type = UdonValue::Type::Function;
				v.function = fn_obj;
				eval_stack.push_back(v);
				break;
			}
			case UdonInstruction::OpCode::CALL:
			{
				if (instr.operands.size() < 2)
				{
					ok.has_error = true;
					ok.opt_error_message = "Malformed CALL instruction";
					return ok;
				}
				const std::string callee = instr.operands[0].string_value;
				const s32 arg_count = instr.operands[1].s32_value;
				std::vector<std::string> arg_names;
				for (size_t i = 2; i < instr.operands.size(); ++i)
					arg_names.push_back(instr.operands[i].string_value);

				std::vector<UdonValue> call_args(static_cast<size_t>(arg_count));
				std::vector<std::string> names(static_cast<size_t>(arg_count));
				for (s32 idx = arg_count - 1; idx >= 0; --idx)
				{
					UdonValue v{};
					if (!pop_value(eval_stack, v, ok))
						return ok;
					call_args[static_cast<size_t>(idx)] = v;
					if (static_cast<size_t>(idx) < arg_names.size())
						names[static_cast<size_t>(idx)] = arg_names[static_cast<size_t>(idx)];
				}

				std::vector<UdonValue> positional;
				std::unordered_map<std::string, UdonValue> named;
				for (size_t i = 0; i < call_args.size(); ++i)
				{
					if (!names[i].empty())
						named[names[i]] = call_args[i];
					else
						positional.push_back(call_args[i]);
				}

				UdonValue call_result;
				CodeLocation inner_err{};
				inner_err.has_error = false;

				struct FunctionBinding
				{
					const std::vector<UdonInstruction>* code = nullptr;
					std::vector<std::string> params;
					std::string variadic_param;
					UdonEnvironment* captured_env = nullptr;
					size_t root_scope_size = 0;
					std::vector<s32> param_slots;
					s32 variadic_slot = -1;
				};

				auto populate_from_managed = [&](UdonValue::ManagedFunction* fn_obj, FunctionBinding& out) -> bool
				{
					if (!fn_obj)
						return false;
					if (fn_obj->code_ptr == nullptr || fn_obj->param_ptr == nullptr)
					{
						auto pit = interp->function_params.find(fn_obj->function_name);
						if (pit != interp->function_params.end())
							fn_obj->param_ptr = &pit->second;
						auto cit = interp->instructions.find(fn_obj->function_name);
						if (cit != interp->instructions.end())
							fn_obj->code_ptr = &cit->second;
					}
					if (!fn_obj->code_ptr || !fn_obj->param_ptr)
						return false;
					if (fn_obj->root_scope_size == 0)
					{
						auto ss_it = interp->function_scope_sizes.find(fn_obj->function_name);
						if (ss_it != interp->function_scope_sizes.end())
							fn_obj->root_scope_size = ss_it->second;
					}
					if (fn_obj->param_slots.empty())
					{
						auto ps_it = interp->function_param_slots.find(fn_obj->function_name);
						if (ps_it != interp->function_param_slots.end())
							fn_obj->param_slots = ps_it->second;
					}
					if (fn_obj->variadic_slot < 0)
					{
						auto vs_it = interp->function_variadic_slot.find(fn_obj->function_name);
						if (vs_it != interp->function_variadic_slot.end())
							fn_obj->variadic_slot = vs_it->second;
					}

					out.code = fn_obj->code_ptr;
					out.params = *fn_obj->param_ptr;
					out.variadic_param = fn_obj->variadic_param;
					out.captured_env = fn_obj->captured_env;
					out.root_scope_size = fn_obj->root_scope_size;
					out.param_slots = fn_obj->param_slots;
					out.variadic_slot = fn_obj->variadic_slot;
					return true;
				};

				auto populate_from_name = [&](const std::string& name, FunctionBinding& out) -> bool
				{
					auto fn_it = interp->instructions.find(name);
					if (fn_it == interp->instructions.end())
						return false;
					out.code = &fn_it->second;
					auto pit = interp->function_params.find(name);
					out.params = (pit != interp->function_params.end()) ? pit->second : std::vector<std::string>();
					auto vit = interp->function_variadic.find(name);
					out.variadic_param = (vit != interp->function_variadic.end()) ? vit->second : std::string();
					auto ss_it = interp->function_scope_sizes.find(name);
					out.root_scope_size = (ss_it != interp->function_scope_sizes.end()) ? ss_it->second : 0;
					auto ps_it = interp->function_param_slots.find(name);
					out.param_slots = (ps_it != interp->function_param_slots.end()) ? ps_it->second : std::vector<s32>();
					auto vs_it = interp->function_variadic_slot.find(name);
					out.variadic_slot = (vs_it != interp->function_variadic_slot.end()) ? vs_it->second : -1;
					out.captured_env = nullptr;
					return true;
				};

				auto invoke_binding = [&](const FunctionBinding& b) -> CodeLocation
				{
					return execute_function(interp,
						*b.code,
						b.params,
						b.variadic_param,
						b.captured_env,
						b.root_scope_size,
						b.param_slots,
						b.variadic_slot,
						positional,
						named,
						call_result);
				};

				auto call_closure = [&](const UdonValue& fn_val) -> bool
				{
					if (fn_val.type != UdonValue::Type::Function || !fn_val.function)
						return false;
					if (!fn_val.function->handler.empty())
					{
						if (fn_val.function->handler == "html_template")
						{
							std::string rendered;
							std::unordered_map<std::string, UdonValue> replacements;
							if (!positional.empty() && positional[0].type == UdonValue::Type::Array && positional[0].array_map)
								replacements = positional[0].array_map->values;
							const std::string& tmpl = fn_val.function->template_body;
							size_t pos = 0;
							while (pos < tmpl.size())
							{
								size_t brace = tmpl.find('{', pos);
								if (brace == std::string::npos)
								{
									rendered.append(tmpl.substr(pos));
									break;
								}
								rendered.append(tmpl.substr(pos, brace - pos));
								size_t end = tmpl.find('}', brace + 1);
								if (end == std::string::npos)
								{
									rendered.append(tmpl.substr(brace));
									break;
								}
								std::string key = tmpl.substr(brace + 1, end - brace - 1);
								auto it = replacements.find(key);
								if (it != replacements.end())
									rendered.append(value_to_string(it->second));
								pos = end + 1;
							}
							call_result = make_string(rendered);
							return true;
						}
						else if (fn_val.function->handler == "import_forward")
						{
							UdonInterpreter* sub = interp->get_imported_interpreter(fn_val.function->handler_data);
							if (!sub)
							{
								inner_err.has_error = true;
								inner_err.opt_error_message = "import_forward: invalid module";
								return true;
							}
							CodeLocation nested = sub->run(fn_val.function->template_body, positional, named, call_result);
							if (nested.has_error)
								inner_err = nested;
							return true;
						}
						else if (fn_val.function->handler == "dl_call")
						{
#if defined(__unix__) || defined(__APPLE__)
							if (positional.size() < 1)
							{
								inner_err.has_error = true;
								inner_err.opt_error_message = "dl_call expects (symbol, args...)";
								return true;
							}
							const UdonValue& symbol_val = positional[0];
							if (symbol_val.type != UdonValue::Type::String)
							{
								inner_err.has_error = true;
								inner_err.opt_error_message = "dl_call symbol must be a string";
								return true;
							}
							void* handle = interp->get_dl_handle(fn_val.function->handler_data);
							if (!handle)
							{
								inner_err.has_error = true;
								inner_err.opt_error_message = "dl_call: invalid handle";
								return true;
							}
							std::string sig_text = symbol_val.string_value;
							std::string sym_name = sig_text;
							std::vector<std::string> arg_types;
							std::string ret_type = "f32";
							auto trim = [](std::string s)
							{
								while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
									s.erase(s.begin());
								while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
									s.pop_back();
								return s;
							};
							size_t lparen = sig_text.find('(');
							size_t rparen = sig_text.find(')');
							if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen)
							{
								sym_name = trim(sig_text.substr(0, lparen));
								std::string args_sig = sig_text.substr(lparen + 1, rparen - lparen - 1);
								std::stringstream ss(args_sig);
								std::string item;
								while (std::getline(ss, item, ','))
								{
									arg_types.push_back(trim(item));
								}
								if (rparen + 1 < sig_text.size() && sig_text[rparen + 1] == ':')
								{
									ret_type = trim(sig_text.substr(rparen + 2));
								}
							}
							void* sym = dlsym(handle, sym_name.c_str());
							if (!sym)
							{
								inner_err.has_error = true;
								inner_err.opt_error_message = "dl_call: symbol not found";
								return true;
							}

							std::vector<double> args;
							if (!arg_types.empty())
							{
								if (positional.size() - 1 != arg_types.size())
								{
									inner_err.has_error = true;
									inner_err.opt_error_message = "dl_call: argument count mismatch";
									return true;
								}
								for (size_t i = 0; i < arg_types.size(); ++i)
								{
									const UdonValue& v = positional[i + 1];
									std::string t = arg_types[i];
									if (t == "s32")
									{
										if (v.type == UdonValue::Type::S32)
											args.push_back(static_cast<double>(v.s32_value));
										else if (v.type == UdonValue::Type::F32)
											args.push_back(static_cast<double>(v.f32_value));
										else
										{
											inner_err.has_error = true;
											inner_err.opt_error_message = "dl_call: expected s32 argument";
											return true;
										}
									}
									else if (t == "f32" || t == "f64" || t == "double")
									{
										if (v.type == UdonValue::Type::F32)
											args.push_back(static_cast<double>(v.f32_value));
										else if (v.type == UdonValue::Type::S32)
											args.push_back(static_cast<double>(v.s32_value));
										else
										{
											inner_err.has_error = true;
											inner_err.opt_error_message = "dl_call: expected float argument";
											return true;
										}
									}
									else
									{
										inner_err.has_error = true;
										inner_err.opt_error_message = "dl_call: unsupported argument type '" + t + "'";
										return true;
									}
								}
							}
							else
							{
								for (size_t i = 1; i < positional.size(); ++i)
								{
									const UdonValue& v = positional[i];
									if (v.type == UdonValue::Type::S32)
										args.push_back(static_cast<double>(v.s32_value));
									else if (v.type == UdonValue::Type::F32)
										args.push_back(static_cast<double>(v.f32_value));
									else
									{
										inner_err.has_error = true;
										inner_err.opt_error_message = "dl_call only supports numeric arguments";
										return true;
									}
								}
							}
							double result = 0.0;
							switch (args.size())
							{
								case 0:
									result = (reinterpret_cast<double (*)()>(sym))();
									break;
								case 1:
									result = (reinterpret_cast<double (*)(double)>(sym))(args[0]);
									break;
								case 2:
									result = (reinterpret_cast<double (*)(double, double)>(sym))(args[0], args[1]);
									break;
								case 3:
									result = (reinterpret_cast<double (*)(double, double, double)>(sym))(args[0], args[1], args[2]);
									break;
								case 4:
									result = (reinterpret_cast<double (*)(double, double, double, double)>(sym))(args[0], args[1], args[2], args[3]);
									break;
								default:
									inner_err.has_error = true;
									inner_err.opt_error_message = "dl_call supports up to 4 arguments";
									return true;
							}
							if (ret_type == "s32")
								call_result = make_int(static_cast<s32>(result));
							else
								call_result = make_float(static_cast<f32>(result));
							return true;
#else
							inner_err.has_error = true;
							inner_err.opt_error_message = "dl_call not supported on this platform";
							return true;
#endif
						}
						else if (fn_val.function->handler == "dl_close")
						{
#if defined(__unix__) || defined(__APPLE__)
							if (!interp->close_dl_handle(fn_val.function->handler_data))
							{
								inner_err.has_error = true;
								inner_err.opt_error_message = "dl_close: invalid handle";
								return true;
							}
							call_result = make_none();
							return true;
#else
							inner_err.has_error = true;
							inner_err.opt_error_message = "dl_close not supported on this platform";
							return true;
#endif
						}
						return false;
					}
					FunctionBinding binding;
					if (!populate_from_managed(fn_val.function, binding))
					{
						inner_err.has_error = true;
						inner_err.opt_error_message = "Function '" + fn_val.function->function_name + "' not found";
						return true;
					}
					CodeLocation nested = invoke_binding(binding);
					if (nested.has_error)
						inner_err = nested;
					return true;
				};

				if (callee.empty())
				{
					UdonValue callable;
					if (!pop_value(eval_stack, callable, ok))
						return ok;
					if (!call_closure(callable))
					{
						ok.has_error = true;
						ok.opt_error_message = "Value is not callable";
						return ok;
					}
					eval_stack.push_back(call_result);
					break;
				}

				bool handled = false;

				if (!positional.empty() && positional[0].type == UdonValue::Type::Array && positional[0].array_map)
				{
					UdonValue member_fn;
					if (array_get(positional[0], callee, member_fn))
					{
						if (member_fn.type != UdonValue::Type::Function || !member_fn.function)
						{
							inner_err.has_error = true;
							inner_err.opt_error_message = "Dot-call on arrays is not supported; use ':' to access properties";
							handled = true;
						}
						else
						{
							handled = true;
							inner_err.has_error = true;
							inner_err.opt_error_message = "Dot-call on arrays is not supported; use ':' to access properties";
						}
					}
				}

				if (!handled)
				{
					bool handled_builtin = handle_builtin(interp, callee, positional, named, call_result, inner_err);
					if (handled_builtin)
					{
						if (inner_err.has_error)
							return inner_err;
						handled = true;
					}
				}

				if (!handled)
				{
					FunctionBinding binding;
					if (populate_from_name(callee, binding))
					{
						CodeLocation nested = invoke_binding(binding);
						if (nested.has_error)
							return nested;
						handled = true;
					}
				}

				if (!handled)
				{
					auto git = interp->globals.find(callee);
					if (git != interp->globals.end())
						handled = call_closure(git->second);
				}

				if (!handled)
				{
					ok.has_error = true;
					ok.opt_error_message = "Function '" + callee + "' not found";
					return ok;
				}

				if (inner_err.has_error)
					return inner_err;

				eval_stack.push_back(call_result);
				break;
			}
			case UdonInstruction::OpCode::RETURN:
			{
				if (!eval_stack.empty())
				{
					return_value = eval_stack.back();
				}
				else
				{
					return_value = make_none();
				}
				push_gc_root_and_collect(return_value);
				return ok;
			}
			case UdonInstruction::OpCode::POP:
			{
				UdonValue tmp;
				if (!pop_value(eval_stack, tmp, ok))
					return ok;
				break;
			}
			case UdonInstruction::OpCode::NOP:
			case UdonInstruction::OpCode::HALT:
				return_value = make_none();
				push_gc_root_and_collect(return_value);
				return ok;
		}
		maybe_collect_periodic();
		++ip;
	}

	return_value = make_none();
	push_gc_root_and_collect(return_value);
	return ok;
}

UdonInterpreter::UdonInterpreter()
{
	register_builtins(this);
}

UdonInterpreter::~UdonInterpreter()
{
	for (void* h : dl_handles)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (h)
			dlclose(h);
#endif
	}
	dl_handles.clear();
	for (auto* env : heap_environments)
		delete env;
	for (auto* arr : heap_arrays)
		delete arr;
	for (auto* fn : heap_functions)
		delete fn;
}

UdonValue::ManagedArray* UdonInterpreter::allocate_array()
{
	auto* arr = new UdonValue::ManagedArray();
	heap_arrays.push_back(arr);
	return arr;
}

UdonValue::ManagedFunction* UdonInterpreter::allocate_function()
{
	auto* fn = new UdonValue::ManagedFunction();
	heap_functions.push_back(fn);
	return fn;
}

UdonEnvironment* UdonInterpreter::allocate_environment(size_t slot_count, UdonEnvironment* parent)
{
	auto* env = new UdonEnvironment();
	env->parent = parent;
	env->slots.resize(slot_count, make_none());
	heap_environments.push_back(env);
	return env;
}

s32 UdonInterpreter::register_dl_handle(void* handle)
{
	dl_handles.push_back(handle);
	return static_cast<s32>(dl_handles.size() - 1);
}

void* UdonInterpreter::get_dl_handle(s32 id)
{
	if (id < 0 || static_cast<size_t>(id) >= dl_handles.size())
		return nullptr;
	return dl_handles[static_cast<size_t>(id)];
}

bool UdonInterpreter::close_dl_handle(s32 id)
{
	if (id < 0 || static_cast<size_t>(id) >= dl_handles.size())
		return false;
	if (dl_handles[static_cast<size_t>(id)])
	{
#if defined(__unix__) || defined(__APPLE__)
		dlclose(dl_handles[static_cast<size_t>(id)]);
#endif
		dl_handles[static_cast<size_t>(id)] = nullptr;
		return true;
	}
	return false;
}

s32 UdonInterpreter::register_imported_interpreter(std::unique_ptr<UdonInterpreter> sub)
{
	imported_interpreters.push_back(std::move(sub));
	return static_cast<s32>(imported_interpreters.size() - 1);
}

UdonInterpreter* UdonInterpreter::get_imported_interpreter(s32 id)
{
	if (id < 0 || static_cast<size_t>(id) >= imported_interpreters.size())
		return nullptr;
	return imported_interpreters[static_cast<size_t>(id)].get();
}

void UdonInterpreter::register_function(const std::string& name,
	const std::string& arg_signature,
	const std::string& return_type,
	UdonBuiltinFunction fn)
{
	UdonBuiltinEntry entry;
	entry.arg_signature = arg_signature;
	entry.return_type = return_type;
	entry.function = fn;
	builtins[name] = entry;
}

std::vector<Token> UdonInterpreter::tokenize(const std::string& source_code)
{
	std::vector<Token> tokens;
	u32 line = 1;
	u32 column = 1;

	size_t i = 0;
	const size_t len = source_code.size();

	auto push_token = [&](Token::Type type, const std::string& text, u32 l, u32 c)
	{
		Token t{};
		t.type = type;
		t.text = text;
		t.line = l;
		t.column = c;
		tokens.push_back(t);
	};

	auto is_ident_start = [](char c)
	{
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
	};

	auto is_ident_char = [](char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	};

	while (i < len)
	{
		char c = source_code[i];
		if (c == ' ' || c == '\t' || c == '\r')
		{
			i++;
			column++;
			continue;
		}
		if (c == '\n')
		{
			i++;
			line++;
			column = 1;
			continue;
		}

		if (c == '$' && i + 1 < len && is_ident_start(source_code[i + 1]))
		{
			u32 tok_line = line;
			u32 tok_col = column;
			i++;
			column++;
			size_t start = i;
			while (i < len && is_ident_char(source_code[i]))
			{
				i++;
				column++;
			}
			std::string proc = source_code.substr(start, i - start);
			while (i < len && (source_code[i] == ' ' || source_code[i] == '\t'))
			{
				i++;
				column++;
			}

			if (i >= len)
			{
				push_token(Token::Type::Unknown, "$" + proc, tok_line, tok_col);
				continue;
			}

			auto matching = [](char open) -> char
			{
				switch (open)
				{
					case '(':
						return ')';
					case '[':
						return ']';
					case '{':
						return '}';
					case '<':
						return '>';
					default:
						return 0;
				}
			};

			char open_ch = source_code[i];
			char close_ch = matching(open_ch);
			if (!close_ch)
			{
				push_token(Token::Type::Unknown, "$" + proc, tok_line, tok_col);
				continue;
			}
			i++;
			column++;

			size_t content_start = i;
			int depth = 1;
			bool in_quote = false;
			char quote_char = 0;
			while (i < len)
			{
				char ch = source_code[i];
				if (in_quote)
				{
					if (ch == '\\' && i + 1 < len)
					{
						i += 2;
						column += 2;
						continue;
					}
					if (ch == quote_char)
						in_quote = false;
					if (ch == '\n')
					{
						line++;
						column = 1;
					}
					else
					{
						column++;
					}
					i++;
					continue;
				}
				if (ch == '"' || ch == '\'')
				{
					in_quote = true;
					quote_char = ch;
					i++;
					column++;
					continue;
				}
				if (ch == open_ch)
					depth++;
				else if (ch == close_ch)
					depth--;

				if (depth == 0)
					break;

				if (ch == '\n')
				{
					line++;
					column = 1;
				}
				else
				{
					column++;
				}
				i++;
			}

			size_t content_end = i;
			std::string content = source_code.substr(content_start, content_end - content_start);
			if (i < len && source_code[i] == close_ch)
			{
				i++;
				column++;
			}

			Token t{};
			t.type = Token::Type::Template;
			t.text = "$" + proc;
			t.template_content = content;
			t.line = tok_line;
			t.column = tok_col;
			tokens.push_back(t);
			continue;
		}

		if (c == '/' && i + 1 < len && source_code[i + 1] == '/')
		{
			i += 2;
			column += 2;
			while (i < len && source_code[i] != '\n')
			{
				i++;
				column++;
			}
			continue;
		}
		if (c == '/' && i + 1 < len && source_code[i + 1] == '*')
		{
			i += 2;
			column += 2;
			while (i + 1 < len && !(source_code[i] == '*' && source_code[i + 1] == '/'))
			{
				if (source_code[i] == '\n')
				{
					line++;
					column = 1;
				}
				else
				{
					column++;
				}
				i++;
			}
			i += 2;
			column += 2;
			continue;
		}

		if (std::isdigit(static_cast<unsigned char>(c)))
		{
			u32 tok_line = line;
			u32 tok_col = column;
			size_t start = i;
			while (i < len && (std::isdigit(static_cast<unsigned char>(source_code[i])) || source_code[i] == '.'))
			{
				i++;
				column++;
			}
			push_token(Token::Type::Number, source_code.substr(start, i - start), tok_line, tok_col);
			continue;
		}

		if (is_ident_start(c))
		{
			u32 tok_line = line;
			u32 tok_col = column;
			size_t start = i;
			while (i < len && is_ident_char(source_code[i]))
			{
				i++;
				column++;
			}
			std::string ident = source_code.substr(start, i - start);
			std::string lower = ident;
			for (auto& ch : lower)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if (lower == "function" || lower == "return" || lower == "var" || lower == "true" || lower == "false" || lower == "none" || lower == "if" || lower == "else" || lower == "while" || lower == "for" || lower == "foreach" || lower == "in" || lower == "break" || lower == "continue" || lower == "switch" || lower == "case" || lower == "default")
			{
				push_token(Token::Type::Keyword, lower, tok_line, tok_col);
			}
			else
			{
				push_token(Token::Type::Identifier, ident, tok_line, tok_col);
			}
			continue;
		}

		if (c == '"' || c == '\'')
		{
			u32 tok_line = line;
			u32 tok_col = column;
			char quote = c;
			i++;
			column++;
			std::string literal;
			while (i < len && source_code[i] != quote)
			{
				if (source_code[i] == '\\' && i + 1 < len)
				{
					char esc = source_code[i + 1];
					switch (esc)
					{
						case 'n':
							literal.push_back('\n');
							break;
						case 'r':
							literal.push_back('\r');
							break;
						case 't':
							literal.push_back('\t');
							break;
						case '0':
							literal.push_back('\0');
							break;
						case 'b':
							literal.push_back('\b');
							break;
						case 'f':
							literal.push_back('\f');
							break;
						case '\\':
							literal.push_back('\\');
							break;
						case '"':
							literal.push_back('"');
							break;
						case '\'':
							literal.push_back('\'');
							break;
						default:
							literal.push_back(esc);
							break;
					}
					i += 2;
					column += 2;
				}
				else if (source_code[i] == '\n')
				{
					literal.push_back('\n');
					i++;
					line++;
					column = 1;
				}
				else
				{
					literal.push_back(source_code[i]);
					i++;
					column++;
				}
			}
			if (i < len && source_code[i] == quote)
			{
				i++;
				column++;
			}
			push_token(Token::Type::String, literal, tok_line, tok_col);
			continue;
		}

		u32 tok_line = line;
		u32 tok_col = column;
		std::string sym(1, c);
		if (i + 1 < len)
		{
			char n = source_code[i + 1];
			if (c == '.' && i + 2 < len && source_code[i + 1] == '.' && source_code[i + 2] == '.')
			{
				sym = "...";
				i += 3;
				column += 3;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if ((c == '=' || c == '!' || c == '<' || c == '>') && n == '=')
			{
				sym = std::string() + c + "=";
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if (c == '-' && n == '>')
			{
				sym = "->";
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
			if ((c == '&' && n == '&') || (c == '|' && n == '|') || (c == '+' && n == '+') || (c == '-' && n == '-') || (c == '+' && n == '=') || (c == '-' && n == '=') || (c == '*' && n == '=') || (c == '/' && n == '='))
			{
				sym = std::string() + c + n;
				i += 2;
				column += 2;
				push_token(Token::Type::Symbol, sym, tok_line, tok_col);
				continue;
			}
		}
		push_token(Token::Type::Symbol, sym, tok_line, tok_col);
		i++;
		column++;
	}

	Token eof{};
	eof.type = Token::Type::EndOfFile;
	eof.line = line;
	eof.column = column;
	tokens.push_back(eof);
	return tokens;
}

void UdonInterpreter::seed_builtin_globals()
{
	declared_globals.insert("Entities");
	declared_globals.insert("Materials");
	declared_globals.insert("Meshes");
	declared_globals.insert("Textures");
}

CodeLocation UdonInterpreter::compile(const std::string& source_code)
{
	instructions.clear();
	function_params.clear();
	function_variadic.clear();
	function_param_slots.clear();
	function_scope_sizes.clear();
	function_variadic_slot.clear();
	event_handlers.clear();
	globals.clear();
	stack.clear();
	declared_globals.clear();
	global_init_counter = 0;
	lambda_counter = 0;
	seed_builtin_globals();
	return compile_append(source_code);
}

CodeLocation UdonInterpreter::compile_append(const std::string& source_code)
{
	seed_builtin_globals();
	std::vector<Token> toks = tokenize(source_code);
	std::vector<UdonInstruction> module_global_init;
	std::unordered_set<std::string> chunk_globals = collect_top_level_globals(toks);
	Parser parser(
		toks,
		instructions,
		function_params,
		function_variadic,
		function_param_slots,
		function_scope_sizes,
		function_variadic_slot,
		event_handlers,
		module_global_init,
		declared_globals,
		chunk_globals,
		lambda_counter);
	CodeLocation res = parser.parse();
	if (res.has_error)
		return res;
	if (!module_global_init.empty())
	{
		std::string init_fn = "__globals_init_" + std::to_string(global_init_counter++);
		instructions[init_fn] = module_global_init;
		function_params[init_fn] = {};
		function_param_slots[init_fn] = {};
		function_scope_sizes[init_fn] = 0;
		function_variadic_slot[init_fn] = -1;
		UdonValue dummy;
		CodeLocation init_res = run(init_fn, {}, {}, dummy);
		if (init_res.has_error)
			return init_res;
	}
	return res;
}

CodeLocation UdonInterpreter::run(std::string function_name,
	std::vector<UdonValue> args,
	std::unordered_map<std::string, UdonValue> named_args,
	UdonValue& return_value)
{
	struct Guard
	{
		UdonInterpreter* self;
		UdonInterpreter* prev;
		Guard(UdonInterpreter* s) : self(s), prev(g_udon_current)
		{
			g_udon_current = s;
		}
		~Guard()
		{
			g_udon_current = prev;
		}
	} guard(this);

	CodeLocation ok{};
	ok.has_error = false;

	auto fn_it = instructions.find(function_name);
	if (fn_it == instructions.end())
	{
		ok.has_error = true;
		ok.opt_error_message = "Function '" + function_name + "' not found";
		return ok;
	}

	auto param_it = function_params.find(function_name);
	std::vector<std::string> param_names = (param_it != function_params.end()) ? param_it->second : std::vector<std::string>();
	std::string variadic_param;
	auto var_it = function_variadic.find(function_name);
	if (var_it != function_variadic.end())
		variadic_param = var_it->second;

	size_t root_scope_size = 0;
	auto scope_it = function_scope_sizes.find(function_name);
	if (scope_it != function_scope_sizes.end())
		root_scope_size = scope_it->second;
	std::vector<s32> param_slot_lookup;
	auto slot_it = function_param_slots.find(function_name);
	if (slot_it != function_param_slots.end())
		param_slot_lookup = slot_it->second;
	s32 variadic_slot = -1;
	auto vs_it = function_variadic_slot.find(function_name);
	if (vs_it != function_variadic_slot.end())
		variadic_slot = vs_it->second;

	return execute_function(this,
		fn_it->second,
		param_names,
		variadic_param,
		nullptr,
		root_scope_size,
		param_slot_lookup,
		variadic_slot,
		std::move(args),
		std::move(named_args),
		return_value);
}

void UdonInterpreter::clear()
{
	for (void* h : dl_handles)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (h)
			dlclose(h);
#endif
	}
	dl_handles.clear();
	imported_interpreters.clear();
	instructions.clear();
	function_params.clear();
	function_variadic.clear();
	function_param_slots.clear();
	function_scope_sizes.clear();
	function_variadic_slot.clear();
	event_handlers.clear();
	globals.clear();
	stack.clear();
	active_env_roots.clear();
	active_value_roots.clear();
	for (auto* env : heap_environments)
		delete env;
	heap_environments.clear();
	for (auto* arr : heap_arrays)
		delete arr;
	heap_arrays.clear();
	for (auto* fn : heap_functions)
		delete fn;
	heap_functions.clear();
	gc_runs = 0;
	gc_time_ms = 0;
}

std::string UdonInterpreter::dump_instructions() const
{
	std::ostringstream ss;
	for (const auto& fn : instructions)
	{
		ss << "function " << fn.first << "(";
		auto pit = function_params.find(fn.first);
		if (pit != function_params.end())
		{
			for (size_t i = 0; i < pit->second.size(); ++i)
			{
				if (i)
					ss << ", ";
				ss << pit->second[i];
			}
		}
		ss << ")\n";

		const auto& body = fn.second;
		for (size_t i = 0; i < body.size(); ++i)
		{
			const auto& instr = body[i];
			ss << "  [" << i << "] ";
			if (instr.opcode == UdonInstruction::OpCode::PUSH_LITERAL && !instr.operands.empty())
			{
				ss << "PUSH " << value_to_string(instr.operands[0]);
				ss << "\n";
				continue;
			}
			auto print_var = [&](const std::string& label)
			{
				ss << label << " " << (instr.operands.empty() ? "<anon>" : instr.operands[0].string_value);
			};
			switch (instr.opcode)
			{
				case UdonInstruction::OpCode::PUSH_LITERAL:
					ss << "PUSH <none>";
					break;
				case UdonInstruction::OpCode::LOAD_LOCAL:
					ss << "LOAD_LOCAL depth=" << (instr.operands.size() > 0 ? instr.operands[0].s32_value : -1)
					   << " slot=" << (instr.operands.size() > 1 ? instr.operands[1].s32_value : -1);
					break;
				case UdonInstruction::OpCode::STORE_LOCAL:
					ss << "STORE_LOCAL depth=" << (instr.operands.size() > 0 ? instr.operands[0].s32_value : -1)
					   << " slot=" << (instr.operands.size() > 1 ? instr.operands[1].s32_value : -1);
					break;
				case UdonInstruction::OpCode::LOAD_GLOBAL:
					print_var("LOADG");
					break;
				case UdonInstruction::OpCode::STORE_GLOBAL:
					print_var("STOREG");
					break;
				case UdonInstruction::OpCode::LOAD_VAR:
					print_var("LOAD");
					break;
				case UdonInstruction::OpCode::STORE_VAR:
					print_var("STORE");
					break;
				case UdonInstruction::OpCode::ENTER_SCOPE:
					ss << "ENTER_SCOPE slots=" << (instr.operands.empty() ? 0 : instr.operands[0].s32_value);
					break;
				case UdonInstruction::OpCode::EXIT_SCOPE:
					ss << "EXIT_SCOPE";
					break;
				case UdonInstruction::OpCode::ADD:
					ss << "ADD";
					break;
				case UdonInstruction::OpCode::SUB:
					ss << "SUB";
					break;
				case UdonInstruction::OpCode::MUL:
					ss << "MUL";
					break;
				case UdonInstruction::OpCode::DIV:
					ss << "DIV";
					break;
				case UdonInstruction::OpCode::MOD:
					ss << "MOD";
					break;
				case UdonInstruction::OpCode::NEGATE:
					ss << "NEG";
					break;
				case UdonInstruction::OpCode::EQ:
					ss << "EQ";
					break;
				case UdonInstruction::OpCode::NEQ:
					ss << "NEQ";
					break;
				case UdonInstruction::OpCode::LT:
					ss << "LT";
					break;
				case UdonInstruction::OpCode::LTE:
					ss << "LTE";
					break;
				case UdonInstruction::OpCode::GT:
					ss << "GT";
					break;
				case UdonInstruction::OpCode::GTE:
					ss << "GTE";
					break;
				case UdonInstruction::OpCode::JUMP:
					ss << "JUMP " << (instr.operands.empty() ? -1 : instr.operands[0].s32_value);
					break;
				case UdonInstruction::OpCode::JUMP_IF_FALSE:
					ss << "JZ " << (instr.operands.empty() ? -1 : instr.operands[0].s32_value);
					break;
				case UdonInstruction::OpCode::TO_BOOL:
					ss << "TO_BOOL";
					break;
				case UdonInstruction::OpCode::LOGICAL_NOT:
					ss << "NOT";
					break;
				case UdonInstruction::OpCode::GET_PROP:
					ss << "GET_PROP " << (instr.operands.empty() ? "<name>" : instr.operands[0].string_value);
					break;
				case UdonInstruction::OpCode::STORE_PROP:
					ss << "STORE_PROP " << (instr.operands.empty() ? "<name>" : instr.operands[0].string_value);
					break;
				case UdonInstruction::OpCode::MAKE_CLOSURE:
					ss << "MAKE_CLOSURE " << (instr.operands.empty() ? "<name>" : instr.operands[0].string_value);
					break;
				case UdonInstruction::OpCode::CALL:
				{
					std::string target = instr.operands.size() > 0 ? instr.operands[0].string_value : "<anon>";
					s32 argc = instr.operands.size() > 1 ? instr.operands[1].s32_value : 0;
					ss << "CALL " << target << " argc=" << argc;
					if (instr.operands.size() > 2)
					{
						ss << " [";
						for (size_t j = 2; j < instr.operands.size(); ++j)
						{
							if (j > 2)
								ss << ", ";
							ss << instr.operands[j].string_value;
						}
						ss << "]";
					}
					break;
				}
				case UdonInstruction::OpCode::RETURN:
					ss << "RETURN";
					break;
				case UdonInstruction::OpCode::POP:
					ss << "POP";
					break;
				case UdonInstruction::OpCode::NOP:
					ss << "NOP";
					break;
				case UdonInstruction::OpCode::HALT:
					ss << "HALT";
					break;
			}
			ss << "\n";
		}
		ss << "\n";
	}
	return ss.str();
}

static void mark_value(const UdonValue& v);

static void mark_environment(UdonEnvironment* env)
{
	std::vector<UdonEnvironment*> stack_envs;
	if (env)
		stack_envs.push_back(env);
	while (!stack_envs.empty())
	{
		UdonEnvironment* current = stack_envs.back();
		stack_envs.pop_back();
		if (!current || current->marked)
			continue;
		current->marked = true;
		for (auto& slot : current->slots)
			mark_value(slot);
		if (current->parent && !current->parent->marked)
			stack_envs.push_back(current->parent);
	}
}

static void mark_value(const UdonValue& v)
{
	if (v.type == UdonValue::Type::Array && v.array_map)
	{
		if (v.array_map->marked)
			return;
		v.array_map->marked = true;
		for (auto& kv : v.array_map->values)
			mark_value(kv.second);
		return;
	}
	if (v.type == UdonValue::Type::Function && v.function)
	{
		if (v.function->marked)
			return;
		v.function->marked = true;
		mark_environment(v.function->captured_env);
		return;
	}
}

void UdonInterpreter::collect_garbage(const std::vector<UdonEnvironment*>* env_roots,
	const std::vector<UdonValue>* value_roots,
	u32 time_budget_ms)
{
	const bool has_budget = time_budget_ms > 0;
	const auto start = std::chrono::steady_clock::now();
	const auto deadline = start + std::chrono::milliseconds(time_budget_ms);
	auto time_up = [&]()
	{
		return has_budget && std::chrono::steady_clock::now() >= deadline;
	};

	for (auto* env : heap_environments)
		env->marked = false;
	for (auto* arr : heap_arrays)
		arr->marked = false;
	for (auto* fn : heap_functions)
		fn->marked = false;

	auto mark_env_roots = [&](const std::vector<UdonEnvironment*>* roots)
	{
		if (!roots)
			return;
		for (auto* env : *roots)
			mark_environment(env);
	};

	auto mark_value_roots = [&](const std::vector<UdonValue>* roots)
	{
		if (!roots)
			return;
		for (const auto& v : *roots)
			mark_value(v);
	};

	for (auto* roots : active_env_roots)
		mark_env_roots(roots);
	for (auto& kv : globals)
		mark_value(kv.second);
	for (auto& v : stack)
		mark_value(v);
	for (auto* roots : active_value_roots)
		mark_value_roots(roots);
	mark_env_roots(env_roots);
	mark_value_roots(value_roots);

	std::vector<UdonEnvironment*> live_envs;
	live_envs.reserve(heap_environments.size());
	for (size_t i = 0; i < heap_environments.size(); ++i)
	{
		auto* env = heap_environments[i];
		if (env->marked)
			live_envs.push_back(env);
		else
			delete env;
		if (time_up())
		{
			for (size_t j = i + 1; j < heap_environments.size(); ++j)
				live_envs.push_back(heap_environments[j]);
			break;
		}
	}
	heap_environments.swap(live_envs);

	std::vector<UdonValue::ManagedArray*> survivors;
	survivors.reserve(heap_arrays.size());
	for (size_t i = 0; i < heap_arrays.size(); ++i)
	{
		auto* arr = heap_arrays[i];
		if (arr->marked)
			survivors.push_back(arr);
		else
			delete arr;
		if (time_up())
		{
			for (size_t j = i + 1; j < heap_arrays.size(); ++j)
				survivors.push_back(heap_arrays[j]);
			break;
		}
	}
	heap_arrays.swap(survivors);

	std::vector<UdonValue::ManagedFunction*> live_functions;
	live_functions.reserve(heap_functions.size());
	for (size_t i = 0; i < heap_functions.size(); ++i)
	{
		auto* fn = heap_functions[i];
		if (fn->marked)
			live_functions.push_back(fn);
		else
			delete fn;
		if (time_up())
		{
			for (size_t j = i + 1; j < heap_functions.size(); ++j)
				live_functions.push_back(heap_functions[j]);
			break;
		}
	}
	heap_functions.swap(live_functions);

	const auto end = std::chrono::steady_clock::now();
	gc_time_ms += static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
	gc_runs += 1;
}
