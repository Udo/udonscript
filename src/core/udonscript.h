#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include "memory.hpp"

#ifndef UDON_ASSERT
#ifndef NDEBUG
#define UDON_ASSERT(expr) assert(expr)
#else
#define UDON_ASSERT(expr) ((void)0)
#endif
#endif

#ifndef UDON_USE_VM2
#define UDON_USE_VM2 1
#endif

struct US2Function;

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

template <typename T>
class ValueHashMap;

struct UdonValue
{
	struct ManagedArray;
	struct ManagedFunction;

	enum class Type
	{
		VariableReference,
		Int,
		Float,
		String,
		Bool,
		Array, // managed array/map
		Function, // managed closure/function object
		None
	};

	Type type;
	union
	{
		s64 int_value;
		f64 float_value;
		void* ptr_value; // for entity, material, mesh, texture references
	};
	std::string string_value;
	ManagedArray* array_map = nullptr;
	ManagedFunction* function = nullptr;

	UdonValue() : type(Type::None), ptr_value(nullptr), array_map(nullptr), function(nullptr) {}
};

bool is_hashable_value(const UdonValue& v);
size_t hash_value(const UdonValue& v);
bool hashable_values_equal(const UdonValue& a, const UdonValue& b);

template <typename T>
struct ValueHashMap
{
	explicit ValueHashMap(size_t initial_buckets = 16)
	{
		init_buckets(initial_buckets);
	}

	void clear()
	{
		buckets.clear();
		count = 0;
	}

	size_t size() const
	{
		return count;
	}
	bool empty() const
	{
		return count == 0;
	}

	bool contains(const UdonValue& key) const
	{
		return find_entry_const(key) != nullptr;
	}

	T* find(const UdonValue& key)
	{
		auto* entry = find_entry_mutable(key);
		return entry ? &entry->value : nullptr;
	}

	const T* find(const UdonValue& key) const
	{
		auto* entry = find_entry_const(key);
		return entry ? &entry->value : nullptr;
	}

	bool set(const UdonValue& key, const T& value)
	{
		if (!is_hashable_value(key))
			return false;
		maybe_rehash();
		const size_t h = hash_value(key);
		auto& bucket = buckets[bucket_index(h)];
		for (auto& entry : bucket)
		{
			if (entry.hash == h && hashable_values_equal(entry.key, key))
			{
				entry.value = value;
				return true;
			}
		}
		bucket.push_back(Entry{ key, value, h });
		++count;
		return true;
	}

	bool erase(const UdonValue& key)
	{
		if (!is_hashable_value(key) || buckets.empty())
			return false;
		const size_t h = hash_value(key);
		auto& bucket = buckets[bucket_index(h)];
		for (size_t i = 0; i < bucket.size(); ++i)
		{
			auto& entry = bucket[i];
			if (entry.hash == h && hashable_values_equal(entry.key, key))
			{
				bucket[i] = bucket.back();
				bucket.pop_back();
				--count;
				return true;
			}
		}
		return false;
	}

	template <typename Fn>
	void for_each(Fn&& fn)
	{
		for (auto& bucket : buckets)
		{
			for (auto& entry : bucket)
			{
				if (!fn(entry.key, entry.value))
					return;
			}
		}
	}

	struct Entry
	{
		UdonValue key;
		T value;
		size_t hash;
	};

	std::vector<std::vector<Entry>> buckets;
	size_t count = 0;
	const double max_load = 0.75;

	void init_buckets(size_t n)
	{
		if (n < 4)
			n = 4;
		buckets.clear();
		buckets.resize(n);
		count = 0;
	}

	size_t bucket_index(size_t h) const
	{
		UDON_ASSERT(!buckets.empty());
		return h % buckets.size();
	}

	Entry* find_entry_mutable(const UdonValue& key)
	{
		if (!is_hashable_value(key) || buckets.empty())
			return nullptr;
		const size_t h = hash_value(key);
		auto& bucket = buckets[bucket_index(h)];
		for (auto& entry : bucket)
		{
			if (entry.hash == h && hashable_values_equal(entry.key, key))
				return &entry;
		}
		return nullptr;
	}

	const Entry* find_entry_const(const UdonValue& key) const
	{
		if (!is_hashable_value(key) || buckets.empty())
			return nullptr;
		const size_t h = hash_value(key);
		const auto& bucket = buckets[bucket_index(h)];
		for (const auto& entry : bucket)
		{
			if (entry.hash == h && hashable_values_equal(entry.key, key))
				return &entry;
		}
		return nullptr;
	}

	void maybe_rehash()
	{
		if (buckets.empty())
		{
			init_buckets(16);
			return;
		}
		if (static_cast<double>(count + 1) <= max_load * static_cast<double>(buckets.size()))
			return;
		const size_t new_size = buckets.size() * 2;
		std::vector<std::vector<Entry>> new_buckets;
		new_buckets.resize(new_size);
		for (auto& bucket : buckets)
		{
			for (auto& entry : bucket)
			{
				const size_t idx = entry.hash % new_size;
				new_buckets[idx].push_back(entry);
			}
		}
		buckets.swap(new_buckets);
	}
};

using UdonBuiltinFunction = std::function<bool(struct UdonInterpreter*,
	const std::vector<UdonValue>&,
	UdonValue&,
	CodeLocation&)>;

struct UdonBuiltinEntry
{
	std::string arg_signature;
	std::string return_type;
	UdonBuiltinFunction function;
};

extern std::vector<std::string> OpcodeNames;
enum class Opcode
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
	CONCAT,
	MUL,
	DIV,
	MOD,
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
	HALT,
	OPCODE_MAX
};

struct UdonInstruction
{
	Opcode opcode_instruction;
	std::vector<UdonValue> operands;
	u32 line = 0;
	u32 column = 0;

	enum class CachedKind
	{
		None,
		Builtin,
		Function
	};
	mutable u64 cached_version = 0;
	mutable s32 cached_global_slot = -1;
	mutable CachedKind cached_kind = CachedKind::None;
	mutable UdonBuiltinFunction cached_builtin;
	mutable UdonValue::ManagedFunction* cached_fn = nullptr;
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
	std::unordered_map<std::string, std::shared_ptr<std::vector<UdonInstruction>>> instructions; // by function name
	std::unordered_map<std::string, std::shared_ptr<std::vector<std::string>>> function_params; // parameter names per function
	std::unordered_map<std::string, std::string> function_variadic; // variadic param name per function (optional)
	std::unordered_map<std::string, std::shared_ptr<std::vector<s32>>> function_param_slots; // slot index per parameter
	std::unordered_map<std::string, size_t> function_frame_sizes; // total local slot counts
	std::unordered_map<std::string, s32> function_variadic_slot; // slot index for variadic parameter if present
	std::unordered_map<std::string, UdonBuiltinEntry> builtins;
	std::unordered_map<std::string, std::vector<std::string>> event_handlers; // on:event -> function names
	std::unordered_set<std::string> declared_globals;
	std::vector<std::string> declared_global_order;
	std::vector<UdonValue> global_slots;
	std::unordered_map<std::string, s32> global_slot_lookup;
	std::unordered_map<std::string, US2Function> functions_v2;
	std::vector<UdonValue> stack;
	std::vector<UdonEnvironment**> active_env_roots;
	std::vector<std::vector<UdonValue>*> active_value_roots;
	std::vector<UdonEnvironment*> heap_environments;
	std::vector<UdonValue::ManagedArray*> heap_arrays;
	std::vector<UdonValue::ManagedFunction*> heap_functions;

	u64 gc_runs = 0;
	u64 gc_time_ms = 0;
	u64 cache_version = 1;
	struct Stats
	{
		u64 opcode_counts[static_cast<size_t>(Opcode::OPCODE_MAX)] = { 0 };
		u64 resolve_function_by_name_calls = 0;
		std::vector<u64> opcode2_counts; // sized to VM2 opcode count when VM2 executes
		u64 scratch_arena_used = 0;
		u64 scratch_arena_capacity = 0;
	} stats;

	std::vector<void*> dl_handles;
	std::vector<std::unique_ptr<UdonInterpreter>> imported_interpreters;
	s32 global_init_counter = 0;
	s32 lambda_counter = 0;
	std::unordered_map<std::string, std::vector<std::string>> context_info;
	std::unordered_map<std::string, UdonValue> function_cache;
	std::unordered_map<const std::vector<UdonInstruction>*, u64> code_cache_versions;
	std::vector<std::vector<UdonValue>> value_buffer_pool;
	Arena scratch_arena;

	UdonInterpreter();
	~UdonInterpreter();
	std::vector<Token> tokenize(const std::string& source_code);
	CodeLocation compile(const std::string& source_code);
	CodeLocation compile_append(const std::string& source_code);
	void seed_builtin_globals();
	void reset_state(bool release_heaps, bool release_handles);
	CodeLocation run(std::string function_name,
		std::vector<UdonValue> args,
		UdonValue& return_value);
	CodeLocation run_us2(std::string function_name,
		std::vector<UdonValue> args,
		UdonValue& return_value);
	void rebuild_global_slots();
	s32 get_global_slot(const std::string& name) const;
	bool get_global_value(const std::string& name, UdonValue& out, s32 slot_hint = -1) const;
	void set_global_value(const std::string& name, const UdonValue& v, s32 slot_hint = -1);
	CodeLocation run_eventhandlers(std::string on_event_name);
	std::string dump_instructions() const;
	void clear();
	void collect_garbage(UdonEnvironment* env_root = nullptr,
		const std::vector<UdonValue>* value_roots = nullptr,
		u32 time_budget_ms = 0,
		bool invalidate_caches = false);
	void register_function(const std::string& name,
		const std::string& arg_signature,
		const std::string& return_type,
		UdonBuiltinFunction fn);
	CodeLocation invoke_function(const UdonValue& fn,
		const std::vector<UdonValue>& positional,
		UdonValue& out);
	UdonEnvironment* allocate_environment(size_t slot_count, UdonEnvironment* parent);
	UdonValue::ManagedArray* allocate_array();
	UdonValue::ManagedFunction* allocate_function();
	s32 register_dl_handle(void* handle);
	void* get_dl_handle(s32 id);
	bool close_dl_handle(s32 id);
	s32 register_imported_interpreter(std::unique_ptr<UdonInterpreter> sub);
	UdonInterpreter* get_imported_interpreter(s32 id);
};

extern thread_local UdonInterpreter* g_udon_current;

struct UdonValue::ManagedArray
{
	struct Entry
	{
		UdonValue key;
		UdonValue value;
		Entry* prev = nullptr;
		Entry* next = nullptr;
		size_t hash = 0;
	};

	ValueHashMap<Entry*> index;
	Entry* head = nullptr;
	Entry* tail = nullptr;
	size_t size = 0;
	bool marked = false;

	~ManagedArray();
};

struct UdonValue::ManagedFunction
{
	std::string function_name;
	UdonEnvironment* captured_env = nullptr;
	std::string template_body; // optional payload for native handlers
	std::shared_ptr<std::vector<UdonInstruction>> code_ptr;
	std::shared_ptr<std::vector<std::string>> param_ptr;
	std::shared_ptr<std::vector<s32>> param_slots;
	size_t root_scope_size = 0;
	s32 variadic_slot = -1;
	std::string variadic_param;
	std::shared_ptr<void> user_data; // optional external payload for native closures
	UdonBuiltinFunction native_handler; // optional native closure entrypoint
	std::vector<UdonValue> rooted_values; // values that must stay alive with this function
	bool marked = false;
	u64 magic = 0;
	bool is_cache_wrapper = false;
};

struct ScopedRoot
{
	ScopedRoot(UdonInterpreter* interp, std::vector<UdonValue>* external = nullptr)
		: interpreter(interp), external_values(external)
	{
		if (interpreter)
			interpreter->active_value_roots.push_back(storage());
	}
	~ScopedRoot()
	{
		if (interpreter)
			interpreter->active_value_roots.pop_back();
	}
	UdonValue& add(const UdonValue& v)
	{
		UDON_ASSERT(storage() != nullptr);
		storage()->push_back(v);
		return storage()->back();
	}
	UdonValue& value()
	{
		UDON_ASSERT(storage() && !storage()->empty());
		return storage()->back();
	}
	std::vector<UdonValue>& values()
	{
		UDON_ASSERT(storage() != nullptr);
		return *storage();
	}

	std::vector<UdonValue>* storage()
	{
		return external_values ? external_values : &owned_values;
	}

	UdonInterpreter* interpreter = nullptr;
	std::vector<UdonValue>* external_values = nullptr;
	std::vector<UdonValue> owned_values;
};

struct RootedArray
{
	explicit RootedArray(UdonInterpreter* interp) : root(interp)
	{
		UdonValue v{};
		v.type = UdonValue::Type::Array;
		v.array_map = interp ? interp->allocate_array() : nullptr;
		root.add(v);
	}
	UdonValue& value()
	{
		return root.value();
	}
	ScopedRoot root;
};

struct RootedFunction
{
	explicit RootedFunction(UdonInterpreter* interp) : root(interp)
	{
		UDON_ASSERT(interp != nullptr);
		UdonValue v{};
		v.type = UdonValue::Type::Function;
		v.function = interp ? interp->allocate_function() : nullptr;
		root.add(v);
	}
	UdonValue& value()
	{
		return root.value();
	}
	ScopedRoot root;
};
