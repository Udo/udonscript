#pragma once

#include "types.h"
#include "udonscript.h"
#include "helpers.h"
#include <cctype>
#include <iostream>
#include <sstream>
#include <stack>
#include <algorithm>
#include <unordered_set>
#include <memory>
#include <functional>
#include <utility>

struct Parser
{
	Parser(const std::vector<Token>& tokens,
		std::unordered_map<std::string, std::shared_ptr<std::vector<UdonInstruction>>>& instructions_out,
		std::unordered_map<std::string, std::shared_ptr<std::vector<std::string>>>& params_out,
		std::unordered_map<std::string, std::string>& variadic_out,
		std::unordered_map<std::string, std::shared_ptr<std::vector<s32>>>& param_slots_out,
		std::unordered_map<std::string, size_t>& scope_size_out,
		std::unordered_map<std::string, s32>& variadic_slot_out,
		std::unordered_map<std::string, std::vector<std::string>>& events_out,
		std::vector<UdonInstruction>& global_init_out,
		std::unordered_set<std::string>& globals_out,
		std::vector<std::string>& global_order_out,
		const std::unordered_set<std::string>& chunk_globals_out,
		s32& lambda_counter_ref)
		: tokens(tokens),
		  current(0),
		  stop_at_colon(false),
		  error_location{},
		  instructions(instructions_out),
		  params(params_out),
		  variadic(variadic_out),
		  param_slots(param_slots_out),
		  scope_sizes(scope_size_out),
		  variadic_slot(variadic_slot_out),
		  events(events_out),
		  global_init(global_init_out),
		  globals(globals_out),
		  global_order(global_order_out),
		  chunk_globals(chunk_globals_out),
		  lambda_counter(lambda_counter_ref)
	{
	}

	CodeLocation parse();

	const std::vector<Token>& tokens;
	size_t current = 0;
	bool stop_at_colon = false;
	CodeLocation error_location{};
	std::unordered_map<std::string, std::shared_ptr<std::vector<UdonInstruction>>>& instructions;
	std::unordered_map<std::string, std::shared_ptr<std::vector<std::string>>>& params;
	std::unordered_map<std::string, std::string>& variadic;
	std::unordered_map<std::string, std::shared_ptr<std::vector<s32>>>& param_slots;
	std::unordered_map<std::string, size_t>& scope_sizes;
	std::unordered_map<std::string, s32>& variadic_slot;
	std::unordered_map<std::string, std::vector<std::string>>& events;
	std::vector<UdonInstruction>& global_init;
	std::unordered_set<std::string>& globals;
	std::vector<std::string>& global_order;
	const std::unordered_set<std::string>& chunk_globals;
	s32& lambda_counter;

	struct ResolvedVariable
	{
		bool is_global = false;
		s32 depth = 0;
		s32 slot = 0;
		std::string name;
	};

	struct ScopeInfo
	{
		std::unordered_map<std::string, s32> slots;
		s32 declare(const std::string& name)
		{
			auto it = slots.find(name);
			if (it != slots.end())
				return it->second;
			s32 idx = static_cast<s32>(slots.size());
			slots[name] = idx;
			return idx;
		}
		bool contains(const std::string& name) const
		{
			return slots.find(name) != slots.end();
		}
	};

	struct ScopeFrame
	{
		std::shared_ptr<ScopeInfo> scope;
		size_t enter_instr = static_cast<size_t>(-1);
		bool runtime_scope = false;
	};

	struct FunctionContext
	{
		std::vector<ScopeFrame> scope_stack;
		std::vector<std::shared_ptr<ScopeInfo>> enclosing_scopes; // innermost -> outermost captured from surrounding contexts
		std::vector<s32> param_slot_indices;
		s32 variadic_slot_index = -1;

		ScopeFrame& current_scope()
		{
			return scope_stack.back();
		}
		const ScopeFrame& current_scope() const
		{
			return scope_stack.back();
		}
		size_t root_slot_count() const
		{
			if (scope_stack.empty())
				return 0;
			return scope_stack.front().scope ? scope_stack.front().scope->slots.size() : 0;
		}
	};

	struct LoopContext
	{
		std::vector<size_t> break_jumps;
		std::vector<size_t> continue_jumps;
		size_t continue_target = 0;
		bool allow_continue = false;
		size_t scope_depth = 0;
	};
	std::vector<LoopContext> loop_stack;

	struct LoopGuard
	{
		std::vector<LoopContext>& ref;
		std::vector<LoopContext> saved;
		LoopGuard(std::vector<LoopContext>& target, bool clear = false) : ref(target), saved(target)
		{
			if (clear)
				ref.clear();
		}
		~LoopGuard()
		{
			ref = saved;
		}
	};

	size_t begin_scope(FunctionContext& ctx, std::vector<UdonInstruction>& body, bool runtime_scope, const Token* tok = nullptr);
	size_t end_scope(FunctionContext& ctx, std::vector<UdonInstruction>& body);
	void emit_unwind_to_depth(FunctionContext& ctx, std::vector<UdonInstruction>& body, size_t target_depth);
	s32 declare_variable(FunctionContext& ctx, const std::string& name);
	bool resolve_variable(const FunctionContext& ctx, const std::string& name, ResolvedVariable& out) const;
	void emit_load_var(std::vector<UdonInstruction>& body, const ResolvedVariable& var, const Token* tok = nullptr);
	void emit_store_var(std::vector<UdonInstruction>& body, const ResolvedVariable& var, const Token* tok = nullptr);
	bool is_end() const;
	const Token& peek() const;
	const Token& previous() const;
	const Token& advance();
	bool check_symbol(const std::string& text) const;
	bool match_symbol(const std::string& text);
	bool match_keyword(const std::string& text);
	CodeLocation make_error(const Token& t, const std::string& msg);
	bool expect_symbol(const std::string& sym, const std::string& message);
	void skip_semicolons();
	bool is_declared(const FunctionContext& ctx, const std::string& name, ResolvedVariable* resolved = nullptr) const;
	bool parse_global_var();
	bool parse_function();
	void emit(std::vector<UdonInstruction>& body, UdonInstruction::OpCode op, const std::vector<UdonValue>& operands = {}, const Token* tok = nullptr);
	bool parse_block(std::vector<UdonInstruction>& body, FunctionContext& ctx, bool create_scope = true);
	bool parse_statement_or_block(std::vector<UdonInstruction>& body, FunctionContext& ctx, bool create_scope = true);
	bool parse_statement(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_expression(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_ternary(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_or(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_and(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_equality(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_comparison(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_assignment_or_expression(std::vector<UdonInstruction>& body, FunctionContext& ctx, bool& produced_value);
	bool parse_multiplicative(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_unary(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_postfix(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_method_postfix(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_key_postfix(std::vector<UdonInstruction>& body);
	bool parse_function_literal(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_primary(std::vector<UdonInstruction>& body, FunctionContext& ctx);
	bool parse_additive(std::vector<UdonInstruction>& body, FunctionContext& ctx);
};
