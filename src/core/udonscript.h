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
		Template,
		EndOfFile,
		Unknown
	};

	Type type;
	std::string text;
	u32 line;
	u32 column;
	std::string template_content;
};

struct UdonValue
{
	struct ManagedArray;
	struct ManagedFunction;

	enum class Type
	{
		VariableReference,
		S32,
		F32,
		String,
		Bool,
		Array, // managed array/map
		Function, // managed closure/function object
		None
	};

	Type type;
	union
	{
		s32 s32_value;
		f32 f32_value;
		void* ptr_value; // for entity, material, mesh, texture references
	};
	std::string string_value;
	ManagedArray* array_map = nullptr;
	ManagedFunction* function = nullptr;

	UdonValue() : type(Type::None), ptr_value(nullptr), array_map(nullptr), function(nullptr) {}
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
		LOAD_LOCAL,
		STORE_LOCAL,
		LOAD_GLOBAL,
		STORE_GLOBAL,
		ENTER_SCOPE,
		EXIT_SCOPE,
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
		STORE_PROP,
		MAKE_CLOSURE,
		CALL,
		RETURN,
		POP,
		HALT
	};

	OpCode opcode;
	std::vector<UdonValue> operands;
	u32 line = 0;
	u32 column = 0;
};

struct UdonEnvironment
{
	std::vector<UdonValue> slots;
	UdonEnvironment* parent = nullptr;
	bool marked = false;
};

struct UdonInterpreter
{
	std::unordered_map<std::string, UdonValue> globals;
	std::unordered_map<std::string, std::vector<UdonInstruction>> instructions; // by function name
	std::unordered_map<std::string, std::vector<std::string>> function_params; // parameter names per function
	std::unordered_map<std::string, std::string> function_variadic; // variadic param name per function (optional)
	std::unordered_map<std::string, std::vector<s32>> function_param_slots; // slot index per parameter
	std::unordered_map<std::string, size_t> function_scope_sizes; // root scope slot counts
	std::unordered_map<std::string, s32> function_variadic_slot; // slot index for variadic parameter if present
	std::unordered_map<std::string, UdonBuiltinEntry> builtins;
	std::unordered_map<std::string, std::vector<std::string>> event_handlers; // on:event -> function names
	std::unordered_set<std::string> declared_globals;
	std::vector<UdonValue> stack;
	std::vector<std::vector<UdonEnvironment*>*> active_env_roots;
	std::vector<std::vector<UdonValue>*> active_value_roots;
	std::vector<UdonEnvironment*> heap_environments;
	std::vector<UdonValue::ManagedArray*> heap_arrays;
	std::vector<UdonValue::ManagedFunction*> heap_functions;
	u64 gc_runs = 0;
	u64 gc_time_ms = 0;
	std::vector<void*> dl_handles;
	std::vector<std::unique_ptr<UdonInterpreter>> imported_interpreters;
	s32 global_init_counter = 0;
	s32 lambda_counter = 0;

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
	void collect_garbage(const std::vector<UdonEnvironment*>* env_roots = nullptr,
		const std::vector<UdonValue>* value_roots = nullptr,
		u32 time_budget_ms = 0);
	void register_function(const std::string& name,
		const std::string& arg_signature,
		const std::string& return_type,
		UdonBuiltinFunction fn);
	UdonEnvironment* allocate_environment(size_t slot_count, UdonEnvironment* parent);
	UdonValue::ManagedArray* allocate_array();
	UdonValue::ManagedFunction* allocate_function();
	s32 register_dl_handle(void* handle);
	void* get_dl_handle(s32 id);
	bool close_dl_handle(s32 id);
	s32 register_imported_interpreter(std::unique_ptr<UdonInterpreter> sub);
	UdonInterpreter* get_imported_interpreter(s32 id);
};

struct UdonValue::ManagedArray
{
	std::unordered_map<std::string, UdonValue> values;
	bool marked = false;
};

struct UdonValue::ManagedFunction
{
	std::string function_name;
	UdonEnvironment* captured_env = nullptr;
	std::string handler; // optional builtin handler tag
	std::string template_body; // optional payload for template handlers
	s32 handler_data = -1; // optional numeric payload for handlers
	std::vector<UdonInstruction>* code_ptr = nullptr;
	std::vector<std::string>* param_ptr = nullptr;
	std::vector<s32> param_slots;
	size_t root_scope_size = 0;
	s32 variadic_slot = -1;
	std::string variadic_param;
	bool marked = false;
};
