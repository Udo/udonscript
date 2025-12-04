#include "udonscript.h"
#include "helpers.h"
#include <cctype>
#include <iostream>
#include <sstream>
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
#include "tokenizer.hpp"

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

struct FunctionBinding;
static bool populate_from_managed(UdonInterpreter* interp, UdonValue::ManagedFunction* fn_obj, FunctionBinding& out_binding);
static bool resolve_function_by_name(UdonInterpreter* interp, const std::string& name, UdonValue& out_fn);

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

struct ValueStack
{
	explicit ValueStack(CodeLocation& err_ref) : err(err_ref) {}

	void push(const UdonValue& v)
	{
		values.push_back(v);
	}

	bool pop(UdonValue& out)
	{
		if (values.empty())
		{
			err.has_error = true;
			err.opt_error_message = "Stack underflow";
			return false;
		}
		out = values.back();
		values.pop_back();
		return true;
	}

	bool pop_two(UdonValue& lhs, UdonValue& rhs)
	{
		if (values.size() < 2)
		{
			err.has_error = true;
			err.opt_error_message = "Stack underflow";
			return false;
		}
		UDON_ASSERT(values.size() >= 2);
		rhs = values.back();
		values.pop_back();
		lhs = values.back();
		values.pop_back();
		return true;
	}

	bool empty() const
	{
		return values.empty();
	}
	size_t size() const
	{
		return values.size();
	}
	const UdonValue& peek() const
	{
		UDON_ASSERT(!values.empty());
		return values.back();
	}
	std::vector<UdonValue>& storage()
	{
		return values;
	}
	const std::vector<UdonValue>& storage() const
	{
		return values;
	}

	CodeLocation& err;
	std::vector<UdonValue> values;
};

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
		s64 idx = static_cast<s64>(as_number(index));
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

	ValueStack eval_stack(ok);
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
	} root_guard(interp, &env_stack, &eval_stack.storage());

	auto pop_checked = [&](UdonValue& out) -> bool
	{
		return eval_stack.pop(out);
	};

	auto pop_two = [&](UdonValue& lhs, UdonValue& rhs) -> bool
	{
		return eval_stack.pop_two(lhs, rhs);
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
		eval_stack.push(result);
		return true;
	};

	size_t ip = 0;
	u32 current_line = 0;
	u32 current_col = 0;
	size_t steps_since_gc = 0;
	const size_t gc_step_budget = 1'000'000;
	const s64 gc_time_budget_ms = 1000;
	auto last_gc_time = std::chrono::steady_clock::now();
	auto maybe_collect_periodic = [&]()
	{
		++steps_since_gc;
		if (steps_since_gc % 1000 != 0)
			return;
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gc_time).count();
		if (steps_since_gc >= gc_step_budget || elapsed_ms >= gc_time_budget_ms)
		{
			steps_since_gc = 0;
			last_gc_time = now;
			interp->collect_garbage(&env_stack, &eval_stack.storage(), 10);
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
					eval_stack.push(instr.operands[0]);
				break;
			case UdonInstruction::OpCode::ENTER_SCOPE:
			{
				s32 slot_count = (!instr.operands.empty()) ? instr.operands[0].int_value : 0;
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
				const s32 depth = instr.operands.size() > 0 ? instr.operands[0].int_value : 0;
				const s32 slot = instr.operands.size() > 1 ? instr.operands[1].int_value : 0;
				UdonValue v{};
				if (!load_slot(depth, slot, v))
					return ok;
				eval_stack.push(v);
				break;
			}
			case UdonInstruction::OpCode::STORE_LOCAL:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
					return ok;
				const s32 depth = instr.operands.size() > 0 ? instr.operands[0].int_value : 0;
				const s32 slot = instr.operands.size() > 1 ? instr.operands[1].int_value : 0;
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
					eval_stack.push(git->second);
				else
					eval_stack.push(make_none());
				break;
			}
			case UdonInstruction::OpCode::STORE_GLOBAL:
			case UdonInstruction::OpCode::STORE_VAR:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
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
					if (v.type == UdonValue::Type::Int)
						v.int_value = -v.int_value;
					else
						v.float_value = -v.float_value;
				}
				else
				{
					fail("Cannot negate value");
					return ok;
				}
				eval_stack.push(v);
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
					if (!eval_stack.pop(idx))
						return ok;
					if (!eval_stack.pop(obj))
						return ok;
					success = get_index_value(obj, idx, prop);
				}
				else
				{
					UdonValue obj;
					if (!eval_stack.pop(obj))
						return ok;
					success = get_property_value(obj, name, prop);
				}
				if (!success)
				{
					ok.has_error = true;
					ok.opt_error_message = "Invalid property access '" + name + "'";
					return ok;
				}
				eval_stack.push(prop);
				break;
			}
			case UdonInstruction::OpCode::STORE_PROP:
			{
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				UdonValue value;
				if (!eval_stack.pop(value))
					return ok;

				if (name == "[index]")
				{
					UdonValue idx;
					UdonValue obj;
					if (!eval_stack.pop(idx))
						return ok;
					if (!eval_stack.pop(obj))
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
					if (!eval_stack.pop(obj))
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
						result.int_value = result.int_value ? 0 : 1;
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
				eval_stack.push(result);
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
				ip = static_cast<size_t>(instr.operands[0].int_value);
				maybe_collect_periodic();
				continue;
			}
			case UdonInstruction::OpCode::JUMP_IF_FALSE:
			{
				UdonValue cond;
				if (!eval_stack.pop(cond))
					return ok;
				if (!is_truthy(cond))
				{
					if (instr.operands.empty())
					{
						ok.has_error = true;
						ok.opt_error_message = "Malformed JUMP_IF_FALSE";
						return ok;
					}
					ip = static_cast<size_t>(instr.operands[0].int_value);
					maybe_collect_periodic();
					continue;
				}
				break;
			}
			case UdonInstruction::OpCode::TO_BOOL:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
					return ok;
				eval_stack.push(make_bool(is_truthy(v)));
				break;
			}
			case UdonInstruction::OpCode::LOGICAL_NOT:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
					return ok;
				eval_stack.push(make_bool(!is_truthy(v)));
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
				auto code_it = interp->instructions.find(fn_name);
				if (code_it != interp->instructions.end())
					fn_obj->code_ptr = code_it->second;
				auto param_it = interp->function_params.find(fn_name);
				if (param_it != interp->function_params.end())
					fn_obj->param_ptr = param_it->second;
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
				eval_stack.push(v);
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
				const s32 arg_count = instr.operands[1].int_value;
				std::vector<std::string> arg_names;
				for (size_t i = 2; i < instr.operands.size(); ++i)
					arg_names.push_back(instr.operands[i].string_value);

				std::vector<UdonValue> call_args(static_cast<size_t>(arg_count));
				ScopedRoot call_arg_root(interp, &call_args);
				std::vector<std::string> names(static_cast<size_t>(arg_count));
				for (s32 idx = arg_count - 1; idx >= 0; --idx)
				{
					UdonValue v{};
					if (!eval_stack.pop(v))
						return ok;
					call_args[static_cast<size_t>(idx)] = v;
					if (static_cast<size_t>(idx) < arg_names.size())
						names[static_cast<size_t>(idx)] = arg_names[static_cast<size_t>(idx)];
				}

				std::vector<UdonValue> positional;
				ScopedRoot positional_root(interp, &positional);
				std::unordered_map<std::string, UdonValue> named;
				ScopedRoot named_root(interp);
				for (size_t i = 0; i < call_args.size(); ++i)
				{
					if (!names[i].empty())
					{
						auto& dest = named[names[i]];
						dest = call_args[i];
						named_root.add(dest);
					}
					else
						positional.push_back(call_args[i]);
				}

				UdonValue call_result;
				CodeLocation inner_err{};
				inner_err.has_error = false;

				auto call_closure = [&](const UdonValue& fn_val) -> bool
				{
					if (fn_val.type != UdonValue::Type::Function || !fn_val.function)
						return false;
					CodeLocation nested = interp->invoke_function(fn_val, positional, named, call_result);
					if (nested.has_error)
						inner_err = nested;
					return true;
				};

				if (callee.empty())
				{
					UdonValue callable;
					if (!eval_stack.pop(callable))
						return ok;
					if (!call_closure(callable))
					{
						ok.has_error = true;
						ok.opt_error_message = "Value is not callable";
						return ok;
					}
					eval_stack.push(call_result);
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
					UdonValue fn_val;
					if (resolve_function_by_name(interp, callee, fn_val))
					{
						CodeLocation nested = interp->invoke_function(fn_val, positional, named, call_result);
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

				eval_stack.push(call_result);
				break;
			}
			case UdonInstruction::OpCode::RETURN:
			{
				return_value = eval_stack.empty() ? make_none() : eval_stack.peek();
				return ok;
			}
			case UdonInstruction::OpCode::POP:
			{
				UdonValue tmp;
				if (!eval_stack.pop(tmp))
					return ok;
				break;
			}
			case UdonInstruction::OpCode::NOP:
			case UdonInstruction::OpCode::HALT:
				return_value = make_none();
				return ok;
		}
		maybe_collect_periodic();
		++ip;
	}

	return_value = make_none();
	return ok;
}

UdonInterpreter::UdonInterpreter()
{
	register_builtins(this);
}

struct FunctionBinding
{
	std::shared_ptr<std::vector<UdonInstruction>> code;
	std::shared_ptr<std::vector<std::string>> params;
	std::string variadic_param;
	UdonEnvironment* captured_env = nullptr;
	size_t root_scope_size = 0;
	std::shared_ptr<std::vector<s32>> param_slots;
	s32 variadic_slot = -1;
};

static bool populate_from_managed(UdonInterpreter* interp, UdonValue::ManagedFunction* fn_obj, FunctionBinding& out_binding)
{
	if (!fn_obj)
		return false;
	if (!fn_obj->code_ptr || !fn_obj->param_ptr)
	{
		auto pit = interp->function_params.find(fn_obj->function_name);
		if (pit != interp->function_params.end())
			fn_obj->param_ptr = pit->second;
		auto cit = interp->instructions.find(fn_obj->function_name);
		if (cit != interp->instructions.end())
			fn_obj->code_ptr = cit->second;
	}
	if (!fn_obj->code_ptr || !fn_obj->param_ptr)
		return false;
	if (fn_obj->root_scope_size == 0)
	{
		auto ss_it = interp->function_scope_sizes.find(fn_obj->function_name);
		if (ss_it != interp->function_scope_sizes.end())
			fn_obj->root_scope_size = ss_it->second;
	}
	if (!fn_obj->param_slots || fn_obj->param_slots->empty())
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

	out_binding.code = fn_obj->code_ptr;
	out_binding.params = fn_obj->param_ptr;
	out_binding.variadic_param = fn_obj->variadic_param;
	out_binding.captured_env = fn_obj->captured_env;
	out_binding.root_scope_size = fn_obj->root_scope_size;
	out_binding.param_slots = fn_obj->param_slots;
	out_binding.variadic_slot = fn_obj->variadic_slot;
	return true;
}

static bool resolve_function_by_name(UdonInterpreter* interp, const std::string& name, UdonValue& out_fn)
{
	auto fn_it = interp->instructions.find(name);
	if (fn_it == interp->instructions.end() || !fn_it->second)
		return false;
	UDON_ASSERT(fn_it->second != nullptr);

	UdonValue fn_val;
	fn_val.type = UdonValue::Type::Function;
	fn_val.function = interp->allocate_function();
	UDON_ASSERT(fn_val.function != nullptr);
	fn_val.function->function_name = name;
	fn_val.function->code_ptr = fn_it->second;
	FunctionBinding tmp{};
	populate_from_managed(interp, fn_val.function, tmp); // populate missing fields

	auto pit = interp->function_params.find(name);
	if (pit != interp->function_params.end())
		fn_val.function->param_ptr = pit->second;
	auto ps_it = interp->function_param_slots.find(name);
	if (ps_it != interp->function_param_slots.end())
		fn_val.function->param_slots = ps_it->second;
	auto ss_it = interp->function_scope_sizes.find(name);
	if (ss_it != interp->function_scope_sizes.end())
		fn_val.function->root_scope_size = ss_it->second;
	auto vs_it = interp->function_variadic_slot.find(name);
	fn_val.function->variadic_slot = (vs_it != interp->function_variadic_slot.end()) ? vs_it->second : -1;
	auto vit = interp->function_variadic.find(name);
	if (vit != interp->function_variadic.end())
		fn_val.function->variadic_param = vit->second;

	out_fn = fn_val;
	return true;
}

CodeLocation UdonInterpreter::invoke_function(const UdonValue& fn,
	const std::vector<UdonValue>& positional,
	const std::unordered_map<std::string, UdonValue>& named,
	UdonValue& out)
{
	CodeLocation err{};
	err.has_error = false;

	if (fn.type != UdonValue::Type::Function || !fn.function)
	{
		err.has_error = true;
		err.opt_error_message = "Value is not callable";
		return err;
	}
	UDON_ASSERT(fn.function != nullptr);

	if (fn.function->native_handler)
	{
		fn.function->native_handler(this, positional, named, out, err);
		return err;
	}

	FunctionBinding binding;
	if (!populate_from_managed(this, fn.function, binding))
	{
		err.has_error = true;
		err.opt_error_message = "Function '" + fn.function->function_name + "' not found";
		return err;
	}
	UDON_ASSERT(binding.code && binding.params);

	static const std::vector<s32> empty_slots;
	err = execute_function(this,
		*binding.code,
		*binding.params,
		binding.variadic_param,
		binding.captured_env,
		binding.root_scope_size,
		binding.param_slots ? *binding.param_slots : empty_slots,
		binding.variadic_slot,
		positional,
		named,
		out);
	return err;
}

UdonInterpreter::~UdonInterpreter()
{
	for (void* h : dl_handles)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (h)
			dlclose(h);
#else
		(void)h;
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
	return tokenize_source(source_code, context_info);
}

void UdonInterpreter::seed_builtin_globals()
{
	declared_globals.insert("context"); // will be filled with context info at runtime
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
	context_info.clear();
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

	auto populate_context_global = [&]()
	{
		UdonValue ctx{};
		ctx.type = UdonValue::Type::Array;
		ctx.array_map = allocate_array();
		for (const auto& pair : context_info)
		{
			UdonValue arr{};
			arr.type = UdonValue::Type::Array;
			arr.array_map = allocate_array();
			s32 index = 0;
			for (const auto& line : pair.second)
				array_set(arr, std::to_string(index++), make_string(line));
			array_set(ctx, pair.first, arr);
		}
		globals["context"] = ctx;
	};
	populate_context_global();

	if (!module_global_init.empty())
	{
		std::string init_fn = "__globals_init_" + std::to_string(global_init_counter++);
		instructions[init_fn] = std::make_shared<std::vector<UdonInstruction>>(module_global_init);
		function_params[init_fn] = std::make_shared<std::vector<std::string>>();
		function_param_slots[init_fn] = std::make_shared<std::vector<s32>>();
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
	if (fn_it == instructions.end() || !fn_it->second)
	{
		ok.has_error = true;
		ok.opt_error_message = "Function '" + function_name + "' not found";
		return ok;
	}

	auto param_it = function_params.find(function_name);
	std::vector<std::string> param_names = (param_it != function_params.end() && param_it->second)
											   ? *param_it->second
											   : std::vector<std::string>();
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
	if (slot_it != function_param_slots.end() && slot_it->second)
		param_slot_lookup = *slot_it->second;
	s32 variadic_slot = -1;
	auto vs_it = function_variadic_slot.find(function_name);
	if (vs_it != function_variadic_slot.end())
		variadic_slot = vs_it->second;

	return execute_function(this,
		*fn_it->second,
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
#else
		(void)h;
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
		if (pit != function_params.end() && pit->second)
		{
			for (size_t i = 0; i < pit->second->size(); ++i)
			{
				if (i)
					ss << ", ";
				ss << (*(pit->second))[i];
			}
		}
		ss << ")\n";

		if (!fn.second)
			continue;
		const auto& body = *fn.second;
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
					ss << "LOAD_LOCAL depth=" << (instr.operands.size() > 0 ? instr.operands[0].int_value : -1)
					   << " slot=" << (instr.operands.size() > 1 ? instr.operands[1].int_value : -1);
					break;
				case UdonInstruction::OpCode::STORE_LOCAL:
					ss << "STORE_LOCAL depth=" << (instr.operands.size() > 0 ? instr.operands[0].int_value : -1)
					   << " slot=" << (instr.operands.size() > 1 ? instr.operands[1].int_value : -1);
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
					ss << "ENTER_SCOPE slots=" << (instr.operands.empty() ? 0 : instr.operands[0].int_value);
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
					ss << "JUMP " << (instr.operands.empty() ? -1 : instr.operands[0].int_value);
					break;
				case UdonInstruction::OpCode::JUMP_IF_FALSE:
					ss << "JZ " << (instr.operands.empty() ? -1 : instr.operands[0].int_value);
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
					s32 argc = instr.operands.size() > 1 ? instr.operands[1].int_value : 0;
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
		auto* entry = v.array_map->head;
		while (entry)
		{
			mark_value(entry->value);
			entry = entry->next;
		}
		return;
	}
	if (v.type == UdonValue::Type::Function && v.function)
	{
		if (v.function->marked)
			return;
		v.function->marked = true;
		for (const auto& rooted : v.function->rooted_values)
			mark_value(rooted);
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
