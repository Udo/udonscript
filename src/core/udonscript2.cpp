#include "udonscript2.h"
#include "helpers.h"
#include "udonscript.h"

#include <algorithm>
#include <unordered_map>
#include <sstream>
#include <iostream>

// Root helper mirroring the original interpreter style
struct HostRootPop
{
	UdonInterpreter* host = nullptr;
	std::vector<UdonValue>* root = nullptr;
	explicit HostRootPop(UdonInterpreter* h, std::vector<UdonValue>* r) : host(h), root(r)
	{
		if (host && root)
			host->active_value_roots.push_back(root);
	}
	~HostRootPop()
	{
		if (host && root && !host->active_value_roots.empty())
			host->active_value_roots.pop_back();
	}
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

// Simple slot allocator used only during translation
struct StackSlotAllocator
{
	s32 next_slot;
	s32 max_slot;

	explicit StackSlotAllocator(s32 initial) : next_slot(initial), max_slot(initial) {}

	s32 push()
	{
		s32 slot = next_slot++;
		max_slot = std::max(max_slot, next_slot);
		return slot;
	}

	s32 pop()
	{
		if (next_slot <= 0)
			return -1;
		--next_slot;
		return next_slot;
	}

	s32 peek() const
	{
		return next_slot - 1;
	}
};

static US2Instruction make_loadk(s32 dst, const UdonValue& lit)
{
	US2Instruction o{};
	o.opcode = Opcode2::LOADK;
	o.dst = {0, dst};
	o.has_literal = true;
	o.literal = lit;
	return o;
}

static US2Instruction make_bin(Opcode2 op, s32 dst, s32 lhs, s32 rhs)
{
	US2Instruction o{};
	o.opcode = op;
	o.dst = {0, dst};
	o.a = {0, lhs};
	o.b = {0, rhs};
	return o;
}

static bool translate_instruction(const UdonInstruction& in,
	StackSlotAllocator& slots,
	std::vector<US2Instruction>& out,
	CodeLocation& err)
{
	US2Instruction o{};
	o.line = in.line;
	o.column = in.column;
	switch (in.opcode_instruction)
	{
		case Opcode::PUSH_LITERAL:
		{
			s32 dst = slots.push();
			o = make_loadk(dst, in.operands.empty() ? make_none() : in.operands[0]);
			out.push_back(o);
			return true;
		}
		case Opcode::POP:
		{
			slots.pop();
			o.opcode = Opcode2::POP;
			out.push_back(o);
			return true;
		}
		case Opcode::NOP:
		{
			o.opcode = Opcode2::NOP;
			out.push_back(o);
			return true;
		}
		case Opcode::MAKE_CLOSURE:
		{
			std::string name = !in.operands.empty() ? in.operands[0].string_value : "";
			s32 dst = slots.push();
			o.opcode = Opcode2::MAKE_CLOSURE;
			o.dst = {0, dst};
			o.has_literal = true;
			o.literal = make_string(name);
			out.push_back(o);
			return true;
		}
		case Opcode::LOAD_LOCAL:
		{
			s32 depth = in.operands.size() > 0 ? static_cast<s32>(in.operands[0].int_value) : 0;
			s32 src_slot = in.operands.size() > 1 ? static_cast<s32>(in.operands[1].int_value) : 0;
			s32 dst = slots.push();
			o.opcode = Opcode2::MOVE;
			o.dst = {0, dst};
			o.a = {depth, src_slot};
			out.push_back(o);
			return true;
		}
		case Opcode::STORE_LOCAL:
		{
			s32 depth = in.operands.size() > 0 ? static_cast<s32>(in.operands[0].int_value) : 0;
			s32 dst_slot = in.operands.size() > 1 ? static_cast<s32>(in.operands[1].int_value) : 0;
			s32 src = slots.pop();
			if (src < 0)
				src = 0;
			o.opcode = Opcode2::MOVE;
			o.dst = {depth, dst_slot};
			o.a = {0, src};
			out.push_back(o);
			return true;
		}
		case Opcode::LOAD_GLOBAL:
		case Opcode::LOAD_VAR:
		{
			s32 dst = slots.push();
			o.opcode = Opcode2::LOAD_GLOBAL;
			o.dst = {0, dst};
			o.has_literal = !in.operands.empty();
			o.literal = in.operands.empty() ? make_string("") : in.operands[0];
			out.push_back(o);
			return true;
		}
		case Opcode::STORE_VAR:
		case Opcode::STORE_GLOBAL:
		{
			s32 src = slots.pop();
			if (src < 0)
				src = 0;
			o.opcode = Opcode2::STORE_GLOBAL;
			o.a = {0, src};
			o.has_literal = !in.operands.empty();
			o.literal = in.operands.empty() ? make_string("") : in.operands[0];
			out.push_back(o);
			return true;
		}
		case Opcode::ADD:
		case Opcode::SUB:
		case Opcode::CONCAT:
		case Opcode::MUL:
		case Opcode::DIV:
		case Opcode::MOD:
		{
			s32 rhs = slots.pop();
			s32 lhs = slots.pop();
			s32 dst = slots.push();
			Opcode2 op2 = Opcode2::ADD;
			switch (in.opcode_instruction)
			{
				case Opcode::ADD: op2 = Opcode2::ADD; break;
				case Opcode::SUB: op2 = Opcode2::SUB; break;
				case Opcode::CONCAT: op2 = Opcode2::CONCAT; break;
				case Opcode::MUL: op2 = Opcode2::MUL; break;
				case Opcode::DIV: op2 = Opcode2::DIV; break;
				case Opcode::MOD: op2 = Opcode2::MOD; break;
				default: break;
			}
			o = make_bin(op2, dst, lhs, rhs);
			out.push_back(o);
			return true;
		}
		case Opcode::GET_PROP:
		{
			std::string name = !in.operands.empty() ? in.operands[0].string_value : "";
			if (name == "[index]")
			{
				s32 idx_slot = slots.pop();
				s32 obj_slot = slots.pop();
				s32 dst = slots.push();
				o.opcode = Opcode2::GET_PROP;
				o.dst = {0, dst};
				o.a = {0, obj_slot};
				o.b = {0, idx_slot};
				o.has_literal = false;
				out.push_back(o);
				return true;
			}
			s32 obj_slot = slots.pop();
			s32 dst = slots.push();
			o.opcode = Opcode2::GET_PROP;
			o.dst = {0, dst};
			o.a = {0, obj_slot};
			o.has_literal = true;
			o.literal = make_string(name);
			out.push_back(o);
			return true;
		}
		case Opcode::STORE_PROP:
		{
			std::string name = !in.operands.empty() ? in.operands[0].string_value : "";
			s32 value_slot = slots.pop();
			if (name == "[index]")
			{
				s32 idx_slot = slots.pop();
				s32 obj_slot = slots.pop();
				o.opcode = Opcode2::STORE_PROP;
				o.dst = {0, obj_slot};
				o.a = {0, value_slot};
				o.b = {0, idx_slot};
				o.has_literal = false;
				out.push_back(o);
				return true;
			}
			s32 obj_slot = slots.pop();
			o.opcode = Opcode2::STORE_PROP;
			o.dst = {0, obj_slot};
			o.a = {0, value_slot};
			o.has_literal = true;
			o.literal = make_string(name);
			out.push_back(o);
			return true;
		}
		case Opcode::EQ:
		case Opcode::NEQ:
		case Opcode::LT:
		case Opcode::LTE:
		case Opcode::GT:
		case Opcode::GTE:
		{
			s32 rhs = slots.pop();
			s32 lhs = slots.pop();
			s32 dst = slots.push();
			Opcode2 op2 = Opcode2::EQ;
			switch (in.opcode_instruction)
			{
				case Opcode::EQ: op2 = Opcode2::EQ; break;
				case Opcode::NEQ: op2 = Opcode2::NEQ; break;
				case Opcode::LT: op2 = Opcode2::LT; break;
				case Opcode::LTE: op2 = Opcode2::LTE; break;
				case Opcode::GT: op2 = Opcode2::GT; break;
				case Opcode::GTE: op2 = Opcode2::GTE; break;
				default: break;
			}
			o = make_bin(op2, dst, lhs, rhs);
			out.push_back(o);
			return true;
		}
		case Opcode::TO_BOOL:
		{
			s32 src = slots.pop();
			s32 dst = slots.push();
			o.opcode = Opcode2::TO_BOOL;
			o.dst = {0, dst};
			o.a = {0, src};
			out.push_back(o);
			return true;
		}
		case Opcode::NEGATE:
		{
			s32 src = slots.pop();
			s32 dst = slots.push();
			o.opcode = Opcode2::NEGATE;
			o.dst = {0, dst};
			o.a = {0, src};
			out.push_back(o);
			return true;
		}
		case Opcode::LOGICAL_NOT:
		{
			s32 src = slots.pop();
			s32 dst = slots.push();
			o.opcode = Opcode2::LOGICAL_NOT;
			o.dst = {0, dst};
			o.a = {0, src};
			out.push_back(o);
			return true;
		}
		case Opcode::JUMP:
		{
			o.opcode = Opcode2::JUMP;
			o.jump_target = in.operands.empty() ? -1 : static_cast<s32>(in.operands[0].int_value);
			out.push_back(o);
			return true;
		}
		case Opcode::JUMP_IF_FALSE:
		{
			s32 cond = slots.pop();
			o.opcode = Opcode2::JUMP_IF_FALSE;
			o.a = {0, cond};
			o.jump_target = in.operands.empty() ? -1 : static_cast<s32>(in.operands[0].int_value);
			out.push_back(o);
			return true;
		}
		case Opcode::ENTER_SCOPE:
		case Opcode::EXIT_SCOPE:
		{
			o.opcode = Opcode2::NOP;
			out.push_back(o);
			return true;
		}
		case Opcode::CALL:
		{
			s32 argc = in.operands.size() > 1 ? static_cast<s32>(in.operands[1].int_value) : 0;
			std::string name = in.operands.empty() ? "" : in.operands[0].string_value;
			// Arguments are expected to be on top of the stack in order. Use the current
			// stack top minus argc as the base, then pop them logically. For dynamic
			// calls, the callable sits just below the arguments, so pop it before
			// allocating the destination slot to avoid overwriting the callable.
			s32 base_slot = slots.next_slot - argc;
			if (base_slot < 0)
				base_slot = 0;
			slots.next_slot -= argc;
			s32 callable_slot = -1;
			if (name.empty())
				callable_slot = slots.pop();
			s32 dst = slots.push();
			o.opcode = Opcode2::CALL;
			o.dst = {0, dst};
			o.literal = make_int(argc);
			o.has_literal = true;
			if (name.empty())
			{
				if (callable_slot < 0)
					callable_slot = 0;
				o.a = {0, callable_slot}; // callable
				o.b = {0, base_slot};     // arg base
				o.callee_name = "";
			}
			else
			{
				o.a = {0, base_slot};
				o.b = {0, 0};
				o.callee_name = name;
			}
			out.push_back(o);
			return true;
		}
		case Opcode::RETURN:
		{
			s32 src = slots.peek();
			if (src < 0)
				src = 0;
			o.opcode = Opcode2::RETURN;
			o.a = {0, src};
			out.push_back(o);
			return true;
		}
		case Opcode::HALT:
		{
			o.opcode = Opcode2::HALT;
			out.push_back(o);
			return true;
		}
		default:
			err.has_error = true;
			err.opt_error_message = "udonscript2: unsupported opcode in translator";
			err.line = in.line;
			err.column = in.column;
			return false;
	}
}

bool compile_to_us2(UdonInterpreter* host,
	const std::string& fn_name,
	const std::vector<UdonInstruction>& legacy,
	size_t legacy_frame_size,
	US2Function& out_fn,
	CodeLocation& err)
{
	err.has_error = false;
	StackSlotAllocator slots(static_cast<s32>(legacy_frame_size));
	std::vector<US2Instruction> code;
	code.reserve(legacy.size());
	std::unordered_map<size_t, s32> slot_overrides;
	for (size_t ip = 0; ip < legacy.size(); ++ip)
	{
		auto it = slot_overrides.find(ip);
		if (it != slot_overrides.end())
		{
			slots.next_slot = it->second;
			slots.max_slot = std::max(slots.max_slot, slots.next_slot);
		}

		const auto& instr = legacy[ip];
		if (!translate_instruction(instr, slots, code, err))
			return false;

		if (!instr.operands.empty() &&
			(instr.opcode_instruction == Opcode::JUMP || instr.opcode_instruction == Opcode::JUMP_IF_FALSE))
		{
			s64 raw_target = instr.operands[0].int_value;
			if (raw_target >= 0)
			{
				size_t target = static_cast<size_t>(raw_target);
				s32 depth_after = slots.next_slot;
				auto jt = slot_overrides.find(target);
				if (jt == slot_overrides.end())
					slot_overrides[target] = depth_after;
				else
					jt->second = std::min(jt->second, depth_after);
			}
		}
	}
	out_fn.code = std::make_shared<US2Code>(std::move(code));
	out_fn.frame_size = static_cast<size_t>(slots.max_slot);
	out_fn.result_slot = 0;
	out_fn.name = fn_name;
	(void)host;
	return true;
}

void UdonInterpreter2::register_function(const std::string& name, const US2Function& fn)
{
	functions[name] = fn;
}

static UdonValue* resolve_ref(UdonInterpreter2& vm, const US2ValueRef& ref)
{
	if (ref.frame_depth < 0 || ref.index < 0)
		return nullptr;
	if (ref.frame_depth >= static_cast<s32>(vm.call_stack.size()))
		return nullptr;
	const size_t frame_idx = vm.call_stack.size() - 1 - static_cast<size_t>(ref.frame_depth);
	US2Frame& frame = vm.call_stack[frame_idx];
	const size_t slot = frame.base + static_cast<size_t>(ref.index);
	if (slot >= vm.value_stack.size())
		return nullptr;
	return &vm.value_stack[slot];
}

CodeLocation UdonInterpreter2::run(const std::string& function_name,
	std::vector<UdonValue> args,
	UdonValue& return_value)
{
	CodeLocation err{};
	err.has_error = false;
	static int call_debug_counter = 0;
	auto fit = functions.find(function_name);
	if (fit == functions.end())
	{
		err.has_error = true;
		err.opt_error_message = "Function '" + function_name + "' not found";
		return err;
	}

	auto fail = [&](const std::string& msg) -> CodeLocation
	{
		err.has_error = true;
		err.opt_error_message = msg;
		return err;
	};

	auto bool_value = [&](const UdonValue& v) -> bool
	{
		return is_truthy(v);
	};

	auto place_args = [&](const US2Function& f, US2Frame& target_frame, const std::vector<UdonValue>& args_vec) -> bool
	{
		size_t total_params = !f.param_slots.empty() ? f.param_slots.size() : f.params.size();
		size_t fixed_params = (f.variadic && total_params > 0) ? (total_params - 1) : total_params;
		size_t to_bind = std::min(args_vec.size(), fixed_params);
		for (size_t i = 0; i < to_bind; ++i)
		{
			s32 slot = (!f.param_slots.empty() && i < f.param_slots.size()) ? f.param_slots[i] : static_cast<s32>(i);
			if (slot < 0)
				slot = static_cast<s32>(i);
			size_t idx = target_frame.base + static_cast<size_t>(slot);
			if (idx >= value_stack.size())
				return false;
			value_stack[idx] = args_vec[i];
			if (target_frame.env && static_cast<size_t>(slot) < target_frame.env->slots.size())
				target_frame.env->slots[static_cast<size_t>(slot)] = args_vec[i];
		}
		// Variadic handling: prefer recorded slot, fallback to the next slot after fixed params if slot unknown.
		if (f.variadic || f.variadic_slot >= 0)
		{
			s32 var_slot = f.variadic_slot >= 0 ? f.variadic_slot : static_cast<s32>(fixed_params);
			UdonValue vargs = make_array();
			for (size_t i = fixed_params; i < args_vec.size(); ++i)
				array_set(vargs, std::to_string(i - fixed_params), args_vec[i]);
			size_t idx = target_frame.base + static_cast<size_t>(var_slot);
			if (idx >= value_stack.size())
				return false;
			value_stack[idx] = vargs;
			if (target_frame.env && static_cast<size_t>(var_slot) < target_frame.env->slots.size())
				target_frame.env->slots[static_cast<size_t>(var_slot)] = vargs;
		}
		return true;
	};

	value_stack.clear();
	call_stack.clear();
	const US2Function* fn = &fit->second;
	US2Frame frame{};
	frame.base = 0;
	frame.size = fn->frame_size;
	frame.ip = 0;
	frame.fn = fn;
	frame.has_ret = false;
	frame.env = host ? host->allocate_environment(frame.size, nullptr) : nullptr;
	value_stack.resize(frame.base + frame.size, make_none());
	call_stack.push_back(frame);
	if (!place_args(*fn, call_stack.back(), args))
		return fail("Argument placement failed");

	HostRootPop root_guard(host, &value_stack);
	UdonEnvironment* env_root = call_stack.empty() ? nullptr : call_stack.back().env;
	struct EnvRootGuard
	{
		UdonInterpreter* host_ptr;
		UdonEnvironment** env_ptr;
		EnvRootGuard(UdonInterpreter* h, UdonEnvironment** e) : host_ptr(h), env_ptr(e)
		{
			if (host_ptr)
				host_ptr->active_env_roots.push_back(env_ptr);
		}
		~EnvRootGuard()
		{
			if (host_ptr)
				host_ptr->active_env_roots.pop_back();
		}
	} env_guard(host, &env_root);

	while (!call_stack.empty())
	{
		env_root = call_stack.empty() ? nullptr : call_stack.back().env;
		US2Frame& fr = call_stack.back();
		if (!fr.fn || !fr.fn->code)
			return fail("Invalid function frame");

		auto load_ref = [&](const US2ValueRef& r, UdonValue& out) -> bool
		{
			UdonValue* slot = resolve_ref(*this, r);
			if (!slot)
				return false;
			out = *slot;
			return true;
		};

		auto frame_for_ref = [&](const US2ValueRef& r) -> US2Frame*
		{
			if (r.frame_depth < 0 || static_cast<size_t>(r.frame_depth) >= call_stack.size())
				return nullptr;
			const size_t frame_idx = call_stack.size() - 1 - static_cast<size_t>(r.frame_depth);
			return &call_stack[frame_idx];
		};

		auto store_ref = [&](const US2ValueRef& r, const UdonValue& v) -> bool
		{
			UdonValue* slot = resolve_ref(*this, r);
			if (!slot)
				return false;
			*slot = v;
			US2Frame* target_frame = frame_for_ref(r);
			if (target_frame && target_frame->env && r.index >= 0 && static_cast<size_t>(r.index) < target_frame->env->slots.size())
				target_frame->env->slots[static_cast<size_t>(r.index)] = v;
			return true;
		};

		if (fr.ip >= fr.fn->code->size())
		{
			// Treat falling off the end like an implicit return of none.
			US2ValueRef ret = fr.ret_dst; // capture before pop
			call_stack.pop_back();
			UdonValue rv = make_none();
			if (call_stack.empty())
			{
				return_value = rv;
				return err;
			}
			else
			{
				store_ref(ret, rv);
				size_t target_size = call_stack.back().base + call_stack.back().size;
				if (value_stack.size() > target_size)
					value_stack.resize(target_size);
				continue;
			}
		}

		const US2Instruction& op = (*(fr.fn->code))[fr.ip];

		switch (op.opcode)
		{
			case Opcode2::NOP:
				fr.ip++;
				break;
			case Opcode2::LOADK:
			{
				store_ref(op.dst, op.literal);
				fr.ip++;
				break;
			}
			case Opcode2::MOVE:
			{
				UdonValue tmp{};
				if (!load_ref(op.a, tmp))
					return fail("Invalid MOVE source");
				store_ref(op.dst, tmp);
				fr.ip++;
				break;
			}
			case Opcode2::POP:
			{
				fr.ip++;
				break;
			}
			case Opcode2::LOAD_GLOBAL:
			{
				if (!host)
					return fail("Host interpreter missing for global load");
				std::string name = op.has_literal ? op.literal.string_value : "";
				UdonValue v;
				if (!host->get_global_value(name, v))
					v = make_none();
				store_ref(op.dst, v);
				fr.ip++;
				break;
			}
			case Opcode2::STORE_GLOBAL:
			{
				if (!host)
					return fail("Host interpreter missing for global store");
				std::string name = op.has_literal ? op.literal.string_value : "";
				UdonValue v{};
				if (!load_ref(op.a, v))
					return fail("Invalid STORE_GLOBAL source");
				host->set_global_value(name, v);
				fr.ip++;
				break;
			}
			case Opcode2::ADD:
			case Opcode2::SUB:
			case Opcode2::CONCAT:
			case Opcode2::MUL:
			case Opcode2::DIV:
			case Opcode2::MOD:
			{
				UdonValue lhs{}, rhs{}, result{};
				if (!load_ref(op.a, lhs) || !load_ref(op.b, rhs))
					return fail("Invalid binary operands");
				bool ok = false;
				switch (op.opcode)
				{
					case Opcode2::CONCAT:
						result = make_string(value_to_string(lhs) + value_to_string(rhs));
						ok = true;
						break;
					case Opcode2::ADD: ok = add_values(lhs, rhs, result); break;
					case Opcode2::SUB: ok = sub_values(lhs, rhs, result); break;
					case Opcode2::MUL: ok = mul_values(lhs, rhs, result); break;
					case Opcode2::DIV: ok = div_values(lhs, rhs, result); break;
					case Opcode2::MOD: ok = mod_values(lhs, rhs, result); break;
					default: break;
				}
				if (!ok)
					return fail("Arithmetic error");
				store_ref(op.dst, result);
				fr.ip++;
				break;
			}
			case Opcode2::NEGATE:
			{
				UdonValue src{};
				if (!load_ref(op.a, src))
					return fail("Invalid NEGATE source");
				if (!is_numeric(src))
					return fail("Cannot negate value");
				if (src.type == UdonValue::Type::Int)
					src.int_value = -src.int_value;
				else
					src.float_value = -src.float_value;
				store_ref(op.dst, src);
				fr.ip++;
				break;
			}
			case Opcode2::TO_BOOL:
			{
				UdonValue src{};
				if (!load_ref(op.a, src))
					return fail("Invalid TO_BOOL source");
				store_ref(op.dst, make_bool(bool_value(src)));
				fr.ip++;
				break;
			}
			case Opcode2::LOGICAL_NOT:
			{
				UdonValue src{};
				if (!load_ref(op.a, src))
					return fail("Invalid NOT source");
				store_ref(op.dst, make_bool(!bool_value(src)));
				fr.ip++;
				break;
			}
			case Opcode2::EQ:
			case Opcode2::NEQ:
			case Opcode2::LT:
			case Opcode2::LTE:
			case Opcode2::GT:
			case Opcode2::GTE:
			{
				UdonValue lhs{}, rhs{}, result{};
				if (!load_ref(op.a, lhs) || !load_ref(op.b, rhs))
					return fail("Invalid compare operands");
				Opcode cmp = Opcode::EQ;
				switch (op.opcode)
				{
					case Opcode2::EQ: cmp = Opcode::EQ; break;
					case Opcode2::NEQ: cmp = Opcode::NEQ; break;
					case Opcode2::LT: cmp = Opcode::LT; break;
					case Opcode2::LTE: cmp = Opcode::LTE; break;
					case Opcode2::GT: cmp = Opcode::GT; break;
					case Opcode2::GTE: cmp = Opcode::GTE; break;
					default: break;
				}
				if (cmp == Opcode::EQ || cmp == Opcode::NEQ)
				{
					if (!equal_values(lhs, rhs, result))
						return fail("Equality comparison failed");
					// equal_values already produced bool; flip for NEQ.
					if (cmp == Opcode::NEQ)
						result.int_value = result.int_value ? 0 : 1;
				}
				else
				{
					if (!compare_values(lhs, rhs, cmp, result))
						return fail("Comparison failed");
				}
				store_ref(op.dst, result);
				fr.ip++;
				break;
			}
			case Opcode2::GET_PROP:
			{
				UdonValue obj{}, res{};
				if (!load_ref(op.a, obj))
					return fail("Invalid GET_PROP object");
				bool ok = false;
				if (op.has_literal)
					ok = get_property_value(obj, op.literal.string_value, res);
				else
				{
					UdonValue idx{};
					if (!load_ref(op.b, idx))
						return fail("Invalid GET_PROP index");
					ok = get_index_value(obj, idx, res);
				}
				if (!ok)
					return fail("Property access failed");
				store_ref(op.dst, res);
				fr.ip++;
				break;
			}
			case Opcode2::STORE_PROP:
			{
				UdonValue* obj_ref = resolve_ref(*this, op.dst);
				if (!obj_ref)
					return fail("Invalid STORE_PROP object");
				UdonValue value{};
				if (!load_ref(op.a, value))
					return fail("Invalid STORE_PROP value");
				if (op.has_literal)
					array_set(*obj_ref, op.literal.string_value, value);
				else
				{
					UdonValue idx{};
					if (!load_ref(op.b, idx))
						return fail("Invalid STORE_PROP index");
					array_set(*obj_ref, key_from_value(idx), value);
				}
				fr.ip++;
				break;
			}
			case Opcode2::MAKE_CLOSURE:
			{
				UdonValue v{};
				v.type = UdonValue::Type::Function;
				v.function = host ? host->allocate_function() : nullptr;
				if (v.function)
				{
					v.function->function_name = op.has_literal ? op.literal.string_value : "";
					v.function->captured_env = fr.env;
					if (host)
					{
						auto code_it = host->instructions.find(v.function->function_name);
						if (code_it != host->instructions.end())
							v.function->code_ptr = code_it->second;
						auto param_it = host->function_params.find(v.function->function_name);
						if (param_it != host->function_params.end())
							v.function->param_ptr = param_it->second;
						auto ps_it = host->function_param_slots.find(v.function->function_name);
						if (ps_it != host->function_param_slots.end())
							v.function->param_slots = ps_it->second;
						auto ss_it = host->function_frame_sizes.find(v.function->function_name);
						if (ss_it != host->function_frame_sizes.end())
							v.function->root_scope_size = ss_it->second;
						auto vs_it = host->function_variadic_slot.find(v.function->function_name);
						v.function->variadic_slot = (vs_it != host->function_variadic_slot.end()) ? vs_it->second : -1;
						auto var_it = host->function_variadic.find(v.function->function_name);
						if (var_it != host->function_variadic.end())
							v.function->variadic_param = var_it->second;
					}
				}
				store_ref(op.dst, v);
				fr.ip++;
				break;
			}
			case Opcode2::JUMP:
			{
				if (op.jump_target < 0 || static_cast<size_t>(op.jump_target) > fr.fn->code->size())
					return fail("Invalid jump target");
				fr.ip = static_cast<size_t>(op.jump_target);
				break;
			}
			case Opcode2::JUMP_IF_FALSE:
			{
				UdonValue cond{};
				if (!load_ref(op.a, cond))
					return fail("Invalid jump condition");
				if (!bool_value(cond))
				{
					if (op.jump_target < 0 || static_cast<size_t>(op.jump_target) > fr.fn->code->size())
						return fail("Invalid jump target");
					fr.ip = static_cast<size_t>(op.jump_target);
				}
				else
				{
					fr.ip++;
				}
				break;
			}
			case Opcode2::CALL:
			{
				s32 argc = op.has_literal ? static_cast<s32>(op.literal.int_value) : 0;
				US2ValueRef args_base = op.callee_name.empty() ? op.b : op.a;
				std::vector<UdonValue> positional;
				auto load_args = [&](const US2ValueRef& base, std::vector<UdonValue>& out) -> bool
				{
					out.clear();
					for (s32 i = 0; i < argc; ++i)
					{
						UdonValue arg{};
						US2ValueRef arg_ref{base.frame_depth, base.index + i};
						if (!load_ref(arg_ref, arg))
							return false;
						out.push_back(arg);
					}
					return true;
				};
				if (!load_args(args_base, positional))
					return fail("Invalid CALL argument");
				// Heuristic recovery: only for `print`, if the first arg is not a string
				// label but the slot immediately before the base is a string, shift left by one.
				if (op.callee_name == "print" && argc >= 2 && args_base.index > 0 && positional.size() >= 1 && positional[0].type != UdonValue::Type::String)
				{
					US2ValueRef shifted = args_base;
					shifted.index -= 1;
					UdonValue prev{};
					if (load_ref(shifted, prev) && prev.type == UdonValue::Type::String)
					{
						std::vector<UdonValue> shifted_args;
						if (load_args(shifted, shifted_args))
						{
							args_base = shifted;
							positional.swap(shifted_args);
						}
					}
				}

				// Lightweight debug for early call plumbing; limited to first few calls.
				if (call_debug_counter < 200)
				{
					std::ostringstream dbg;
					dbg << "[VM2 CALL " << call_debug_counter << "] callee=";
					if (!op.callee_name.empty())
					{
						dbg << op.callee_name;
					}
					else
					{
						UdonValue callable_dbg{};
						if (load_ref(op.a, callable_dbg))
							dbg << value_to_string(callable_dbg);
						else
							dbg << "<callable load error>";
					}
					dbg << " argc=" << argc << " args_base=" << args_base.frame_depth << ":" << args_base.index << " args=[";
					for (size_t i = 0; i < positional.size(); ++i)
					{
						if (i)
							dbg << ", ";
						dbg << value_to_string(positional[i]);
					}
					dbg << "] dst=" << op.dst.frame_depth << ":" << op.dst.index;
					// Show raw slot contents around the arg base for quick inspection.
					dbg << " slots=";
					for (s32 i = 0; i < argc; ++i)
					{
						US2ValueRef arg_ref{args_base.frame_depth, args_base.index + i};
						UdonValue raw{};
						if (load_ref(arg_ref, raw))
							dbg << value_to_string(raw);
						else
							dbg << "<bad>";
						if (i + 1 < argc)
							dbg << ",";
					}
					if (call_debug_counter >= 16 && call_debug_counter <= 21)
					{
						dbg << " | frame0=";
						for (size_t i = 0; i < std::min<size_t>(value_stack.size(), 14); ++i)
						{
							if (i)
								dbg << ";";
							dbg << i << "=" << value_to_string(value_stack[i]);
						}
					}
					std::cerr << dbg.str() << std::endl;
					++call_debug_counter;
				}

				auto finish_return = [&](const UdonValue& rv)
				{
					store_ref(op.dst, rv);
					fr.ip++;
				};

				if (!op.callee_name.empty())
				{
					auto fn_it = functions.find(op.callee_name);
					if (fn_it != functions.end())
					{
						fr.ip++;
						US2Frame child{};
						child.base = value_stack.size();
						child.size = fn_it->second.frame_size;
						child.ip = 0;
						child.fn = &fn_it->second;
						child.has_ret = true;
						child.ret_dst = op.dst;
						child.env = host ? host->allocate_environment(child.size, fr.env) : nullptr;
						value_stack.resize(child.base + child.size, make_none());
						if (!place_args(*child.fn, child, positional))
							return fail("Argument placement failed");
						if (call_debug_counter < 200)
						{
							std::ostringstream dbg;
							dbg << "[VM2 ENTER " << call_debug_counter << "] fn=" << child.fn->name
							    << " base=" << child.base << " size=" << child.size
							    << " params=" << child.fn->params.size() << " slots=[";
							for (size_t i = 0; i < child.fn->param_slots.size(); ++i)
							{
								if (i)
									dbg << ",";
								dbg << child.fn->param_slots[i];
							}
							dbg << "] var_slot=" << child.fn->variadic_slot;
							dbg << " args=[";
							for (size_t i = 0; i < positional.size(); ++i)
							{
								if (i)
									dbg << ",";
								dbg << value_to_string(positional[i]);
							}
							dbg << "]";
							std::cerr << dbg.str() << std::endl;
						}
						call_stack.push_back(child);
						continue;
					}
				}

				if (host)
				{
					if (!op.callee_name.empty())
					{
						// Try builtins first
						auto bit = host->builtins.find(op.callee_name);
						if (bit != host->builtins.end())
						{
							UdonValue rv{};
							CodeLocation inner{};
							if (!bit->second.function(host, positional, rv, inner))
								return inner.has_error ? inner : fail("Builtin call failed");
							finish_return(rv);
							break;
						}

						// Call host interpreter by name
						UdonValue rv{};
						CodeLocation inner = host->run(op.callee_name, positional, rv);
						if (inner.has_error)
							return inner;
						finish_return(rv);
						break;
					}

					// Dynamic callable
					UdonValue callable{};
					if (!load_ref(op.a, callable))
						return fail("Invalid callable value");
					UdonValue rv{};
					CodeLocation inner = host->invoke_function(callable, positional, rv);
					if (inner.has_error)
						return inner;
					finish_return(rv);
					break;
				}

				return fail(op.callee_name.empty() ? "Dynamic call requires host interpreter" : "Function '" + op.callee_name + "' not found");
			}
			case Opcode2::RETURN:
			{
				UdonValue rv{};
				load_ref(op.a, rv);
				US2ValueRef ret = fr.ret_dst; // capture before pop
				const std::string fn_name = fr.fn ? fr.fn->name : std::string("<null>");
				call_stack.pop_back();
				if (call_debug_counter < 200)
				{
					std::ostringstream dbg;
					dbg << "[VM2 RETURN " << call_debug_counter << "] fn="
					    << fn_name << " rv=" << value_to_string(rv);
					std::cerr << dbg.str() << std::endl;
				}
				if (call_stack.empty())
				{
					return_value = rv;
					return err;
				}
				else
				{
					store_ref(ret, rv);
					size_t target_size = call_stack.back().base + call_stack.back().size;
					if (value_stack.size() > target_size)
						value_stack.resize(target_size);
				}
				break;
			}
			case Opcode2::HALT:
				return err;
		}
	}

	return err;
}

bool UdonInterpreter2::load_from_host(UdonInterpreter* host_interp, CodeLocation& err)
{
	err.has_error = false;
	if (!host_interp)
		return true;
	host = host_interp;
	for (const auto& kv : host_interp->instructions)
	{
		const std::string& name = kv.first;
		if (!kv.second)
			continue;
		US2Function fn{};
		fn.name = name;
		auto frame_it = host_interp->function_frame_sizes.find(name);
		size_t frame_size = (frame_it != host_interp->function_frame_sizes.end()) ? frame_it->second : 0;
		if (!compile_to_us2(host_interp, name, *kv.second, frame_size, fn, err))
			return false;
		auto param_it = host_interp->function_params.find(name);
		if (param_it != host_interp->function_params.end() && param_it->second)
			fn.params = *param_it->second;
		auto var_it = host_interp->function_variadic.find(name);
		if (var_it != host_interp->function_variadic.end())
			fn.variadic = !var_it->second.empty();
		auto ps_it = host_interp->function_param_slots.find(name);
		if (ps_it != host_interp->function_param_slots.end() && ps_it->second)
			fn.param_slots = *ps_it->second;
		auto vs_it = host_interp->function_variadic_slot.find(name);
		if (vs_it != host_interp->function_variadic_slot.end())
			fn.variadic_slot = vs_it->second;
		register_function(name, fn);
	}
	return true;
}

std::string dump_us2_function(const US2Function& fn)
{
	std::ostringstream ss;
	ss << "function " << fn.name << "(frame_size=" << fn.frame_size << ")\n";
	if (fn.code)
	{
		for (size_t i = 0; i < fn.code->size(); ++i)
		{
			const auto& op = (*(fn.code))[i];
			ss << i << ": op=" << static_cast<int>(op.opcode)
			   << " dst=" << op.dst.frame_depth << ":" << op.dst.index
			   << " a=" << op.a.frame_depth << ":" << op.a.index
			   << " b=" << op.b.frame_depth << ":" << op.b.index;
			if (op.has_literal)
				ss << " lit=" << value_to_string(op.literal);
			if (!op.callee_name.empty())
				ss << " callee=" << op.callee_name;
			if (op.jump_target >= 0)
				ss << " jmp=" << op.jump_target;
			ss << "\n";
		}
	}
	return ss.str();
}
