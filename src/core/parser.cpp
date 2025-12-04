#include "parser.h"
#include "helpers.h"

CodeLocation Parser::parse()
{
	CodeLocation ok{};
	ok.has_error = false;
	while (!is_end())
	{
		skip_semicolons();
		if (is_end())
			break;
		if (match_keyword("function"))
		{
			if (!parse_function())
				return error_location;
			continue;
		}
		if (match_keyword("var"))
		{
			if (!parse_global_var())
				return error_location;
			continue;
		}
		return make_error(peek(), "Expected 'function' or 'var'");
	}
	return ok;
}

size_t Parser::begin_scope(FunctionContext& ctx, std::vector<UdonInstruction>& body, bool runtime_scope, const Token* tok)
{
	ScopeFrame frame;
	frame.scope = std::make_shared<ScopeInfo>();
	frame.runtime_scope = runtime_scope;
	if (runtime_scope)
	{
		frame.enter_instr = body.size();
		emit(body, UdonInstruction::OpCode::ENTER_SCOPE, { make_int(0) }, tok);
	}
	ctx.scope_stack.push_back(frame);
	return ctx.scope_stack.size() - 1;
}

size_t Parser::end_scope(FunctionContext& ctx, std::vector<UdonInstruction>& body)
{
	size_t exit_index = body.size();
	if (ctx.scope_stack.empty())
		return exit_index;
	ScopeFrame frame = ctx.scope_stack.back();
	ctx.scope_stack.pop_back();
	if (frame.runtime_scope)
	{
		if (frame.enter_instr < body.size())
			body[frame.enter_instr].operands[0].int_value = static_cast<s64>(frame.scope->slots.size());
		exit_index = body.size();
		emit(body, UdonInstruction::OpCode::EXIT_SCOPE);
	}
	return exit_index;
}

void Parser::emit_unwind_to_depth(FunctionContext& ctx, std::vector<UdonInstruction>& body, size_t target_depth)
{
	if (target_depth > ctx.scope_stack.size())
		target_depth = ctx.scope_stack.size();
	for (size_t i = ctx.scope_stack.size(); i > target_depth; --i)
	{
		const ScopeFrame& frame = ctx.scope_stack[i - 1];
		if (frame.runtime_scope)
			emit(body, UdonInstruction::OpCode::EXIT_SCOPE);
	}
}

s32 Parser::declare_variable(FunctionContext& ctx, const std::string& name)
{
	if (ctx.scope_stack.empty())
		return -1;
	return ctx.current_scope().scope->declare(name);
}

bool Parser::resolve_variable(const FunctionContext& ctx, const std::string& name, ResolvedVariable& out) const
{
	for (int i = static_cast<int>(ctx.scope_stack.size()) - 1; i >= 0; --i)
	{
		const auto& scope = ctx.scope_stack[static_cast<size_t>(i)].scope;
		auto it = scope->slots.find(name);
		if (it != scope->slots.end())
		{
			out.is_global = false;
			out.depth = static_cast<s32>(ctx.scope_stack.size() - 1 - static_cast<size_t>(i));
			out.slot = it->second;
			out.name = name;
			return true;
		}
	}
	for (size_t i = 0; i < ctx.enclosing_scopes.size(); ++i)
	{
		const auto& scope = ctx.enclosing_scopes[i];
		auto it = scope->slots.find(name);
		if (it != scope->slots.end())
		{
			out.is_global = false;
			out.depth = static_cast<s32>(ctx.scope_stack.size() + i);
			out.slot = it->second;
			out.name = name;
			return true;
		}
	}
	if (globals.find(name) != globals.end() || chunk_globals.find(name) != chunk_globals.end())
	{
		out.is_global = true;
		out.depth = 0;
		out.slot = 0;
		out.name = name;
		return true;
	}
	return false;
}

void Parser::emit_load_var(std::vector<UdonInstruction>& body, const ResolvedVariable& var, const Token* tok)
{
	if (var.is_global)
	{
		emit(body, UdonInstruction::OpCode::LOAD_GLOBAL, { make_string(var.name) }, tok);
	}
	else
	{
		emit(body, UdonInstruction::OpCode::LOAD_LOCAL, { make_int(var.depth), make_int(var.slot) }, tok);
	}
}

void Parser::emit_store_var(std::vector<UdonInstruction>& body, const ResolvedVariable& var, const Token* tok)
{
	if (var.is_global)
		emit(body, UdonInstruction::OpCode::STORE_GLOBAL, { make_string(var.name) }, tok);
	else
		emit(body, UdonInstruction::OpCode::STORE_LOCAL, { make_int(var.depth), make_int(var.slot) }, tok);
}

bool Parser::is_end() const
{
	return peek().type == Token::Type::EndOfFile;
}

const Token& Parser::peek() const
{
	return tokens[current];
}

const Token& Parser::previous() const
{
	return tokens[current - 1];
}

const Token& Parser::advance()
{
	if (!is_end())
		current++;
	return previous();
}

bool Parser::check_symbol(const std::string& text) const
{
	if (is_end())
		return false;
	const auto& t = peek();
	return t.type == Token::Type::Symbol && t.text == text;
}

bool Parser::match_symbol(const std::string& text)
{
	if (check_symbol(text))
	{
		advance();
		return true;
	}
	return false;
}

bool Parser::match_keyword(const std::string& text)
{
	if (is_end())
		return false;
	const auto& t = peek();
	if (t.type == Token::Type::Keyword && t.text == text)
	{
		advance();
		return true;
	}
	return false;
}

CodeLocation Parser::make_error(const Token& t, const std::string& msg)
{
	error_location.has_error = true;
	error_location.line = t.line;
	error_location.column = t.column;
	error_location.opt_error_message = msg;
	return error_location;
}

bool Parser::expect_symbol(const std::string& sym, const std::string& message)
{
	if (match_symbol(sym))
		return true;
	make_error(peek(), message);
	return false;
}

void Parser::skip_semicolons()
{
	while (match_symbol(";"))
		;
}

bool Parser::is_declared(const FunctionContext& ctx, const std::string& name, ResolvedVariable* resolved) const
{
	ResolvedVariable tmp;
	if (resolve_variable(ctx, name, tmp))
	{
		if (resolved)
			*resolved = tmp;
		return true;
	}
	return false;
}

bool Parser::parse_global_var()
{
	if (peek().type != Token::Type::Identifier)
		return !make_error(peek(), "Expected variable name").has_error;
	const std::string name = advance().text;
	if (globals.find(name) != globals.end())
		return !make_error(previous(), "Global '" + name + "' already declared").has_error;
	if (match_symbol(":"))
		advance();
	globals.insert(name);

	if (match_symbol("="))
	{
		FunctionContext dummy_ctx;
		if (!parse_expression(global_init, dummy_ctx))
			return false;
	}
	else
	{
		emit(global_init, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
	}
	emit(global_init, UdonInstruction::OpCode::STORE_GLOBAL, { make_string(name) });
	return true;
}

bool Parser::parse_function()
{
	if (is_end())
		return false;
	std::string function_name;
	bool is_event_handler = false;
	std::string on_target;

	auto maybe_event = [&](bool already_consumed_on)
	{
		if (!match_symbol(":"))
			return !make_error(peek(), "Expected ':' after on").has_error;
		if (peek().type != Token::Type::Identifier)
			return !make_error(peek(), "Expected event name after on:").has_error;
		on_target = advance().text;
		is_event_handler = true;
		if (!already_consumed_on)
			function_name.clear();
		return true;
	};

	if (peek().type == Token::Type::Identifier)
	{
		if (peek().text == "on" && tokens.size() > current + 1 && tokens[current + 1].type == Token::Type::Symbol && tokens[current + 1].text == ":")
		{
			advance(); // consume 'on'
			if (!maybe_event(true))
				return false;
		}
		else
		{
			function_name = advance().text;
			if ((function_name == "on") && tokens.size() > current && tokens[current].type == Token::Type::Symbol && tokens[current].text == ":")
			{
				if (!maybe_event(true))
					return false;
				function_name.clear();
			}
			else if (!is_event_handler && tokens.size() > current + 1 && peek().type == Token::Type::Identifier && peek().text == "on" && tokens[current + 1].type == Token::Type::Symbol && tokens[current + 1].text == ":")
			{
				advance(); // consume 'on'
				if (!maybe_event(true))
					return false;
			}
		}
	}
	else if (match_keyword("on"))
	{
		if (!maybe_event(true))
			return false;
	}

	if (!is_event_handler && (match_keyword("on") || match_symbol("on")))
	{
		if (!maybe_event(false))
			return false;
	}

	if (function_name.empty())
	{
		function_name = "_anon_" + std::to_string(instructions.size());
	}

	if (!expect_symbol("(", "Expected '(' after function name"))
		return false;

	std::vector<std::string> param_names;
	std::string variadic_param;
	if (!match_symbol(")"))
	{
		do
		{
			if (peek().type != Token::Type::Identifier)
				return !make_error(peek(), "Expected parameter name").has_error;
			param_names.push_back(advance().text);
			if (match_symbol(":"))
			{
				advance();
			}
			if (match_symbol("..."))
			{
				variadic_param = param_names.back();
				break;
			}
		} while (match_symbol(","));
		if (!expect_symbol(")", "Expected ')' after parameters"))
			return false;
	}

	if (match_symbol("->"))
	{
		advance();
	}

	if (!expect_symbol("{", "Expected '{' to start function body"))
		return false;

	std::vector<UdonInstruction> body;
	FunctionContext fn_ctx;
	begin_scope(fn_ctx, body, false, &previous());
	for (const auto& p : param_names)
	{
		s32 slot = declare_variable(fn_ctx, p);
		fn_ctx.param_slot_indices.push_back(slot);
		if (!variadic_param.empty() && p == variadic_param)
			fn_ctx.variadic_slot_index = slot;
	}

	while (!is_end())
	{
		skip_semicolons();
		if (match_symbol("}"))
			break;
		if (!parse_statement(body, fn_ctx))
			return false;
	}
	if (is_end() && previous().text != "}")
	{
		return !make_error(previous(), "Missing closing '}'").has_error;
	}
	instructions[function_name] = std::make_shared<std::vector<UdonInstruction>>(body);
	params[function_name] = std::make_shared<std::vector<std::string>>(param_names);
	param_slots[function_name] = std::make_shared<std::vector<s32>>(fn_ctx.param_slot_indices);
	scope_sizes[function_name] = fn_ctx.root_slot_count();
	if (fn_ctx.variadic_slot_index >= 0)
		variadic_slot[function_name] = fn_ctx.variadic_slot_index;
	if (!variadic_param.empty())
		variadic[function_name] = variadic_param;
	if (is_event_handler)
	{
		events["on:" + on_target].push_back(function_name);
	}
	return true;
}

void Parser::emit(std::vector<UdonInstruction>& body,
	UdonInstruction::OpCode op,
	const std::vector<UdonValue>& operands,
	const Token* tok)
{
	UdonInstruction i{};
	i.opcode = op;
	i.operands = operands;
	const Token* loc_tok = tok;
	if (!loc_tok && current > 0)
		loc_tok = &previous();
	if (loc_tok)
	{
		i.line = loc_tok->line;
		i.column = loc_tok->column;
	}
	body.push_back(i);
}

bool Parser::parse_block(std::vector<UdonInstruction>& body, FunctionContext& ctx, bool create_scope)
{
	if (!expect_symbol("{", "Expected '{' to start block"))
		return false;
	if (create_scope)
		begin_scope(ctx, body, true, &previous());
	while (!is_end())
	{
		skip_semicolons();
		if (match_symbol("}"))
			break;
		if (!parse_statement(body, ctx))
			return false;
	}
	if (is_end())
		return !make_error(previous(), "Missing closing '}'").has_error;
	if (create_scope)
		end_scope(ctx, body);
	return true;
}

bool Parser::parse_statement_or_block(std::vector<UdonInstruction>& body, FunctionContext& ctx, bool create_scope)
{
	if (check_symbol("{"))
		return parse_block(body, ctx, create_scope);
	return parse_statement(body, ctx);
}

bool Parser::parse_statement(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	skip_semicolons();
	if (match_keyword("if"))
	{
		begin_scope(ctx, body, true, &previous());
		if (!expect_symbol("(", "Expected '(' after if"))
			return false;
		if (!parse_expression(body, ctx))
			return false;
		if (!expect_symbol(")", "Expected ')' after if condition"))
			return false;

		size_t jmp_false_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

		if (!parse_statement_or_block(body, ctx, false))
			return false;

		size_t jmp_end_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });

		body[jmp_false_index].operands[0].int_value = static_cast<s64>(body.size());

		skip_semicolons();
		if (match_keyword("else"))
		{
			if (!parse_statement_or_block(body, ctx, false))
				return false;
		}
		body[jmp_end_index].operands[0].int_value = static_cast<s64>(body.size());
		end_scope(ctx, body);
		return true;
	}
	if (match_keyword("while"))
	{
		begin_scope(ctx, body, true, &previous());
		if (!expect_symbol("(", "Expected '(' after while"))
			return false;
		size_t cond_index = body.size();
		if (!parse_expression(body, ctx))
			return false;
		if (!expect_symbol(")", "Expected ')' after while condition"))
			return false;

		size_t jmp_false_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

		loop_stack.push_back({});
		loop_stack.back().continue_target = static_cast<size_t>(cond_index);
		loop_stack.back().allow_continue = true;
		loop_stack.back().scope_depth = ctx.scope_stack.size();
		if (!parse_statement_or_block(body, ctx, false))
			return false;
		for (size_t ci : loop_stack.back().continue_jumps)
			body[ci].operands[0].int_value = static_cast<s64>(cond_index);
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(static_cast<s64>(cond_index)) });
		size_t exit_index = end_scope(ctx, body);
		body[jmp_false_index].operands[0].int_value = static_cast<s64>(exit_index);
		for (size_t bi : loop_stack.back().break_jumps)
			body[bi].operands[0].int_value = static_cast<s64>(exit_index);
		loop_stack.pop_back();
		return true;
	}

	if (match_keyword("for"))
	{
		if (!expect_symbol("(", "Expected '(' after for"))
			return false;
		begin_scope(ctx, body, true, &previous());

		if (!match_symbol(";"))
		{
			if (match_keyword("var"))
			{
				if (peek().type != Token::Type::Identifier)
					return !make_error(peek(), "Expected variable name").has_error;
				const std::string name = advance().text;
				declare_variable(ctx, name);
				ResolvedVariable init_var;
				resolve_variable(ctx, name, init_var);
				if (match_symbol(":"))
					advance();
				if (match_symbol("="))
				{
					if (!parse_expression(body, ctx))
						return false;
				}
				else
				{
					emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
				}
				emit_store_var(body, init_var);
				if (!expect_symbol(";", "Expected ';' after for init"))
					return false;
			}
			else
			{
				bool produced = false;
				if (!parse_assignment_or_expression(body, ctx, produced))
					return false;
				if (produced)
					emit(body, UdonInstruction::OpCode::POP);
				if (!expect_symbol(";", "Expected ';' after for init"))
					return false;
			}
		}

		size_t cond_index = body.size();
		if (!match_symbol(";"))
		{
			if (!parse_expression(body, ctx))
				return false;
			if (!expect_symbol(";", "Expected ';' after for condition"))
				return false;
		}
		else
		{
			emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
		}
		size_t jmp_false_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

		std::vector<UdonInstruction> increment_code;
		if (!match_symbol(")"))
		{
			bool produced = false;
			if (!parse_assignment_or_expression(increment_code, ctx, produced))
				return false;
			if (produced)
				emit(increment_code, UdonInstruction::OpCode::POP);
			if (!expect_symbol(")", "Expected ')' after for increment"))
				return false;
		}

		loop_stack.push_back({});
		loop_stack.back().allow_continue = true;
		loop_stack.back().scope_depth = ctx.scope_stack.size();
		if (!parse_statement_or_block(body, ctx))
			return false;
		size_t continue_target = body.size();
		for (size_t ci : loop_stack.back().continue_jumps)
			body[ci].operands[0].int_value = static_cast<s64>(continue_target);
		body.insert(body.end(), increment_code.begin(), increment_code.end());
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(static_cast<s64>(cond_index)) });
		size_t exit_index = end_scope(ctx, body);
		body[jmp_false_index].operands[0].int_value = static_cast<s64>(exit_index);
		for (size_t bi : loop_stack.back().break_jumps)
			body[bi].operands[0].int_value = static_cast<s64>(exit_index);
		loop_stack.pop_back();
		return true;
	}

	if (match_keyword("foreach"))
	{
		if (!expect_symbol("(", "Expected '(' after foreach"))
			return false;
		begin_scope(ctx, body, true, &previous());
		bool declared = match_keyword("var");
		if (peek().type != Token::Type::Identifier)
			return !make_error(peek(), "Expected iterator variable name").has_error;
		std::string key_name = advance().text;
		ResolvedVariable key_var;
		if (declared)
		{
			declare_variable(ctx, key_name);
			resolve_variable(ctx, key_name, key_var);
		}
		else if (!resolve_variable(ctx, key_name, key_var))
		{
			return !make_error(previous(), "Undeclared variable '" + key_name + "'").has_error;
		}

		std::string value_name;
		ResolvedVariable value_var;
		bool has_value = false;
		if (match_symbol(","))
		{
			if (peek().type != Token::Type::Identifier)
				return !make_error(peek(), "Expected UdonValue variable name after ','").has_error;
			value_name = advance().text;
			if (declared)
			{
				declare_variable(ctx, value_name);
				resolve_variable(ctx, value_name, value_var);
			}
			else if (!resolve_variable(ctx, value_name, value_var))
			{
				return !make_error(previous(), "Undeclared variable '" + value_name + "'").has_error;
			}
			has_value = true;
		}

		if (!match_keyword("in"))
			return !make_error(peek(), "Expected 'in' in foreach").has_error;
		std::string collection_tmp = "__foreach_coll_" + std::to_string(body.size());
		std::string keys_tmp = "__foreach_keys_" + std::to_string(body.size());
		std::string idx_tmp = "__foreach_i_" + std::to_string(body.size());
		declare_variable(ctx, collection_tmp);
		declare_variable(ctx, keys_tmp);
		declare_variable(ctx, idx_tmp);
		ResolvedVariable coll_var;
		ResolvedVariable keys_var;
		ResolvedVariable idx_var;
		resolve_variable(ctx, collection_tmp, coll_var);
		resolve_variable(ctx, keys_tmp, keys_var);
		resolve_variable(ctx, idx_tmp, idx_var);

		if (!parse_expression(body, ctx))
			return false;
		emit_store_var(body, coll_var);

		emit_load_var(body, coll_var);
		std::vector<UdonValue> call_ops;
		call_ops.push_back(make_string("keys"));
		call_ops.push_back(make_int(1));
		call_ops.push_back(make_string(""));
		emit(body, UdonInstruction::OpCode::CALL, call_ops);
		emit_store_var(body, keys_var);

		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(0) });
		emit_store_var(body, idx_var);

		if (!expect_symbol(")", "Expected ')' after foreach header"))
			return false;

		size_t cond_index = body.size();
		emit_load_var(body, idx_var);
		emit_load_var(body, keys_var);
		std::vector<UdonValue> len_ops;
		len_ops.push_back(make_string("len"));
		len_ops.push_back(make_int(1));
		len_ops.push_back(make_string(""));
		emit(body, UdonInstruction::OpCode::CALL, len_ops);
		emit(body, UdonInstruction::OpCode::LT);
		size_t jmp_false_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

		emit_load_var(body, keys_var);
		emit_load_var(body, idx_var);
		std::vector<UdonValue> get_key_ops;
		get_key_ops.push_back(make_string("array_get"));
		get_key_ops.push_back(make_int(2));
		get_key_ops.push_back(make_string(""));
		get_key_ops.push_back(make_string(""));
		emit(body, UdonInstruction::OpCode::CALL, get_key_ops);
		emit_store_var(body, key_var);

		if (has_value)
		{
			emit_load_var(body, coll_var);
			emit_load_var(body, key_var);
			std::vector<UdonValue> get_val_ops;
			get_val_ops.push_back(make_string("array_get"));
			get_val_ops.push_back(make_int(2));
			get_val_ops.push_back(make_string(""));
			get_val_ops.push_back(make_string(""));
			emit(body, UdonInstruction::OpCode::CALL, get_val_ops);
			emit_store_var(body, value_var);
		}

		loop_stack.push_back({});
		loop_stack.back().allow_continue = true;
		loop_stack.back().scope_depth = ctx.scope_stack.size();
		if (!parse_block(body, ctx))
			return false;

		size_t continue_target = body.size();
		for (size_t ci : loop_stack.back().continue_jumps)
			body[ci].operands[0].int_value = static_cast<s64>(continue_target);

		emit_load_var(body, idx_var);
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
		emit(body, UdonInstruction::OpCode::ADD);
		emit_store_var(body, idx_var);

		emit(body, UdonInstruction::OpCode::JUMP, { make_int(static_cast<s64>(cond_index)) });
		size_t exit_index = end_scope(ctx, body);
		body[jmp_false_index].operands[0].int_value = static_cast<s64>(exit_index);
		for (size_t bi : loop_stack.back().break_jumps)
			body[bi].operands[0].int_value = static_cast<s64>(exit_index);
		loop_stack.pop_back();
		return true;
	}

	if (match_keyword("switch"))
	{
		if (!expect_symbol("(", "Expected '(' after switch"))
			return false;
		begin_scope(ctx, body, true, &previous());
		std::string tmp_name = "__switch_val_" + std::to_string(body.size());
		declare_variable(ctx, tmp_name);
		ResolvedVariable tmp_var;
		resolve_variable(ctx, tmp_name, tmp_var);
		if (!parse_expression(body, ctx))
			return false;
		if (!expect_symbol(")", "Expected ')' after switch expression"))
			return false;
		emit_store_var(body, tmp_var);
		if (!expect_symbol("{", "Expected '{' after switch header"))
			return false;

		loop_stack.push_back({});
		loop_stack.back().allow_continue = false;
		loop_stack.back().scope_depth = ctx.scope_stack.size();

		bool has_default = false;
		while (!is_end() && !check_symbol("}"))
		{
			skip_semicolons();
			if (check_symbol("}"))
				break;
			if (match_keyword("case"))
			{
				if (peek().type != Token::Type::Number && peek().type != Token::Type::String && peek().type != Token::Type::Identifier && peek().type != Token::Type::Keyword)
					return !make_error(peek(), "Expected literal after case").has_error;
				UdonValue case_val;
				if (peek().type == Token::Type::Number)
				{
					const std::string num_text = advance().text;
					const bool is_float = num_text.find('.') != std::string::npos || num_text.find('e') != std::string::npos || num_text.find('E') != std::string::npos;
					case_val = is_float ? make_float(std::stod(num_text)) : make_int(std::stoll(num_text));
				}
				else if (peek().type == Token::Type::String)
				{
					case_val = make_string(advance().text);
				}
				else
				{
					std::string kw = advance().text;
					if (kw == "true")
						case_val = make_bool(true);
					else if (kw == "false")
						case_val = make_bool(false);
					else
						case_val = make_string(kw);
				}
				if (!expect_symbol(":", "Expected ':' after case UdonValue"))
					return false;

				emit_load_var(body, tmp_var);
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { case_val });
				emit(body, UdonInstruction::OpCode::EQ);
				size_t jz_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

				while (!is_end())
				{
					skip_semicolons();
					if (check_symbol("}"))
						break;
					if (peek().type == Token::Type::Keyword && (peek().text == "case" || peek().text == "default"))
						break;
					if (!parse_statement(body, ctx))
						return false;
				}

				size_t end_jump = body.size();
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
				body[jz_index].operands[0].int_value = static_cast<s64>(body.size());
				loop_stack.back().break_jumps.push_back(end_jump);
			}
			else if (match_keyword("default"))
			{
				if (has_default)
					return !make_error(previous(), "Multiple default labels").has_error;
				has_default = true;
				if (!expect_symbol(":", "Expected ':' after default"))
					return false;
				while (!is_end())
				{
					skip_semicolons();
					if (check_symbol("}"))
						break;
					if (peek().type == Token::Type::Keyword && (peek().text == "case"))
						break;
					if (!parse_statement(body, ctx))
						return false;
				}
			}
			else
			{
				return !make_error(peek(), "Expected case/default or '}' in switch").has_error;
			}
		}
		if (!expect_symbol("}", "Expected '}' to close switch"))
			return false;
		size_t exit_index = end_scope(ctx, body);
		for (size_t bi : loop_stack.back().break_jumps)
			body[bi].operands[0].int_value = static_cast<s64>(exit_index);
		loop_stack.pop_back();
		return true;
	}
	if (match_keyword("return"))
	{
		size_t value_count = 0;
		if (match_symbol("("))
		{
			if (match_symbol(")"))
				return !make_error(previous(), "return requires a UdonValue").has_error;
			do
			{
				if (!parse_expression(body, ctx))
					return false;
				value_count++;
			} while (match_symbol(","));
			if (!expect_symbol(")", "Expected ')' after return UdonValue"))
				return false;
		}
		else
		{
			if (check_symbol("}") || is_end())
				return !make_error(previous(), "return requires a UdonValue").has_error;
			if (!parse_expression(body, ctx))
				return false;
			value_count = 1;
		}
		if (value_count == 0)
		{
			return !make_error(previous(), "return requires a UdonValue").has_error;
		}
		else if (value_count > 1)
		{
			std::vector<UdonValue> ops;
			ops.push_back(make_string("array"));
			ops.push_back(make_int(static_cast<s64>(value_count)));
			for (size_t i = 0; i < value_count; ++i)
				ops.push_back(make_string(""));
			emit(body, UdonInstruction::OpCode::CALL, ops);
		}
		emit(body, UdonInstruction::OpCode::RETURN);
		return true;
	}

	if (match_keyword("break"))
	{
		if (loop_stack.empty())
			return !make_error(previous(), "break outside of loop/switch").has_error;
		size_t target_depth = loop_stack.back().scope_depth;
		emit_unwind_to_depth(ctx, body, target_depth);
		size_t jmp_idx = body.size();
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
		loop_stack.back().break_jumps.push_back(jmp_idx);
		return true;
	}

	if (match_keyword("continue"))
	{
		if (loop_stack.empty() || !loop_stack.back().allow_continue)
			return !make_error(previous(), "continue outside of loop").has_error;
		size_t target_depth = loop_stack.back().scope_depth;
		emit_unwind_to_depth(ctx, body, target_depth);
		size_t jmp_idx = body.size();
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
		loop_stack.back().continue_jumps.push_back(jmp_idx);
		return true;
	}

	bool produced = false;
	if (!parse_assignment_or_expression(body, ctx, produced))
		return false;
	if (produced)
		emit(body, UdonInstruction::OpCode::POP);
	return true;
}

bool Parser::parse_expression(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	bool produced = true;
	return parse_assignment_or_expression(body, ctx, produced);
}

bool Parser::parse_ternary(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_or(body, ctx))
		return false;
	while (match_symbol("?"))
	{
		size_t jmp_false = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });
		bool prev_stop = stop_at_colon;
		stop_at_colon = true;
		if (!parse_expression(body, ctx))
			return false;
		stop_at_colon = prev_stop;
		size_t jmp_end = body.size();
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
		if (!expect_symbol(":", "Expected ':' in ternary expression"))
			return false;
		size_t else_index = body.size();
		body[jmp_false].operands[0].int_value = static_cast<s64>(else_index);
		prev_stop = stop_at_colon;
		stop_at_colon = true;
		if (!parse_expression(body, ctx))
			return false;
		stop_at_colon = prev_stop;
		size_t end_index = body.size();
		body[jmp_end].operands[0].int_value = static_cast<s64>(end_index);
	}
	return true;
}

bool Parser::parse_or(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_and(body, ctx))
		return false;
	while (match_symbol("||"))
	{
		emit(body, UdonInstruction::OpCode::TO_BOOL);
		size_t jz_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_bool(true) });
		size_t jmp_end = body.size();
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
		body[jz_index].operands[0].int_value = static_cast<s64>(body.size());
		if (!parse_and(body, ctx))
			return false;
		emit(body, UdonInstruction::OpCode::TO_BOOL);
		body[jmp_end].operands[0].int_value = static_cast<s64>(body.size());
	}
	return true;
}

bool Parser::parse_and(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_equality(body, ctx))
		return false;
	while (match_symbol("&&"))
	{
		emit(body, UdonInstruction::OpCode::TO_BOOL);
		size_t jz_index = body.size();
		emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });
		if (!parse_equality(body, ctx))
			return false;
		emit(body, UdonInstruction::OpCode::TO_BOOL);
		size_t jmp_end = body.size();
		emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
		body[jz_index].operands[0].int_value = static_cast<s64>(body.size());
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_bool(false) });
		body[jmp_end].operands[0].int_value = static_cast<s64>(body.size());
	}
	return true;
}

bool Parser::parse_equality(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_comparison(body, ctx))
		return false;
	while (true)
	{
		if (match_symbol("=="))
		{
			if (!parse_comparison(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::EQ);
		}
		else if (match_symbol("!="))
		{
			if (!parse_comparison(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::NEQ);
		}
		else
		{
			break;
		}
	}
	return true;
}

bool Parser::parse_comparison(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_additive(body, ctx))
		return false;
	while (true)
	{
		if (match_symbol("<"))
		{
			if (!parse_additive(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::LT);
		}
		else if (match_symbol(">"))
		{
			if (!parse_additive(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::GT);
		}
		else if (match_symbol("<="))
		{
			if (!parse_additive(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::LTE);
		}
		else if (match_symbol(">="))
		{
			if (!parse_additive(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::GTE);
		}
		else
		{
			break;
		}
	}
	return true;
}

bool Parser::parse_assignment_or_expression(std::vector<UdonInstruction>& body, FunctionContext& ctx, bool& produced_value)
{
	auto destructure_assign = [&](const std::vector<std::string>& names, bool allow_new, bool push_first_value) -> bool
	{
		const std::string tmp_name = "__tuple_tmp_" + std::to_string(body.size());
		declare_variable(ctx, tmp_name);
		ResolvedVariable tmp_var;
		resolve_variable(ctx, tmp_name, tmp_var);
		emit_store_var(body, tmp_var);

		auto load_element = [&](size_t idx, bool use_index)
		{
			emit_load_var(body, tmp_var);
			if (use_index)
			{
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(static_cast<s64>(idx)) });
				emit(body, UdonInstruction::OpCode::GET_PROP, { make_string("[index]") });
			}
		};

		const bool use_indexing = names.size() > 1;
		for (size_t i = 0; i < names.size(); ++i)
		{
			const std::string& name = names[i];
			if (name == "_")
				continue;
			ResolvedVariable target;
			if (!allow_new && !resolve_variable(ctx, name, target))
				return !make_error(previous(), "Undeclared variable '" + name + "'").has_error;
			if (allow_new)
			{
				declare_variable(ctx, name);
				resolve_variable(ctx, name, target);
			}
			load_element(i, use_indexing);
			emit_store_var(body, target);
		}

		if (push_first_value)
		{
			if (names.empty())
			{
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
			}
			else
			{
				load_element(0, use_indexing);
			}
		}
		return true;
	};

	auto compound_opcode_for = [](const std::string& op, UdonInstruction::OpCode& out) -> bool
	{
		if (op == "+=")
			out = UdonInstruction::OpCode::ADD;
		else if (op == "-=")
			out = UdonInstruction::OpCode::SUB;
		else if (op == "*=")
			out = UdonInstruction::OpCode::MUL;
		else if (op == "/=")
			out = UdonInstruction::OpCode::DIV;
		else
			return false;
		return true;
	};

	auto make_temp_var = [&](const std::string& base_prefix) -> ResolvedVariable
	{
		std::string name = base_prefix;
		ResolvedVariable tmp{};
		int suffix = 0;
		while (is_declared(ctx, name, &tmp))
			name = base_prefix + "_" + std::to_string(suffix++);
		declare_variable(ctx, name);
		resolve_variable(ctx, name, tmp);
		return tmp;
	};

	if (match_keyword("var"))
	{
		std::vector<std::string> names;
		do
		{
			if (peek().type != Token::Type::Identifier)
				return !make_error(peek(), "Expected variable name").has_error;
			names.push_back(advance().text);
			if (match_symbol(":"))
				advance();
		} while (match_symbol(","));

		if (match_symbol("="))
		{
			if (!parse_expression(body, ctx))
				return false;
			if (!destructure_assign(names, true, true))
				return false;
		}
		else
		{
			for (const auto& n : names)
			{
				if (n == "_")
					continue;
				declare_variable(ctx, n);
				ResolvedVariable var_ref;
				resolve_variable(ctx, n, var_ref);
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
				emit_store_var(body, var_ref);
			}
			emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
		}
		produced_value = true;
		return true;
	}

	if (peek().type == Token::Type::Identifier)
	{
		size_t lookahead = current;
		std::vector<std::string> names;
		bool saw_comma = false;
		while (lookahead < tokens.size() && tokens[lookahead].type == Token::Type::Identifier)
		{
			names.push_back(tokens[lookahead].text);
			lookahead++;
			if (lookahead < tokens.size() && tokens[lookahead].type == Token::Type::Symbol && tokens[lookahead].text == ",")
			{
				saw_comma = true;
				lookahead++;
				continue;
			}
			break;
		}
		if (saw_comma && lookahead < tokens.size() && tokens[lookahead].type == Token::Type::Symbol && tokens[lookahead].text == "=")
		{
			names.clear();
			do
			{
				names.push_back(advance().text);
			} while (match_symbol(","));
			if (!expect_symbol("=", "Expected '=' in destructuring assignment"))
				return false;
			if (!parse_expression(body, ctx))
				return false;
			if (!destructure_assign(names, false, true))
				return false;
			produced_value = true;
			return true;
		}
	}

	if (peek().type == Token::Type::Identifier && tokens.size() > current + 2)
	{
		size_t la = current;
		std::string base_name = tokens[la].text;
		++la;
		std::vector<std::string> prop_chain;
		while (la + 1 < tokens.size() && tokens[la].type == Token::Type::Symbol && tokens[la].text == ":" &&
			   (tokens[la + 1].type == Token::Type::Identifier || tokens[la + 1].type == Token::Type::String || tokens[la + 1].type == Token::Type::Number))
		{
			prop_chain.push_back(tokens[la + 1].text);
			la += 2;
		}
		if (!prop_chain.empty() && la < tokens.size() && tokens[la].type == Token::Type::Symbol && tokens[la].text == "[")
		{
			int bracket_depth = 1;
			size_t bracket_end = la + 1;
			while (bracket_end < tokens.size() && bracket_depth > 0)
			{
				if (tokens[bracket_end].type == Token::Type::Symbol)
				{
					if (tokens[bracket_end].text == "[")
						++bracket_depth;
					else if (tokens[bracket_end].text == "]")
						--bracket_depth;
				}
				if (bracket_depth > 0)
					++bracket_end;
			}
			if (bracket_depth == 0 && bracket_end + 1 < tokens.size() &&
				tokens[bracket_end + 1].type == Token::Type::Symbol)
			{
				const std::string assign_op = tokens[bracket_end + 1].text;
				UdonInstruction::OpCode compound_op{};
				bool is_compound = compound_opcode_for(assign_op, compound_op);
				if (assign_op == "=" || is_compound)
				{
					advance(); // base identifier
					ResolvedVariable base_ref;
					if (!resolve_variable(ctx, base_name, base_ref))
						return !make_error(previous(), "Undeclared variable '" + base_name + "'").has_error;
					for (size_t i = 0; i < prop_chain.size(); ++i)
					{
						advance(); // ':'
						advance(); // prop token
					}
					advance(); // '['
					emit_load_var(body, base_ref);
					for (const auto& prop : prop_chain)
						emit(body, UdonInstruction::OpCode::GET_PROP, { make_string(prop) });
					if (!parse_expression(body, ctx))
						return false;
					if (!expect_symbol("]", "Expected ']' after index"))
						return false;

					if (is_compound)
					{
						ResolvedVariable obj_tmp = make_temp_var("__tmp_obj_" + std::to_string(body.size()));
						ResolvedVariable idx_tmp = make_temp_var("__tmp_idx_" + std::to_string(body.size()));
						ResolvedVariable res_tmp = make_temp_var("__tmp_res_" + std::to_string(body.size()));
						emit_store_var(body, idx_tmp);
						emit_store_var(body, obj_tmp);
						advance(); // operator
						emit_load_var(body, obj_tmp);
						emit_load_var(body, idx_tmp);
						emit(body, UdonInstruction::OpCode::GET_PROP, { make_string("[index]") });
						if (!parse_expression(body, ctx))
							return false;
						emit(body, compound_op);
						emit_store_var(body, res_tmp);
						emit_load_var(body, obj_tmp);
						emit_load_var(body, idx_tmp);
						emit_load_var(body, res_tmp);
						emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string("[index]") });
					}
					else
					{
						advance(); // '='
						if (!parse_expression(body, ctx))
							return false;
						emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string("[index]") });
					}
					produced_value = false;
					return true;
				}
			}
		}
		if (!prop_chain.empty() && la < tokens.size() && tokens[la].type == Token::Type::Symbol)
		{
			const std::string assign_op = tokens[la].text;
			UdonInstruction::OpCode compound_op{};
			bool is_compound = compound_opcode_for(assign_op, compound_op);
			if (assign_op == "=" || is_compound)
			{
				advance(); // base identifier
				ResolvedVariable base_ref;
				if (!resolve_variable(ctx, base_name, base_ref))
					return !make_error(previous(), "Undeclared variable '" + base_name + "'").has_error;
				for (size_t i = 0; i < prop_chain.size(); ++i)
				{
					advance(); // ':'
					advance(); // prop token
				}
				advance(); // assignment operator
				emit_load_var(body, base_ref);
				for (size_t i = 0; i + 1 < prop_chain.size(); ++i)
					emit(body, UdonInstruction::OpCode::GET_PROP, { make_string(prop_chain[i]) });
				if (!is_compound)
				{
					if (!parse_expression(body, ctx))
						return false;
					emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string(prop_chain.back()) });
				}
				else
				{
					ResolvedVariable obj_tmp = make_temp_var("__tmp_obj_" + std::to_string(body.size()));
					ResolvedVariable res_tmp = make_temp_var("__tmp_res_" + std::to_string(body.size()));
					emit_store_var(body, obj_tmp);
					emit_load_var(body, obj_tmp);
					emit(body, UdonInstruction::OpCode::GET_PROP, { make_string(prop_chain.back()) });
					if (!parse_expression(body, ctx))
						return false;
					emit(body, compound_op);
					emit_store_var(body, res_tmp);
					emit_load_var(body, obj_tmp);
					emit_load_var(body, res_tmp);
					emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string(prop_chain.back()) });
				}
				produced_value = false;
				return true;
			}
		}

		size_t lookahead = current + 1;
		if (tokens[lookahead].type == Token::Type::Symbol)
		{
			std::string next_sym = tokens[lookahead].text;
			if (next_sym == ":" && lookahead + 2 < tokens.size())
			{
				if (tokens[lookahead + 2].type == Token::Type::Symbol)
				{
					const std::string assign_op = tokens[lookahead + 2].text;
					UdonInstruction::OpCode compound_op{};
					bool is_compound = compound_opcode_for(assign_op, compound_op);
					if (assign_op == "=" || is_compound)
					{
						std::string obj_name = advance().text;
						ResolvedVariable obj_ref;
						if (!resolve_variable(ctx, obj_name, obj_ref))
							return !make_error(previous(), "Undeclared variable '" + obj_name + "'").has_error;
						advance(); // ':'
						if (peek().type != Token::Type::Identifier && peek().type != Token::Type::String && peek().type != Token::Type::Number)
							return !make_error(peek(), "Expected property name after ':'").has_error;
						std::string prop_name = advance().text;
						advance(); // assignment operator
						emit_load_var(body, obj_ref);
						if (!is_compound)
						{
							if (!parse_expression(body, ctx))
								return false;
							emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string(prop_name) });
						}
						else
						{
							ResolvedVariable obj_tmp = make_temp_var("__tmp_obj_" + std::to_string(body.size()));
							ResolvedVariable res_tmp = make_temp_var("__tmp_res_" + std::to_string(body.size()));
							emit_store_var(body, obj_tmp);
							emit_load_var(body, obj_tmp);
							emit(body, UdonInstruction::OpCode::GET_PROP, { make_string(prop_name) });
							if (!parse_expression(body, ctx))
								return false;
							emit(body, compound_op);
							emit_store_var(body, res_tmp);
							emit_load_var(body, obj_tmp);
							emit_load_var(body, res_tmp);
							emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string(prop_name) });
						}
						produced_value = false;
						return true;
					}
				}
			}
			else if (next_sym == "[")
			{
				int bracket_depth = 1;
				size_t bracket_end = lookahead + 1;
				while (bracket_end < tokens.size() && bracket_depth > 0)
				{
					if (tokens[bracket_end].type == Token::Type::Symbol)
					{
						if (tokens[bracket_end].text == "[")
							bracket_depth++;
						else if (tokens[bracket_end].text == "]")
							bracket_depth--;
					}
					if (bracket_depth > 0)
						bracket_end++;
				}
				if (bracket_depth == 0 && bracket_end + 1 < tokens.size() &&
					tokens[bracket_end + 1].type == Token::Type::Symbol)
				{
					const std::string assign_op = tokens[bracket_end + 1].text;
					UdonInstruction::OpCode compound_op{};
					bool is_compound = compound_opcode_for(assign_op, compound_op);
					if (assign_op == "=" || is_compound)
					{
						std::string obj_name = advance().text;
						ResolvedVariable obj_ref;
						if (!resolve_variable(ctx, obj_name, obj_ref))
							return !make_error(previous(), "Undeclared variable '" + obj_name + "'").has_error;
						advance(); // '['
						emit_load_var(body, obj_ref);
						if (!parse_expression(body, ctx))
							return false;
						if (!expect_symbol("]", "Expected ']' after index"))
							return false;

						if (is_compound)
						{
							ResolvedVariable idx_tmp = make_temp_var("__tmp_idx_" + std::to_string(body.size()));
							ResolvedVariable obj_tmp = make_temp_var("__tmp_obj_" + std::to_string(body.size()));
							ResolvedVariable res_tmp = make_temp_var("__tmp_res_" + std::to_string(body.size()));
							emit_store_var(body, idx_tmp);
							emit_store_var(body, obj_tmp);
							advance(); // operator
							emit_load_var(body, obj_tmp);
							emit_load_var(body, idx_tmp);
							emit(body, UdonInstruction::OpCode::GET_PROP, { make_string("[index]") });
							if (!parse_expression(body, ctx))
								return false;
							emit(body, compound_op);
							emit_store_var(body, res_tmp);
							emit_load_var(body, obj_tmp);
							emit_load_var(body, idx_tmp);
							emit_load_var(body, res_tmp);
							emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string("[index]") });
						}
						else
						{
							advance(); // '='
							if (!parse_expression(body, ctx))
								return false;
							emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string("[index]") });
						}
						produced_value = false;
						return true;
					}
				}
			}
		}
	}

	if (peek().type == Token::Type::Identifier && tokens.size() > current + 1 && tokens[current + 1].type == Token::Type::Symbol)
	{
		const std::string name = advance().text;
		std::string op = tokens[current].text;
		UdonInstruction::OpCode compound_op{};
		if (op == "=" || compound_opcode_for(op, compound_op))
		{
			ResolvedVariable var_ref;
			if (!resolve_variable(ctx, name, var_ref))
				return !make_error(previous(), "Undeclared variable '" + name + "'").has_error;
			advance();
			if (op != "=")
				emit_load_var(body, var_ref);
			if (!parse_expression(body, ctx))
				return false;
			if (op != "=")
				emit(body, compound_op);
			emit_store_var(body, var_ref);
			emit_load_var(body, var_ref); // leave assigned value on stack
			produced_value = true;
			return true;
		}
		current--; // rewind name consumption
	}
	produced_value = true;
	return parse_ternary(body, ctx);
}

bool Parser::parse_additive(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_multiplicative(body, ctx))
		return false;
	while (true)
	{
		if (match_symbol("+"))
		{
			if (!parse_multiplicative(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::ADD);
		}
		else if (match_symbol("-"))
		{
			if (!parse_multiplicative(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::SUB);
		}
		else if (match_symbol(".."))
		{
			if (!parse_multiplicative(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::CONCAT);
		}
		else
		{
			break;
		}
	}
	return true;
}

bool Parser::parse_multiplicative(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!parse_unary(body, ctx))
		return false;
	while (true)
	{
		if (match_symbol("*"))
		{
			if (!parse_unary(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::MUL);
		}
		else if (match_symbol("/"))
		{
			if (!parse_unary(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::DIV);
		}
		else if (match_symbol("%"))
		{
			if (!parse_unary(body, ctx))
				return false;
			emit(body, UdonInstruction::OpCode::MOD);
		}
		else
		{
			break;
		}
	}
	return true;
}

bool Parser::parse_unary(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (match_symbol("-"))
	{
		if (!parse_unary(body, ctx))
			return false;
		emit(body, UdonInstruction::OpCode::NEGATE);
		return true;
	}
	if (match_symbol("!"))
	{
		if (!parse_unary(body, ctx))
			return false;
		emit(body, UdonInstruction::OpCode::TO_BOOL);
		emit(body, UdonInstruction::OpCode::LOGICAL_NOT);
		return true;
	}
	if (match_symbol("++") || match_symbol("--"))
	{
		bool inc = previous().text == "++";
		if (peek().type != Token::Type::Identifier)
			return !make_error(peek(), "Expected identifier after increment").has_error;
		std::string name = advance().text;
		ResolvedVariable var_ref;
		if (!resolve_variable(ctx, name, var_ref))
			return !make_error(previous(), "Undeclared variable '" + name + "'").has_error;
		emit_load_var(body, var_ref);
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
		emit(body, inc ? UdonInstruction::OpCode::ADD : UdonInstruction::OpCode::SUB);
		emit_store_var(body, var_ref);
		emit_load_var(body, var_ref);
		return true;
	}
	return parse_primary(body, ctx);
}

bool Parser::parse_postfix(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	while (true)
	{
		if (stop_at_colon && check_symbol(":"))
			break;
		if (match_symbol("."))
		{
			if (!parse_method_postfix(body, ctx))
				return false;
			continue;
		}
		if (match_symbol(":"))
		{
			if (!parse_key_postfix(body))
				return false;
			continue;
		}
		if (match_symbol("["))
		{
			if (!parse_expression(body, ctx))
				return false;
			if (!expect_symbol("]", "Expected ']' after index"))
				return false;
			emit(body, UdonInstruction::OpCode::GET_PROP, { make_string("[index]") });
			continue;
		}
		if (match_symbol("("))
		{
			size_t arg_count = 0;
			if (!match_symbol(")"))
			{
				do
				{
					if (!parse_expression(body, ctx))
						return false;
					arg_count++;
				} while (match_symbol(","));
				if (!expect_symbol(")", "Expected ')' after arguments"))
					return false;
			}
			std::vector<UdonValue> operands;
			operands.push_back(make_string("")); // empty callee signals dynamic call using callable on stack
			operands.push_back(make_int(static_cast<s64>(arg_count)));
			emit(body, UdonInstruction::OpCode::CALL, operands);
			continue;
		}
		break;
	}
	return true;
}

bool Parser::parse_method_postfix(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (peek().type != Token::Type::Identifier)
		return !make_error(peek(), "Expected member name after '.'").has_error;
	std::string member = advance().text;
	if (!match_symbol("("))
		return !make_error(peek(), "Expected '(' after method access").has_error;

	std::vector<std::string> arg_names;
	size_t arg_count = 1; // receiver already on stack
	arg_names.push_back("");
	if (!match_symbol(")"))
	{
		do
		{
			std::string arg_name;
			if (peek().type == Token::Type::Identifier && tokens.size() > current + 1 && tokens[current + 1].type == Token::Type::Symbol && tokens[current + 1].text == "=")
			{
				arg_name = advance().text;
				advance(); // '='
			}
			if (!parse_expression(body, ctx))
				return false;
			arg_names.push_back(arg_name);
			arg_count++;
		} while (match_symbol(","));
		if (!expect_symbol(")", "Expected ')' after arguments"))
			return false;
	}
	std::vector<UdonValue> operands;
	operands.push_back(make_string(member));
	operands.push_back(make_int(static_cast<s64>(arg_count)));
	for (const auto& n : arg_names)
		operands.push_back(make_string(n));
	emit(body, UdonInstruction::OpCode::CALL, operands);
	return true;
}

bool Parser::parse_key_postfix(std::vector<UdonInstruction>& body)
{
	if (peek().type != Token::Type::Identifier && peek().type != Token::Type::String && peek().type != Token::Type::Number)
		return !make_error(peek(), "Expected key after ':'").has_error;
	std::string key = advance().text;
	emit(body, UdonInstruction::OpCode::GET_PROP, { make_string(key) });
	return true;
}

bool Parser::parse_function_literal(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (!expect_symbol("(", "Expected '(' after function"))
		return false;

	std::vector<std::string> param_names;
	std::string variadic_param;
	if (!match_symbol(")"))
	{
		do
		{
			if (peek().type != Token::Type::Identifier)
				return !make_error(peek(), "Expected parameter name").has_error;
			param_names.push_back(advance().text);
			if (match_symbol(":"))
			{
				advance();
			}
			if (match_symbol("..."))
			{
				variadic_param = param_names.back();
				break;
			}
		} while (match_symbol(","));
		if (!expect_symbol(")", "Expected ')' after parameters"))
			return false;
	}

	if (match_symbol("->"))
	{
		advance();
	}

	if (!expect_symbol("{", "Expected '{' to start function body"))
		return false;

	std::vector<UdonInstruction> fn_body;
	FunctionContext fn_ctx;
	for (auto it = ctx.scope_stack.rbegin(); it != ctx.scope_stack.rend(); ++it)
		fn_ctx.enclosing_scopes.push_back(it->scope);
	for (const auto& enc : ctx.enclosing_scopes)
		fn_ctx.enclosing_scopes.push_back(enc);

	begin_scope(fn_ctx, fn_body, false, &previous());
	for (const auto& p : param_names)
	{
		s32 slot = declare_variable(fn_ctx, p);
		fn_ctx.param_slot_indices.push_back(slot);
		if (!variadic_param.empty() && p == variadic_param)
			fn_ctx.variadic_slot_index = slot;
	}

	LoopGuard loop_guard(loop_stack, true);

	while (!is_end())
	{
		skip_semicolons();
		if (match_symbol("}"))
			break;
		if (!parse_statement(fn_body, fn_ctx))
		{
			return false;
		}
	}
	if (is_end() && previous().text != "}")
	{
		return !make_error(previous(), "Missing closing '}'").has_error;
	}
	std::string fn_name = "__lambda_" + std::to_string(lambda_counter++);
	instructions[fn_name] = std::make_shared<std::vector<UdonInstruction>>(fn_body);
	params[fn_name] = std::make_shared<std::vector<std::string>>(param_names);
	param_slots[fn_name] = std::make_shared<std::vector<s32>>(fn_ctx.param_slot_indices);
	scope_sizes[fn_name] = fn_ctx.root_slot_count();
	if (fn_ctx.variadic_slot_index >= 0)
		variadic_slot[fn_name] = fn_ctx.variadic_slot_index;
	if (!variadic_param.empty())
		variadic[fn_name] = variadic_param;

	emit(body, UdonInstruction::OpCode::MAKE_CLOSURE, { make_string(fn_name) });
	return true;
}

bool Parser::parse_primary(std::vector<UdonInstruction>& body, FunctionContext& ctx)
{
	if (match_keyword("function"))
	{
		if (!parse_function_literal(body, ctx))
			return false;
		return parse_postfix(body, ctx);
	}

	if (peek().type == Token::Type::Number)
	{
		const std::string num_text = advance().text;
		const bool is_float = num_text.find('.') != std::string::npos || num_text.find('e') != std::string::npos || num_text.find('E') != std::string::npos;
		if (is_float)
			emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_float(std::stod(num_text)) });
		else
			emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(std::stoll(num_text)) });
		return parse_postfix(body, ctx);
	}

	if (peek().type == Token::Type::String)
	{
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(advance().text) });
		return parse_postfix(body, ctx);
	}

	if (peek().type == Token::Type::Identifier)
	{
		Token ident = advance();
		ResolvedVariable var_ref;
		bool has_var = resolve_variable(ctx, ident.text, var_ref);
		if (match_symbol("("))
		{
			bool dynamic_call = has_var;
			if (dynamic_call)
				emit_load_var(body, var_ref);
			std::vector<std::string> arg_names;
			size_t arg_count = 0;
			if (!match_symbol(")"))
			{
				do
				{
					std::string arg_name;
					if (!dynamic_call && peek().type == Token::Type::Identifier && tokens.size() > current + 1 && tokens[current + 1].type == Token::Type::Symbol && tokens[current + 1].text == "=")
					{
						arg_name = advance().text;
						advance(); // '='
					}
					if (!parse_expression(body, ctx))
						return false;
					arg_names.push_back(arg_name);
					arg_count++;
				} while (match_symbol(","));
				if (!expect_symbol(")", "Expected ')' after arguments"))
					return false;
			}

			std::vector<UdonValue> operands;
			if (dynamic_call)
			{
				operands.push_back(make_string(""));
			}
			else
			{
				operands.push_back(make_string(ident.text));
			}
			operands.push_back(make_int(static_cast<s64>(arg_count)));
			if (!dynamic_call)
			{
				for (const auto& n : arg_names)
					operands.push_back(make_string(n));
			}
			emit(body, UdonInstruction::OpCode::CALL, operands);
			return parse_postfix(body, ctx);
		}

		if (!has_var)
			return !make_error(previous(), "Undeclared variable '" + ident.text + "'").has_error;
		emit_load_var(body, var_ref);
		if (match_symbol("++") || match_symbol("--"))
		{
			bool inc = previous().text == "++";
			emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
			emit(body, inc ? UdonInstruction::OpCode::ADD : UdonInstruction::OpCode::SUB);
			emit_store_var(body, var_ref);
			emit_load_var(body, var_ref);
		}
		return parse_postfix(body, ctx);
	}

	if (match_symbol("["))
	{
		int count = 0;
		if (match_symbol("]"))
		{
			std::vector<UdonValue> ops;
			ops.push_back(make_string("array"));
			ops.push_back(make_int(0));
			emit(body, UdonInstruction::OpCode::CALL, ops);
			return parse_postfix(body, ctx);
		}

		do
		{
			if (!parse_expression(body, ctx))
				return false;
			count++;
		} while (match_symbol(","));

		if (!expect_symbol("]", "Expected ']' after array literal"))
			return false;

		std::vector<UdonValue> ops;
		ops.push_back(make_string("array"));
		ops.push_back(make_int(count));
		for (int i = 0; i < count; ++i)
			ops.push_back(make_string(""));
		emit(body, UdonInstruction::OpCode::CALL, ops);
		return parse_postfix(body, ctx);
	}

	if (peek().type == Token::Type::Keyword && (peek().text == "true" || peek().text == "false"))
	{
		const bool val = advance().text == "true";
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_bool(val) });
		return parse_postfix(body, ctx);
	}
	if (peek().type == Token::Type::Keyword && peek().text == "none")
	{
		advance();
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
		return parse_postfix(body, ctx);
	}

	if (peek().type == Token::Type::Template)
	{
		Token templ = advance();
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(templ.template_content) });
		std::vector<UdonValue> ops;
		ops.push_back(make_string(templ.text));
		ops.push_back(make_int(1));
		ops.push_back(make_string(""));
		emit(body, UdonInstruction::OpCode::CALL, ops);
		return parse_postfix(body, ctx);
	}

	if (match_symbol("("))
	{
		if (!parse_expression(body, ctx))
			return false;
		if (!expect_symbol(")", "Expected ')'"))
			return false;
		return parse_postfix(body, ctx);
	}

	if (match_symbol("{"))
	{
		std::vector<std::string> keys;
		size_t auto_index = 0;

		if (!match_symbol("}"))
		{
			do
			{
				bool has_explicit_key = false;
				std::string key;
				Token key_token = peek();
				if (key_token.type == Token::Type::Identifier || key_token.type == Token::Type::String || key_token.type == Token::Type::Number)
				{
					key = advance().text;
					if (match_symbol(":"))
					{
						has_explicit_key = true;
					}
				}
				else
				{
					return !make_error(peek(), "Expected property name").has_error;
				}

				if (!has_explicit_key)
				{
					key = std::to_string(auto_index++);
					current = current - 1;
				}

				if (!parse_expression(body, ctx))
					return false;

				if (has_explicit_key && key_token.type == Token::Type::Number)
				{
					try
					{
						const s64 key_num = std::stoll(key);
						if (key_num >= 0 && static_cast<size_t>(key_num + 1) > auto_index)
							auto_index = static_cast<size_t>(key_num + 1);
					}
					catch (...)
					{
					}
				}
				keys.push_back(key);

			} while (match_symbol(","));

			if (!expect_symbol("}", "Expected '}' after object literal"))
				return false;
		}

		for (const auto& k : keys)
			emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(k) });

		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(static_cast<s64>(keys.size())) });

		std::vector<UdonValue> ops;
		ops.push_back(make_string("__object_literal"));
		ops.push_back(make_int(static_cast<s64>(keys.size() * 2 + 1))); // total arg count

		emit(body, UdonInstruction::OpCode::CALL, ops);
		return parse_postfix(body, ctx);
	}
	if (peek().type == Token::Type::Template)
	{
		Token templ = advance();
		emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(templ.template_content) });
		std::vector<UdonValue> ops;
		ops.push_back(make_string(templ.text));
		ops.push_back(make_int(1));
		ops.push_back(make_string(""));
		emit(body, UdonInstruction::OpCode::CALL, ops);
		return parse_postfix(body, ctx);
	}
	return !make_error(peek(), "Unexpected token '" + peek().text + "' in expression").has_error;
}
