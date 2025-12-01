#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>

struct CodeLocation
{
	u32 line;
	u32 column;
	bool has_error = false;
	std::string file_or_buffer_name;
	std::string opt_error_message;
};

struct Token
{
	enum class Type
	{
		Identifier,
		Number,
		String,
		Keyword,
		Symbol,
		EndOfFile,
		Unknown
	};

	Type type;
	std::string text;
	u32 line;
	u32 column;
};

struct UdonValue
{
	struct ManagedArray;

	enum class Type
	{
		VariableReference,
		S32,
		F32,
		String,
		Vector2,
		Vector3,
		Vector4,
		Bool,
		Array, // managed array/map
		None
	};

	Type type;
	union
	{
		s32 s32_value;
		f32 f32_value;
		Vector2 vec2_value;
		Vector3 vec3_value;
		Vector4 vec4_value;
		void* ptr_value; // for entity, material, mesh, texture references
	};
	std::string string_value;
	ManagedArray* array_map = nullptr;

	UdonValue() : type(Type::None) {}
};

using UdonBuiltinFunction = std::function<bool(struct UdonInterpreter*,
	const std::vector<UdonValue>&,
	const std::unordered_map<std::string, UdonValue>&,
	UdonValue&,
	CodeLocation&)>;

struct UdonBuiltinEntry
{
	std::string arg_signature;
	std::string return_type;
	UdonBuiltinFunction function;
};

struct UdonInstruction
{
	enum class OpCode
	{
		NOP,
		PUSH_LITERAL,
		LOAD_VAR,
		STORE_VAR,
		ADD,
		SUB,
		MUL,
		DIV,
		NEGATE,
		EQ,
		NEQ,
		LT,
		LTE,
		GT,
		GTE,
		JUMP,
		JUMP_IF_FALSE,
		TO_BOOL,
		LOGICAL_NOT,
		GET_PROP,
		CALL,
		RETURN,
		POP,
		HALT
	};

	OpCode opcode;
	std::vector<UdonValue> operands;
};

/* // Example code, typed, with optionally named arguments 

function add(a: s32, b: s32, c: s32) -> s32 { // type names match engine typedefs
    return a + b * c;
}

function main() {
    var result: s32 = add(5, c=10, b=2); // missing args default to 0
    scene.root.transform.translate_by(Vector3(0.0, result, 0.0)); // implicit conversions
    print("Result: " + result);
}

*/

struct UdonInterpreter
{
	std::unordered_map<std::string, UdonValue> globals;
	std::unordered_map<std::string, std::vector<UdonInstruction>> instructions; // by function name
	std::unordered_map<std::string, std::vector<std::string>> function_params; // parameter names per function
	std::unordered_map<std::string, UdonBuiltinEntry> builtins;
	std::unordered_map<std::string, std::vector<std::string>> event_handlers; // on:event -> function names
	std::unordered_set<std::string> declared_globals;
	std::vector<UdonValue> stack;
	std::vector<UdonValue::ManagedArray*> heap_arrays;
	s32 global_init_counter = 0;

	UdonInterpreter();
	~UdonInterpreter();
	std::vector<Token> tokenize(const std::string& source_code);
	CodeLocation compile(const std::string& source_code);
	CodeLocation compile_append(const std::string& source_code);
	void seed_builtin_globals();
	CodeLocation run(std::string function_name,
		std::vector<UdonValue> args,
		std::unordered_map<std::string, UdonValue> named_args,
		UdonValue& return_value);
	CodeLocation run_eventhandlers(std::string on_event_name);
	std::string dump_instructions() const;
	void clear();
	void collect_garbage();
	void register_function(const std::string& name,
		const std::string& arg_signature,
		const std::string& return_type,
		UdonBuiltinFunction fn);
	UdonValue::ManagedArray* allocate_array();
};

struct UdonValue::ManagedArray
{
	std::unordered_map<std::string, UdonValue> values;
	bool marked = false;
};
