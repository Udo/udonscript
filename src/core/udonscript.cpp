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

constexpr s32 GLOBAL_SLOT_UNKNOWN = -1;
constexpr s32 GLOBAL_SLOT_MISS = -2;

template <typename T>
struct ScratchVector
{
	std::vector<T> data;
	std::vector<std::vector<T>>* pool = nullptr;
	explicit ScratchVector(std::vector<std::vector<T>>* p, size_t size_hint = 0) : pool(p)
	{
		if (pool && !pool->empty())
		{
			data = std::move(pool->back());
			pool->pop_back();
		}
		data.clear();
		if (size_hint)
			data.resize(size_hint);
	}
	~ScratchVector()
	{
		if (pool)
		{
			data.clear();
			pool->push_back(std::move(data));
		}
	}
};

struct ScratchMap
{
	std::unordered_map<std::string, UdonValue> data;
	std::vector<std::unordered_map<std::string, UdonValue>>* pool = nullptr;
	explicit ScratchMap(std::vector<std::unordered_map<std::string, UdonValue>>* p) : pool(p)
	{
		if (pool && !pool->empty())
		{
			data = std::move(pool->back());
			pool->pop_back();
		}
		data.clear();
	}
	~ScratchMap()
	{
		if (pool)
		{
			data.clear();
			pool->push_back(std::move(data));
		}
	}
};

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
	struct EnvRootGuard
	{
		UdonInterpreter* interp_ptr;
		UdonEnvironment** env_ptr;
		EnvRootGuard(UdonInterpreter* interp_in, UdonEnvironment** env_root) : interp_ptr(interp_in), env_ptr(env_root)
		{
			if (interp_ptr)
				interp_ptr->active_env_roots.push_back(env_ptr);
		}
		~EnvRootGuard()
		{
			if (interp_ptr)
				interp_ptr->active_env_roots.pop_back();
		}
	} env_root_guard(interp, &current_env);

	struct ValueRootGuard
	{
		UdonInterpreter* interp_ptr;
		std::vector<UdonValue>* values;
		ValueRootGuard(UdonInterpreter* interp_in, std::vector<UdonValue>* val_ptr) : interp_ptr(interp_in), values(val_ptr)
		{
			if (interp_ptr)
				interp_ptr->active_value_roots.push_back(values);
		}
		~ValueRootGuard()
		{
			if (interp_ptr)
				interp_ptr->active_value_roots.pop_back();
		}
	} value_root_guard(interp, &eval_stack.storage());

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

	auto refresh_instruction_cache = [&](const std::vector<UdonInstruction>& fn_code)
	{
		for (const auto& instr : fn_code)
		{
			instr.cached_version = interp->cache_version;
			instr.cached_global_slot = GLOBAL_SLOT_UNKNOWN;
			instr.cached_kind = UdonInstruction::CachedKind::None;
			instr.cached_builtin = UdonBuiltinFunction();
			instr.cached_fn = nullptr;
			instr.cached_call_arg_names.clear();
			instr.cached_has_named_args = false;
		}
	};

	const auto* code_ptr = &code;
	auto cache_it = interp->code_cache_versions.find(code_ptr);
	if (cache_it == interp->code_cache_versions.end() || cache_it->second != interp->cache_version)
	{
		refresh_instruction_cache(code);
		interp->code_cache_versions[code_ptr] = interp->cache_version;
	}

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
			interp->collect_garbage(current_env, &eval_stack.storage(), 10);
		}
	};

	while (ip < code.size())
	{
		auto& instr = code[ip];
		current_line = instr.line;
		current_col = instr.column;
		ok.line = current_line;
		ok.column = current_col;
		switch (instr.opcode_instruction)
		{
			case Opcode::PUSH_LITERAL:
				if (!instr.operands.empty())
					eval_stack.push(instr.operands[0]);
				break;
			case Opcode::ENTER_SCOPE:
			{
				break;
			}
			case Opcode::EXIT_SCOPE:
			{
				break;
			}
			case Opcode::LOAD_LOCAL:
			{
				const s32 depth = instr.operands.size() > 0 ? instr.operands[0].int_value : 0;
				const s32 slot = instr.operands.size() > 1 ? instr.operands[1].int_value : 0;
				UdonValue v{};
				if (!load_slot(depth, slot, v))
					return ok;
				eval_stack.push(v);
				break;
			}
			case Opcode::STORE_LOCAL:
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
			case Opcode::LOAD_GLOBAL:
			case Opcode::LOAD_VAR:
			{
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				s32 slot = instr.cached_global_slot;
				if (slot == GLOBAL_SLOT_UNKNOWN || instr.cached_version != interp->cache_version)
				{
					slot = interp->get_global_slot(name);
					instr.cached_global_slot = (slot >= 0) ? slot : GLOBAL_SLOT_MISS;
				}
				UdonValue tmp{};
				if (interp->get_global_value(name, tmp, slot))
					eval_stack.push(tmp);
				else
					eval_stack.push(make_none());
				break;
			}
			case Opcode::STORE_GLOBAL:
			case Opcode::STORE_VAR:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
					return ok;
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				s32 slot = instr.cached_global_slot;
				if (slot == GLOBAL_SLOT_UNKNOWN || instr.cached_version != interp->cache_version)
				{
					slot = interp->get_global_slot(name);
					instr.cached_global_slot = (slot >= 0) ? slot : GLOBAL_SLOT_MISS;
				}
				interp->set_global_value(name, v, slot);
				break;
			}
			case Opcode::ADD:
			case Opcode::SUB:
			case Opcode::CONCAT:
			case Opcode::MUL:
			case Opcode::DIV:
			case Opcode::MOD:
			{
				auto op = [&](const UdonValue& lhs, const UdonValue& rhs, UdonValue& out) -> bool
				{
					if (instr.opcode_instruction == Opcode::CONCAT)
					{
						out = make_string(value_to_string(lhs) + value_to_string(rhs));
						return true;
					}
					if (instr.opcode_instruction == Opcode::ADD)
						return add_values(lhs, rhs, out);
					if (instr.opcode_instruction == Opcode::SUB)
						return sub_values(lhs, rhs, out);
					if (instr.opcode_instruction == Opcode::MUL)
						return mul_values(lhs, rhs, out);
					if (instr.opcode_instruction == Opcode::DIV)
						return div_values(lhs, rhs, out);
					return mod_values(lhs, rhs, out);
				};
				if (!do_binary(op))
					return ok;
				break;
			}
			case Opcode::NEGATE:
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
			case Opcode::GET_PROP:
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
			case Opcode::STORE_PROP:
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
			case Opcode::EQ:
			case Opcode::NEQ:
			case Opcode::LT:
			case Opcode::LTE:
			case Opcode::GT:
			case Opcode::GTE:
			{
				UdonValue rhs{};
				UdonValue lhs{};
				if (!pop_two(lhs, rhs))
					return ok;
				UdonValue result{};
				bool success = false;
				if (instr.opcode_instruction == Opcode::EQ || instr.opcode_instruction == Opcode::NEQ)
				{
					success = equal_values(lhs, rhs, result);
					if (instr.opcode_instruction == Opcode::NEQ)
					{
						result.int_value = result.int_value ? 0 : 1;
					}
				}
				else
				{
					success = compare_values(lhs, rhs, instr.opcode_instruction, result);
				}
				if (!success)
				{
					fail("Invalid operands for comparison");
					return ok;
				}
				eval_stack.push(result);
				break;
			}
			case Opcode::JUMP:
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
			case Opcode::JUMP_IF_FALSE:
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
			case Opcode::TO_BOOL:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
					return ok;
				eval_stack.push(make_bool(is_truthy(v)));
				break;
			}
			case Opcode::LOGICAL_NOT:
			{
				UdonValue v{};
				if (!eval_stack.pop(v))
					return ok;
				eval_stack.push(make_bool(!is_truthy(v)));
				break;
			}
			case Opcode::MAKE_CLOSURE:
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
				auto ss_it = interp->function_frame_sizes.find(fn_name);
				if (ss_it != interp->function_frame_sizes.end())
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
			case Opcode::CALL:
			{
				if (instr.operands.size() < 2)
				{
					ok.has_error = true;
					ok.opt_error_message = "Malformed CALL instruction";
					return ok;
				}
				const std::string callee = instr.operands[0].string_value;
				const s32 arg_count = instr.operands[1].int_value;
				if (instr.cached_call_arg_names.empty() && arg_count > 0)
				{
					instr.cached_call_arg_names.resize(static_cast<size_t>(arg_count));
					instr.cached_has_named_args = false;
					for (size_t i = 2; i < instr.operands.size() && (i - 2) < instr.cached_call_arg_names.size(); ++i)
					{
						const std::string& nm = instr.operands[i].string_value;
						instr.cached_call_arg_names[i - 2] = nm;
						if (!nm.empty())
							instr.cached_has_named_args = true;
					}
				}
				const auto& arg_names = instr.cached_call_arg_names;
				const bool has_named_args = instr.cached_has_named_args;

				ScratchVector<UdonValue> call_args_buf(&interp->value_buffer_pool, static_cast<size_t>(std::max<s32>(arg_count, 0)));
				auto& call_args = call_args_buf.data;
				ScopedRoot call_arg_root(interp, &call_args);
				for (s32 idx = arg_count - 1; idx >= 0; --idx)
				{
					UdonValue v{};
					if (!eval_stack.pop(v))
						return ok;
					call_args[static_cast<size_t>(idx)] = v;
				}

				ScratchVector<UdonValue> positional_buf(&interp->value_buffer_pool);
				auto& positional = positional_buf.data;
				positional.clear();
				positional.reserve(static_cast<size_t>(std::max<s32>(arg_count, 0)));

				ScratchMap named_buf(has_named_args ? &interp->map_buffer_pool : nullptr);
				auto& named = named_buf.data;
				if (has_named_args)
					named.reserve(static_cast<size_t>(std::max<s32>(arg_count, 0)));

				ScopedRoot positional_root(interp, &positional);
				ScopedRoot named_root(has_named_args ? interp : nullptr);
				for (size_t i = 0; i < call_args.size(); ++i)
				{
					if (has_named_args && i < arg_names.size() && !arg_names[i].empty())
					{
						auto& dest = named[arg_names[i]];
						dest = call_args[i];
						if (has_named_args)
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

				if (instr.cached_kind == UdonInstruction::CachedKind::Builtin)
				{
					instr.cached_builtin(interp, positional, named, call_result, inner_err);
					handled = !inner_err.has_error;
				}
				else if (instr.cached_kind == UdonInstruction::CachedKind::Function && instr.cached_fn)
				{
					UdonValue fn_val;
					fn_val.type = UdonValue::Type::Function;
					fn_val.function = instr.cached_fn;
					CodeLocation nested = interp->invoke_function(fn_val, positional, named, call_result);
					if (nested.has_error)
						inner_err = nested;
					else
						handled = true;
				}

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
						instr.cached_kind = UdonInstruction::CachedKind::Builtin;
						instr.cached_builtin = interp->builtins[callee].function;
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
						instr.cached_kind = UdonInstruction::CachedKind::Function;
						instr.cached_fn = fn_val.function;
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
			case Opcode::RETURN:
			{
				return_value = eval_stack.empty() ? make_none() : eval_stack.peek();
				return ok;
			}
			case Opcode::POP:
			{
				UdonValue tmp;
				if (!eval_stack.pop(tmp))
					return ok;
				break;
			}
			case Opcode::NOP:
			case Opcode::HALT:
				return_value = make_none();
				return ok;
		}
		maybe_collect_periodic();
		++ip;
	}

	return_value = make_none();
	return ok;
}

std::vector<std::string> OpcodeNames;

UdonInterpreter::UdonInterpreter()
{
	OpcodeNames = {
		"NOP",
		"PUSH_LITERAL",
		"LOAD_VAR",
		"STORE_VAR",
		"LOAD_LOCAL",
		"STORE_LOCAL",
		"LOAD_GLOBAL",
		"STORE_GLOBAL",
		"ENTER_SCOPE",
		"EXIT_SCOPE",
		"ADD",
		"SUB",
		"CONCAT",
		"MUL",
		"DIV",
		"MOD",
		"NEGATE",
		"EQ",
		"NEQ",
		"LT",
		"LTE",
		"GT",
		"GTE",
		"JUMP",
		"JUMP_IF_FALSE",
		"TO_BOOL",
		"LOGICAL_NOT",
		"GET_PROP",
		"STORE_PROP",
		"MAKE_CLOSURE",
		"CALL",
		"RETURN",
		"POP",
		"HALT"
	};
	register_builtins(this);
}

void UdonInterpreter::reset_state(bool release_heaps, bool release_handles)
{
	if (release_handles)
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
	}

	if (release_heaps)
	{
		for (auto* env : heap_environments)
			delete env;
		for (auto* arr : heap_arrays)
			delete arr;
		for (auto* fn : heap_functions)
			delete fn;
		heap_environments.clear();
		heap_arrays.clear();
		heap_functions.clear();
	}

	instructions.clear();
	function_params.clear();
	function_variadic.clear();
	function_param_slots.clear();
	function_frame_sizes.clear();
	function_variadic_slot.clear();
	function_cache.clear();
	event_handlers.clear();
	globals.clear();
	global_slots.clear();
	global_slot_lookup.clear();
	declared_globals.clear();
	declared_global_order.clear();
	stack.clear();
	active_env_roots.clear();
	active_value_roots.clear();
	code_cache_versions.clear();
	value_buffer_pool.clear();
	map_buffer_pool.clear();
	gc_runs = 0;
	gc_time_ms = 0;
	cache_version = 1;
	global_init_counter = 0;
	lambda_counter = 0;
	context_info.clear();
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
		auto ss_it = interp->function_frame_sizes.find(fn_obj->function_name);
		if (ss_it != interp->function_frame_sizes.end())
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
	auto cache_it = interp->function_cache.find(name);
	if (cache_it != interp->function_cache.end())
	{
		out_fn = cache_it->second;
		return true;
	}

	auto fn_it = interp->instructions.find(name);
	if (fn_it == interp->instructions.end() || !fn_it->second)
		return false;
	UDON_ASSERT(fn_it->second != nullptr);

	UdonValue fn_val;
	fn_val.type = UdonValue::Type::Function;
	fn_val.function = interp->allocate_function();
	fn_val.function->is_cache_wrapper = true;
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
	auto ss_it = interp->function_frame_sizes.find(name);
	if (ss_it != interp->function_frame_sizes.end())
		fn_val.function->root_scope_size = ss_it->second;
	auto vs_it = interp->function_variadic_slot.find(name);
	fn_val.function->variadic_slot = (vs_it != interp->function_variadic_slot.end()) ? vs_it->second : -1;
	auto vit = interp->function_variadic.find(name);
	if (vit != interp->function_variadic.end())
		fn_val.function->variadic_param = vit->second;

	out_fn = fn_val;
	interp->function_cache[name] = fn_val;
	return true;
}

CodeLocation UdonInterpreter::invoke_function(const UdonValue& fn,
	const std::vector<UdonValue>& positional,
	const std::unordered_map<std::string, UdonValue>& named,
	UdonValue& out)
{
	ScopedRoot fn_root(this);
	fn_root.add(fn);

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
	fn->magic = 0xF00DF00DCAFEBEEFULL;
	heap_functions.push_back(fn);
	return fn;
}

UdonEnvironment* UdonInterpreter::allocate_environment(size_t slot_count, UdonEnvironment* parent)
{
	auto* env = new UdonEnvironment();
	env->parent = parent;
	env->slots.assign(slot_count, UdonValue());
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
	if (declared_globals.insert("context").second) // will be filled with context info at runtime
		declared_global_order.push_back("context");
}

CodeLocation UdonInterpreter::compile(const std::string& source_code)
{
	reset_state(false, false);
	seed_builtin_globals();
	return compile_append(source_code);
}

void UdonInterpreter::rebuild_global_slots()
{
	if (global_slots.size() < declared_global_order.size())
		global_slots.resize(declared_global_order.size(), make_none());

	global_slot_lookup.clear();
	for (size_t i = 0; i < declared_global_order.size(); ++i)
	{
		global_slot_lookup[declared_global_order[i]] = static_cast<s32>(i);
		auto it = globals.find(declared_global_order[i]);
		if (it != globals.end())
			global_slots[i] = it->second;
	}
}

s32 UdonInterpreter::get_global_slot(const std::string& name) const
{
	auto it = global_slot_lookup.find(name);
	if (it == global_slot_lookup.end())
		return -1;
	return it->second;
}

bool UdonInterpreter::get_global_value(const std::string& name, UdonValue& out, s32 slot_hint) const
{
	s32 slot = (slot_hint >= 0) ? slot_hint : get_global_slot(name);
	if (slot >= 0 && static_cast<size_t>(slot) < global_slots.size())
	{
		out = global_slots[static_cast<size_t>(slot)];
		return true;
	}
	auto git = globals.find(name);
	if (git != globals.end())
	{
		out = git->second;
		return true;
	}
	return false;
}

void UdonInterpreter::set_global_value(const std::string& name, const UdonValue& v, s32 slot_hint)
{
	s32 slot = (slot_hint >= 0) ? slot_hint : get_global_slot(name);
	if (slot >= 0)
	{
		if (global_slots.size() <= static_cast<size_t>(slot))
			global_slots.resize(static_cast<size_t>(slot) + 1, make_none());
		global_slots[static_cast<size_t>(slot)] = v;
	}
	globals[name] = v;
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
		function_frame_sizes,
		function_variadic_slot,
		event_handlers,
		module_global_init,
		declared_globals,
		declared_global_order,
		chunk_globals,
		lambda_counter);
	CodeLocation res = parser.parse();
	if (res.has_error)
		return res;

	rebuild_global_slots();

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
			{
				array_set(arr, std::to_string(index++), make_string(line));
			}
			array_set(ctx, pair.first, arr);
		}
		globals["context"] = ctx;
		auto slot = get_global_slot("context");
		if (slot >= 0)
		{
			if (global_slots.size() <= static_cast<size_t>(slot))
				global_slots.resize(static_cast<size_t>(slot) + 1, make_none());
			global_slots[static_cast<size_t>(slot)] = ctx;
		}
	};
	populate_context_global();

	if (!module_global_init.empty())
	{
		std::string init_fn = "__globals_init_" + std::to_string(global_init_counter++);
		instructions[init_fn] = std::make_shared<std::vector<UdonInstruction>>(module_global_init);
		function_params[init_fn] = std::make_shared<std::vector<std::string>>();
		function_param_slots[init_fn] = std::make_shared<std::vector<s32>>();
		function_frame_sizes[init_fn] = 0;
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
	auto scope_it = function_frame_sizes.find(function_name);
	if (scope_it != function_frame_sizes.end())
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
	reset_state(true, true);
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
			if (instr.opcode_instruction == Opcode::PUSH_LITERAL && !instr.operands.empty())
			{
				ss << "PUSH " << value_to_string(instr.operands[0]);
				ss << "\n";
				continue;
			}
			auto print_var = [&](const std::string& label)
			{
				ss << label << " " << (instr.operands.empty() ? "<anon>" : instr.operands[0].string_value);
			};
			switch (instr.opcode_instruction)
			{
				case Opcode::PUSH_LITERAL:
					ss << "PUSH <none>";
					break;
				case Opcode::LOAD_LOCAL:
					ss << "LOAD_LOCAL depth=" << (instr.operands.size() > 0 ? instr.operands[0].int_value : -1)
					   << " slot=" << (instr.operands.size() > 1 ? instr.operands[1].int_value : -1);
					break;
				case Opcode::STORE_LOCAL:
					ss << "STORE_LOCAL depth=" << (instr.operands.size() > 0 ? instr.operands[0].int_value : -1)
					   << " slot=" << (instr.operands.size() > 1 ? instr.operands[1].int_value : -1);
					break;
				case Opcode::LOAD_GLOBAL:
					print_var("LOADG");
					break;
				case Opcode::STORE_GLOBAL:
					print_var("STOREG");
					break;
				case Opcode::LOAD_VAR:
					print_var("LOAD");
					break;
				case Opcode::STORE_VAR:
					print_var("STORE");
					break;
				case Opcode::ENTER_SCOPE:
					ss << "ENTER_SCOPE slots=" << (instr.operands.empty() ? 0 : instr.operands[0].int_value);
					break;
				case Opcode::EXIT_SCOPE:
					ss << "EXIT_SCOPE";
					break;
				case Opcode::ADD:
					ss << "ADD";
					break;
				case Opcode::SUB:
					ss << "SUB";
					break;
				case Opcode::CONCAT:
					ss << "CONCAT";
					break;
				case Opcode::MUL:
					ss << "MUL";
					break;
				case Opcode::DIV:
					ss << "DIV";
					break;
				case Opcode::MOD:
					ss << "MOD";
					break;
				case Opcode::NEGATE:
					ss << "NEG";
					break;
				case Opcode::EQ:
					ss << "EQ";
					break;
				case Opcode::NEQ:
					ss << "NEQ";
					break;
				case Opcode::LT:
					ss << "LT";
					break;
				case Opcode::LTE:
					ss << "LTE";
					break;
				case Opcode::GT:
					ss << "GT";
					break;
				case Opcode::GTE:
					ss << "GTE";
					break;
				case Opcode::JUMP:
					ss << "JUMP " << (instr.operands.empty() ? -1 : instr.operands[0].int_value);
					break;
				case Opcode::JUMP_IF_FALSE:
					ss << "JZ " << (instr.operands.empty() ? -1 : instr.operands[0].int_value);
					break;
				case Opcode::TO_BOOL:
					ss << "TO_BOOL";
					break;
				case Opcode::LOGICAL_NOT:
					ss << "NOT";
					break;
				case Opcode::GET_PROP:
					ss << "GET_PROP " << (instr.operands.empty() ? "<name>" : instr.operands[0].string_value);
					break;
				case Opcode::STORE_PROP:
					ss << "STORE_PROP " << (instr.operands.empty() ? "<name>" : instr.operands[0].string_value);
					break;
				case Opcode::MAKE_CLOSURE:
					ss << "MAKE_CLOSURE " << (instr.operands.empty() ? "<name>" : instr.operands[0].string_value);
					break;
				case Opcode::CALL:
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
				case Opcode::RETURN:
					ss << "RETURN";
					break;
				case Opcode::POP:
					ss << "POP";
					break;
				case Opcode::NOP:
					ss << "NOP";
					break;
				case Opcode::HALT:
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
			mark_value(entry->key);
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

void UdonInterpreter::collect_garbage(UdonEnvironment* env_root,
	const std::vector<UdonValue>* value_roots,
	u32 time_budget_ms,
	bool invalidate_caches)
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

	auto mark_value_roots = [&](const std::vector<UdonValue>* roots)
	{
		if (!roots)
			return;
		for (const auto& v : *roots)
			mark_value(v);
	};

	auto mark_env_root = [&](UdonEnvironment* root)
	{
		if (root)
			mark_environment(root);
	};

	for (auto* root_ptr : active_env_roots)
		mark_env_root(root_ptr ? *root_ptr : nullptr);
	for (auto& kv : globals)
		mark_value(kv.second);
	for (auto& v : stack)
		mark_value(v);
	if (!invalidate_caches)
	{
		for (auto& kv : function_cache)
			mark_value(kv.second);
	}
	for (auto* roots : active_value_roots)
		mark_value_roots(roots);
	mark_env_root(env_root);
	mark_value_roots(value_roots);

	if (invalidate_caches)
	{
		function_cache.clear();
		code_cache_versions.clear();
		++cache_version;
	}

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
