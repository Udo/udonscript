#pragma once

#include "helpers.h"
#include <vector>
#include <memory>
#include <unordered_map>

struct UdonInterpreter; // forward to allow reuse of builtins/invoke

struct US2ValueRef
{
	s32 frame_depth = 0;
	s32 index = 0;
};

enum class Opcode2
{
	NOP,
	MOVE, // dst = a
	LOADK, // dst = literal
	POP,
	LOAD_GLOBAL,
	STORE_GLOBAL,
	ADD,
	SUB,
	CONCAT,
	MUL,
	DIV,
	MOD,
	NEGATE,
	TO_BOOL,
	LOGICAL_NOT,
	GET_PROP,
	STORE_PROP,
	MAKE_CLOSURE,
	EQ,
	NEQ,
	LT,
	LTE,
	GT,
	GTE,
	JUMP, // jump_target
	JUMP_IF_FALSE,
	CALL, // a=function ref or named, b = arg count (index in literal.int_value), dst receives result
	RETURN,
	HALT,
};

constexpr size_t kOpcode2Count = static_cast<size_t>(Opcode2::HALT) + 1;

struct US2Instruction
{
	Opcode2 opcode = Opcode2::NOP;
	US2ValueRef dst{};
	US2ValueRef a{};
	US2ValueRef b{};
	s32 jump_target = -1;
	bool has_literal = false;
	UdonValue literal{};
	std::string callee_name; // optional for CALL
	u32 line = 0;
	u32 column = 0;
};

using US2Code = std::vector<US2Instruction>;

struct US2Function
{
	std::shared_ptr<US2Code> code;
	size_t frame_size = 0;
	s32 result_slot = 0; // where RETURN reads from within the current frame
	std::vector<std::string> params;
	bool variadic = false;
	std::vector<s32> param_slots;
	s32 variadic_slot = -1;
	std::string name;
};

struct US2Frame
{
	size_t base = 0;
	size_t size = 0;
	size_t ip = 0;
	const US2Function* fn = nullptr;
	bool has_ret = false;
	US2ValueRef ret_dst{}; // target slot in caller frame
	UdonEnvironment* env = nullptr; // optional lexical environment for closures
};

struct UdonInterpreter2
{
	std::vector<UdonValue> value_stack;
	std::vector<US2Frame> call_stack;
	std::unordered_map<std::string, US2Function> functions;

	void register_function(const std::string& name, const US2Function& fn);
	CodeLocation run(const std::string& function_name,
		std::vector<UdonValue> args,
		UdonValue& return_value);
	bool load_from_host(UdonInterpreter* host_interp, CodeLocation& err);
};

bool compile_to_us2(
	const std::string& fn_name,
	const std::vector<UdonInstruction>& legacy,
	size_t legacy_frame_size,
	US2Function& out_fn,
	CodeLocation& err);

std::string dump_us2_function(const US2Function& fn);

const char* opcode2_name(Opcode2 op);
