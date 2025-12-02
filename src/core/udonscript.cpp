#include "udonscript.h"
#include "udonscript-internal.h"
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
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

using namespace udon_script_helpers;
using namespace udon_script_builtins;

thread_local UdonInterpreter* g_udon_current = nullptr;

namespace
{
	using Value = UdonValue;
	struct Parser
	{
		Parser(const std::vector<Token>& tokens,
			std::unordered_map<std::string, std::vector<UdonInstruction>>& instructions_out,
			std::unordered_map<std::string, std::vector<std::string>>& params_out,
			std::unordered_map<std::string, std::string>& variadic_out,
			std::unordered_map<std::string, std::vector<std::string>>& events_out,
			std::vector<UdonInstruction>& global_init_out,
			std::unordered_set<std::string>& globals_out,
			const std::unordered_set<std::string>& chunk_globals_out,
			s32& lambda_counter_ref)
			: tokens(tokens), instructions(instructions_out), params(params_out), variadic(variadic_out), events(events_out), global_init(global_init_out), globals(globals_out), chunk_globals(chunk_globals_out), lambda_counter(lambda_counter_ref)
		{
		}

		CodeLocation parse()
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

	  private:
		const std::vector<Token>& tokens;
		size_t current = 0;
		CodeLocation error_location{};
		std::unordered_map<std::string, std::vector<UdonInstruction>>& instructions;
		std::unordered_map<std::string, std::vector<std::string>>& params;
		std::unordered_map<std::string, std::string>& variadic;
		std::unordered_map<std::string, std::vector<std::string>>& events;
		std::vector<UdonInstruction>& global_init;
		std::unordered_set<std::string>& globals;
		const std::unordered_set<std::string>& chunk_globals;
		s32& lambda_counter;
		struct LoopContext
		{
			std::vector<size_t> break_jumps;
			std::vector<size_t> continue_jumps;
			size_t continue_target = 0;
			bool allow_continue = false;
		};
		std::vector<LoopContext> loop_stack;
		std::vector<std::unordered_set<std::string>*> local_scope_stack;

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

		struct ScopeGuard
		{
			std::vector<std::unordered_set<std::string>*>& stack;
			bool active = false;
			ScopeGuard(std::vector<std::unordered_set<std::string>*>& s, std::unordered_set<std::string>& scope) : stack(s)
			{
				stack.push_back(&scope);
				active = true;
			}
			~ScopeGuard()
			{
				if (active && !stack.empty())
					stack.pop_back();
			}
		};

		bool is_end() const
		{
			return peek().type == Token::Type::EndOfFile;
		}

		const Token& peek() const
		{
			return tokens[current];
		}

		const Token& previous() const
		{
			return tokens[current - 1];
		}

		const Token& advance()
		{
			if (!is_end())
				current++;
			return previous();
		}

		bool check_symbol(const std::string& text) const
		{
			if (is_end())
				return false;
			const auto& t = peek();
			return t.type == Token::Type::Symbol && t.text == text;
		}

		bool match_symbol(const std::string& text)
		{
			if (check_symbol(text))
			{
				advance();
				return true;
			}
			return false;
		}

		bool match_keyword(const std::string& text)
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

		CodeLocation make_error(const Token& t, const std::string& msg)
		{
			error_location.has_error = true;
			error_location.line = t.line;
			error_location.column = t.column;
			error_location.opt_error_message = msg;
			return error_location;
		}

		bool expect_symbol(const std::string& sym, const std::string& message)
		{
			if (match_symbol(sym))
				return true;
			make_error(peek(), message);
			return false;
		}

		void skip_semicolons()
		{
			while (match_symbol(";"))
				;
		}

		bool is_declared(const std::unordered_set<std::string>& locals, const std::string& name) const
		{
			if (locals.find(name) != locals.end())
				return true;
			for (auto it = local_scope_stack.rbegin(); it != local_scope_stack.rend(); ++it)
			{
				if ((*it)->find(name) != (*it)->end())
					return true;
			}
			return globals.find(name) != globals.end() || chunk_globals.find(name) != chunk_globals.end();
		}

		bool parse_global_var()
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
				std::unordered_set<std::string> locals;
				if (!parse_expression(global_init, locals))
					return false;
			}
			else
			{
				emit(global_init, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
			}
			emit(global_init, UdonInstruction::OpCode::STORE_VAR, { make_string(name) });
			return true;
		}

		bool parse_function()
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
			std::unordered_set<std::string> locals;
			for (const auto& p : param_names)
				locals.insert(p);

			ScopeGuard fn_scope(local_scope_stack, locals);
			while (!is_end())
			{
				skip_semicolons();
				if (match_symbol("}"))
					break;
				if (!parse_statement(body, locals))
					return false;
			}
			if (is_end() && (body.empty() || previous().text != "}"))
			{
				return !make_error(previous(), "Missing closing '}'").has_error;
			}

			instructions[function_name] = body;
			params[function_name] = param_names;
			if (!variadic_param.empty())
				variadic[function_name] = variadic_param;
			if (is_event_handler)
			{
				events["on:" + on_target].push_back(function_name);
			}
			return true;
		}

		void emit(std::vector<UdonInstruction>& body, UdonInstruction::OpCode op, const std::vector<Value>& operands = {})
		{
			UdonInstruction i{};
			i.opcode = op;
			i.operands = operands;
			body.push_back(i);
		}

		bool parse_block(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!expect_symbol("{", "Expected '{' to start block"))
				return false;
			while (!is_end())
			{
				skip_semicolons();
				if (match_symbol("}"))
					break;
				if (!parse_statement(body, locals))
					return false;
			}
			if (is_end())
				return !make_error(previous(), "Missing closing '}'").has_error;
			return true;
		}

		bool parse_statement_or_block(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (check_symbol("{"))
				return parse_block(body, locals);
			return parse_statement(body, locals);
		}

		bool parse_statement(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			skip_semicolons();
			if (match_keyword("if"))
			{
				if (!expect_symbol("(", "Expected '(' after if"))
					return false;
				if (!parse_expression(body, locals))
					return false;
				if (!expect_symbol(")", "Expected ')' after if condition"))
					return false;

				size_t jmp_false_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

				if (!parse_statement_or_block(body, locals))
					return false;

				size_t jmp_end_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });

				body[jmp_false_index].operands[0].s32_value = static_cast<s32>(body.size());

				skip_semicolons();
				if (match_keyword("else"))
				{
					if (!parse_statement_or_block(body, locals))
						return false;
				}
				body[jmp_end_index].operands[0].s32_value = static_cast<s32>(body.size());
				return true;
			}
			if (match_keyword("while"))
			{
				if (!expect_symbol("(", "Expected '(' after while"))
					return false;
				size_t cond_index = body.size();
				if (!parse_expression(body, locals))
					return false;
				if (!expect_symbol(")", "Expected ')' after while condition"))
					return false;

				size_t jmp_false_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

				loop_stack.push_back({});
				loop_stack.back().continue_target = static_cast<size_t>(cond_index);
				loop_stack.back().allow_continue = true;
				if (!parse_statement_or_block(body, locals))
					return false;
				for (size_t ci : loop_stack.back().continue_jumps)
					body[ci].operands[0].s32_value = static_cast<s32>(cond_index);
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(static_cast<s32>(cond_index)) });
				body[jmp_false_index].operands[0].s32_value = static_cast<s32>(body.size());
				for (size_t bi : loop_stack.back().break_jumps)
					body[bi].operands[0].s32_value = static_cast<s32>(body.size());
				loop_stack.pop_back();
				return true;
			}

			if (match_keyword("for"))
			{
				if (!expect_symbol("(", "Expected '(' after for"))
					return false;

				if (!match_symbol(";"))
				{
					if (match_keyword("var"))
					{
						if (peek().type != Token::Type::Identifier)
							return !make_error(peek(), "Expected variable name").has_error;
						const std::string name = advance().text;
						locals.insert(name);
						if (match_symbol(":"))
							advance();
						if (match_symbol("="))
						{
							if (!parse_expression(body, locals))
								return false;
						}
						else
						{
							emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
						}
						emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(name) });
						if (!expect_symbol(";", "Expected ';' after for init"))
							return false;
					}
					else
					{
						bool produced = false;
						if (!parse_assignment_or_expression(body, locals, produced))
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
					if (!parse_expression(body, locals))
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
					if (!parse_assignment_or_expression(increment_code, locals, produced))
						return false;
					if (produced)
						emit(increment_code, UdonInstruction::OpCode::POP);
					if (!expect_symbol(")", "Expected ')' after for increment"))
						return false;
				}

				loop_stack.push_back({});
				loop_stack.back().allow_continue = true;
				if (!parse_statement_or_block(body, locals))
					return false;
				size_t continue_target = body.size();
				for (size_t ci : loop_stack.back().continue_jumps)
					body[ci].operands[0].s32_value = static_cast<s32>(continue_target);
				body.insert(body.end(), increment_code.begin(), increment_code.end());
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(static_cast<s32>(cond_index)) });
				body[jmp_false_index].operands[0].s32_value = static_cast<s32>(body.size());
				for (size_t bi : loop_stack.back().break_jumps)
					body[bi].operands[0].s32_value = static_cast<s32>(body.size());
				loop_stack.pop_back();
				return true;
			}

			if (match_keyword("foreach"))
			{
				if (!expect_symbol("(", "Expected '(' after foreach"))
					return false;
				bool declared = match_keyword("var");
				if (peek().type != Token::Type::Identifier)
					return !make_error(peek(), "Expected iterator variable name").has_error;
				std::string key_name = advance().text;
				if (declared)
					locals.insert(key_name);

				std::string value_name;
				bool has_value = false;
				if (match_symbol(","))
				{
					if (peek().type != Token::Type::Identifier)
						return !make_error(peek(), "Expected value variable name after ','").has_error;
					value_name = advance().text;
					if (declared)
						locals.insert(value_name);
					has_value = true;
				}

				if (!match_keyword("in"))
					return !make_error(peek(), "Expected 'in' in foreach").has_error;
				std::string collection_tmp = "__foreach_coll_" + std::to_string(body.size());
				std::string keys_tmp = "__foreach_keys_" + std::to_string(body.size());
				std::string idx_tmp = "__foreach_i_" + std::to_string(body.size());
				locals.insert(collection_tmp);
				locals.insert(keys_tmp);
				locals.insert(idx_tmp);

				if (!parse_expression(body, locals))
					return false;
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(collection_tmp) });

				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(collection_tmp) });
				std::vector<Value> call_ops;
				call_ops.push_back(make_string("keys"));
				call_ops.push_back(make_int(1));
				call_ops.push_back(make_string(""));
				emit(body, UdonInstruction::OpCode::CALL, call_ops);
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(keys_tmp) });

				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(0) });
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(idx_tmp) });

				if (!expect_symbol(")", "Expected ')' after foreach header"))
					return false;

				size_t cond_index = body.size();
				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(idx_tmp) });
				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(keys_tmp) });
				std::vector<Value> len_ops;
				len_ops.push_back(make_string("len"));
				len_ops.push_back(make_int(1));
				len_ops.push_back(make_string(""));
				emit(body, UdonInstruction::OpCode::CALL, len_ops);
				emit(body, UdonInstruction::OpCode::LT);
				size_t jmp_false_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });

				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(keys_tmp) });
				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(idx_tmp) });
				std::vector<Value> get_key_ops;
				get_key_ops.push_back(make_string("array_get"));
				get_key_ops.push_back(make_int(2));
				get_key_ops.push_back(make_string(""));
				get_key_ops.push_back(make_string(""));
				emit(body, UdonInstruction::OpCode::CALL, get_key_ops);
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(key_name) });

				if (has_value)
				{
					emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(collection_tmp) });
					emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(key_name) });
					std::vector<Value> get_val_ops;
					get_val_ops.push_back(make_string("array_get"));
					get_val_ops.push_back(make_int(2));
					get_val_ops.push_back(make_string(""));
					get_val_ops.push_back(make_string(""));
					emit(body, UdonInstruction::OpCode::CALL, get_val_ops);
					emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(value_name) });
				}

				loop_stack.push_back({});
				loop_stack.back().allow_continue = true;
				if (!parse_block(body, locals))
					return false;

				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(idx_tmp) });
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
				emit(body, UdonInstruction::OpCode::ADD);
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(idx_tmp) });

				size_t continue_target = body.size();
				for (size_t ci : loop_stack.back().continue_jumps)
					body[ci].operands[0].s32_value = static_cast<s32>(continue_target);
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(static_cast<s32>(cond_index)) });
				body[jmp_false_index].operands[0].s32_value = static_cast<s32>(body.size());
				for (size_t bi : loop_stack.back().break_jumps)
					body[bi].operands[0].s32_value = static_cast<s32>(body.size());
				loop_stack.pop_back();
				return true;
			}

			if (match_keyword("switch"))
			{
				if (!expect_symbol("(", "Expected '(' after switch"))
					return false;
				std::string tmp_name = "__switch_val_" + std::to_string(body.size());
				locals.insert(tmp_name);
				if (!parse_expression(body, locals))
					return false;
				if (!expect_symbol(")", "Expected ')' after switch expression"))
					return false;
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(tmp_name) });
				if (!expect_symbol("{", "Expected '{' after switch header"))
					return false;

				loop_stack.push_back({});
				loop_stack.back().allow_continue = false;

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
						Value case_val;
						if (peek().type == Token::Type::Number)
						{
							const std::string num_text = advance().text;
							case_val = (num_text.find('.') != std::string::npos)
										   ? make_float(static_cast<f32>(std::stof(num_text)))
										   : make_int(static_cast<s32>(std::stoi(num_text)));
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
						if (!expect_symbol(":", "Expected ':' after case value"))
							return false;

						emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(tmp_name) });
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
							if (!parse_statement(body, locals))
								return false;
						}

						size_t end_jump = body.size();
						emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
						body[jz_index].operands[0].s32_value = static_cast<s32>(body.size());
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
							if (!parse_statement(body, locals))
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
				for (size_t bi : loop_stack.back().break_jumps)
					body[bi].operands[0].s32_value = static_cast<s32>(body.size());
				loop_stack.pop_back();
				return true;
			}

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
					if (!parse_expression(body, locals))
						return false;
					auto destructure_var = [&](const std::vector<std::string>& targets) -> bool
					{
						const std::string tmp_name = "__tuple_tmp_" + std::to_string(body.size());
						locals.insert(tmp_name);
						emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(tmp_name) });
						const bool use_indexing = targets.size() > 1;
						for (size_t i = 0; i < targets.size(); ++i)
						{
							const std::string& n = targets[i];
							if (n == "_")
								continue;
							locals.insert(n);
							emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(tmp_name) });
							if (use_indexing)
							{
								emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(static_cast<s32>(i)) });
								emit(body, UdonInstruction::OpCode::GET_PROP, { make_string("[index]") });
							}
							emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(n) });
						}
						return true;
					};
					if (!destructure_var(names))
						return false;
				}
				else
				{
					for (const auto& n : names)
					{
						if (n == "_")
							continue;
						locals.insert(n);
						emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
						emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(n) });
					}
				}
				return true;
			}
			if (match_keyword("return"))
			{
				if (!expect_symbol("(", "Expected '(' after return"))
					return false;
				size_t value_count = 0;
				if (!match_symbol(")"))
				{
					do
					{
						if (!parse_expression(body, locals))
							return false;
						value_count++;
					} while (match_symbol(","));
					if (!expect_symbol(")", "Expected ')' after return value"))
						return false;
				}
				if (value_count == 0)
				{
					emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
				}
				else if (value_count > 1)
				{
					std::vector<Value> ops;
					ops.push_back(make_string("array"));
					ops.push_back(make_int(static_cast<s32>(value_count)));
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
				size_t jmp_idx = body.size();
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
				loop_stack.back().break_jumps.push_back(jmp_idx);
				return true;
			}

			if (match_keyword("continue"))
			{
				if (loop_stack.empty() || !loop_stack.back().allow_continue)
					return !make_error(previous(), "continue outside of loop").has_error;
				size_t jmp_idx = body.size();
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
				loop_stack.back().continue_jumps.push_back(jmp_idx);
				return true;
			}

			bool produced = false;
			if (!parse_assignment_or_expression(body, locals, produced))
				return false;
			if (produced)
				emit(body, UdonInstruction::OpCode::POP);
			return true;
		}

		bool parse_expression(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			bool produced = true;
			return parse_assignment_or_expression(body, locals, produced);
		}

		bool parse_or(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!parse_and(body, locals))
				return false;
			while (match_symbol("||"))
			{
				emit(body, UdonInstruction::OpCode::TO_BOOL);
				size_t jz_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_bool(true) });
				size_t jmp_end = body.size();
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
				body[jz_index].operands[0].s32_value = static_cast<s32>(body.size());
				if (!parse_and(body, locals))
					return false;
				emit(body, UdonInstruction::OpCode::TO_BOOL);
				body[jmp_end].operands[0].s32_value = static_cast<s32>(body.size());
			}
			return true;
		}

		bool parse_and(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!parse_equality(body, locals))
				return false;
			while (match_symbol("&&"))
			{
				emit(body, UdonInstruction::OpCode::TO_BOOL);
				size_t jz_index = body.size();
				emit(body, UdonInstruction::OpCode::JUMP_IF_FALSE, { make_int(0) });
				if (!parse_equality(body, locals))
					return false;
				emit(body, UdonInstruction::OpCode::TO_BOOL);
				size_t jmp_end = body.size();
				emit(body, UdonInstruction::OpCode::JUMP, { make_int(0) });
				body[jz_index].operands[0].s32_value = static_cast<s32>(body.size());
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_bool(false) });
				body[jmp_end].operands[0].s32_value = static_cast<s32>(body.size());
			}
			return true;
		}

		bool parse_equality(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!parse_comparison(body, locals))
				return false;
			while (true)
			{
				if (match_symbol("=="))
				{
					if (!parse_comparison(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::EQ);
				}
				else if (match_symbol("!="))
				{
					if (!parse_comparison(body, locals))
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

		bool parse_comparison(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!parse_additive(body, locals))
				return false;
			while (true)
			{
				if (match_symbol("<"))
				{
					if (!parse_additive(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::LT);
				}
				else if (match_symbol(">"))
				{
					if (!parse_additive(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::GT);
				}
				else if (match_symbol("<="))
				{
					if (!parse_additive(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::LTE);
				}
				else if (match_symbol(">="))
				{
					if (!parse_additive(body, locals))
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

		bool parse_assignment_or_expression(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals, bool& produced_value)
		{
			auto destructure_assign = [&](const std::vector<std::string>& names, bool allow_new, bool push_first_value) -> bool
			{
				const std::string tmp_name = "__tuple_tmp_" + std::to_string(body.size());
				locals.insert(tmp_name);
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(tmp_name) });

				auto load_element = [&](size_t idx, bool use_index)
				{
					emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(tmp_name) });
					if (use_index)
					{
						emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(static_cast<s32>(idx)) });
						emit(body, UdonInstruction::OpCode::GET_PROP, { make_string("[index]") });
					}
				};

				const bool use_indexing = names.size() > 1;
				for (size_t i = 0; i < names.size(); ++i)
				{
					const std::string& name = names[i];
					if (name == "_")
						continue;
					if (!allow_new && !is_declared(locals, name))
						return !make_error(previous(), "Undeclared variable '" + name + "'").has_error;
					if (allow_new)
						locals.insert(name);
					load_element(i, use_indexing);
					emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(name) });
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
					if (!parse_expression(body, locals))
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
						locals.insert(n);
						emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_none() });
						emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(n) });
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
					if (!parse_expression(body, locals))
						return false;
					if (!destructure_assign(names, false, true))
						return false;
					produced_value = true;
					return true;
				}
			}

			if (peek().type == Token::Type::Identifier && tokens.size() > current + 2)
			{
				size_t lookahead = current + 1;
				if (tokens[lookahead].type == Token::Type::Symbol)
				{
					std::string next_sym = tokens[lookahead].text;
					if (next_sym == ":" && lookahead + 2 < tokens.size())
					{
						if (tokens[lookahead + 2].type == Token::Type::Symbol && tokens[lookahead + 2].text == "=")
						{
							std::string obj_name = advance().text;
							if (!is_declared(locals, obj_name))
								return !make_error(previous(), "Undeclared variable '" + obj_name + "'").has_error;
							advance(); // ':'
							if (peek().type != Token::Type::Identifier && peek().type != Token::Type::String && peek().type != Token::Type::Number)
								return !make_error(peek(), "Expected property name after ':'").has_error;
							std::string prop_name = advance().text;
							advance(); // '='
							emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(obj_name) });
							if (!parse_expression(body, locals))
								return false;
							emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string(prop_name) });
							produced_value = false;
							return true;
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
							tokens[bracket_end + 1].type == Token::Type::Symbol && tokens[bracket_end + 1].text == "=")
						{
							std::string obj_name = advance().text;
							if (!is_declared(locals, obj_name))
								return !make_error(previous(), "Undeclared variable '" + obj_name + "'").has_error;
							advance(); // '['
							emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(obj_name) });
							if (!parse_expression(body, locals))
								return false;
							if (!expect_symbol("]", "Expected ']' after index"))
								return false;
							advance(); // '='
							if (!parse_expression(body, locals))
								return false;
							emit(body, UdonInstruction::OpCode::STORE_PROP, { make_string("[index]") });
							produced_value = false;
							return true;
						}
					}
				}
			}

			if (peek().type == Token::Type::Identifier && tokens.size() > current + 1 && tokens[current + 1].type == Token::Type::Symbol)
			{
				const std::string name = advance().text;
				std::string op = tokens[current].text;
				if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=")
				{
					if (!is_declared(locals, name))
						return !make_error(previous(), "Undeclared variable '" + name + "'").has_error;
					advance();
					if (op != "=")
						emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(name) });
					if (!parse_expression(body, locals))
						return false;
					if (op == "+=")
						emit(body, UdonInstruction::OpCode::ADD);
					else if (op == "-=")
						emit(body, UdonInstruction::OpCode::SUB);
					else if (op == "*=")
						emit(body, UdonInstruction::OpCode::MUL);
					else if (op == "/=")
						emit(body, UdonInstruction::OpCode::DIV);
					emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(name) });
					produced_value = false;
					return true;
				}
				current--; // rewind name consumption
			}
			produced_value = true;
			return parse_or(body, locals);
		}
		bool parse_additive(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!parse_multiplicative(body, locals))
				return false;
			while (true)
			{
				if (match_symbol("+"))
				{
					if (!parse_multiplicative(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::ADD);
				}
				else if (match_symbol("-"))
				{
					if (!parse_multiplicative(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::SUB);
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool parse_multiplicative(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (!parse_unary(body, locals))
				return false;
			while (true)
			{
				if (match_symbol("*"))
				{
					if (!parse_unary(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::MUL);
				}
				else if (match_symbol("/"))
				{
					if (!parse_unary(body, locals))
						return false;
					emit(body, UdonInstruction::OpCode::DIV);
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool parse_unary(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (match_symbol("-"))
			{
				if (!parse_unary(body, locals))
					return false;
				emit(body, UdonInstruction::OpCode::NEGATE);
				return true;
			}
			if (match_symbol("!"))
			{
				if (!parse_unary(body, locals))
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
				if (!is_declared(locals, name))
					return !make_error(previous(), "Undeclared variable '" + name + "'").has_error;
				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(name) });
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
				emit(body, inc ? UdonInstruction::OpCode::ADD : UdonInstruction::OpCode::SUB);
				emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(name) });
				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(name) });
				return true;
			}
			return parse_primary(body, locals);
		}

		bool parse_postfix(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			while (true)
			{
				if (match_symbol("."))
				{
					if (!parse_method_postfix(body, locals))
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
					if (!parse_expression(body, locals))
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
							if (!parse_expression(body, locals))
								return false;
							arg_count++;
						} while (match_symbol(","));
						if (!expect_symbol(")", "Expected ')' after arguments"))
							return false;
					}
					std::vector<Value> operands;
					operands.push_back(make_string("")); // empty callee signals dynamic call using callable on stack
					operands.push_back(make_int(static_cast<s32>(arg_count)));
					emit(body, UdonInstruction::OpCode::CALL, operands);
					continue;
				}
				break;
			}
			return true;
		}

		bool parse_method_postfix(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
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
					if (!parse_expression(body, locals))
						return false;
					arg_names.push_back(arg_name);
					arg_count++;
				} while (match_symbol(","));
				if (!expect_symbol(")", "Expected ')' after arguments"))
					return false;
			}
			std::vector<Value> operands;
			operands.push_back(make_string(member));
			operands.push_back(make_int(static_cast<s32>(arg_count)));
			for (const auto& n : arg_names)
				operands.push_back(make_string(n));
			emit(body, UdonInstruction::OpCode::CALL, operands);
			return true;
		}

		bool parse_key_postfix(std::vector<UdonInstruction>& body)
		{
			if (peek().type != Token::Type::Identifier && peek().type != Token::Type::String && peek().type != Token::Type::Number)
				return !make_error(peek(), "Expected key after ':'").has_error;
			std::string key = advance().text;
			emit(body, UdonInstruction::OpCode::GET_PROP, { make_string(key) });
			return true;
		}

		bool parse_function_literal(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			(void)locals;
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
			std::unordered_set<std::string> fn_locals;
			for (const auto& p : param_names)
				fn_locals.insert(p);

			LoopGuard loop_guard(loop_stack, true);
			ScopeGuard scope_guard(local_scope_stack, fn_locals);

			while (!is_end())
			{
				skip_semicolons();
				if (match_symbol("}"))
					break;
				if (!parse_statement(fn_body, fn_locals))
				{
					return false;
				}
			}
			if (is_end() && (fn_body.empty() || previous().text != "}"))
			{
				return !make_error(previous(), "Missing closing '}'").has_error;
			}

			std::string fn_name = "__lambda_" + std::to_string(lambda_counter++);
			instructions[fn_name] = fn_body;
			params[fn_name] = param_names;
			if (!variadic_param.empty())
				variadic[fn_name] = variadic_param;

			emit(body, UdonInstruction::OpCode::MAKE_CLOSURE, { make_string(fn_name) });
			return true;
		}

		bool parse_primary(std::vector<UdonInstruction>& body, std::unordered_set<std::string>& locals)
		{
			if (match_keyword("function"))
			{
				if (!parse_function_literal(body, locals))
					return false;
				return parse_postfix(body, locals);
			}

			if (peek().type == Token::Type::Number)
			{
				const std::string num_text = advance().text;
				if (num_text.find('.') != std::string::npos)
					emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_float(static_cast<f32>(std::stof(num_text))) });
				else
					emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(static_cast<s32>(std::stoi(num_text))) });
				return parse_postfix(body, locals);
			}

			if (peek().type == Token::Type::String)
			{
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(advance().text) });
				return parse_postfix(body, locals);
			}

			if (peek().type == Token::Type::Identifier)
			{
				Token ident = advance();
				if (match_symbol("("))
				{
					std::vector<std::string> arg_names;
					size_t arg_count = 0;
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
							if (!parse_expression(body, locals))
								return false;
							arg_names.push_back(arg_name);
							arg_count++;
						} while (match_symbol(","));
						if (!expect_symbol(")", "Expected ')' after arguments"))
							return false;
					}

					std::vector<Value> operands;
					operands.push_back(make_string(ident.text));
					operands.push_back(make_int(static_cast<s32>(arg_count)));
					for (const auto& n : arg_names)
						operands.push_back(make_string(n));
					emit(body, UdonInstruction::OpCode::CALL, operands);
					return parse_postfix(body, locals);
				}

				if (!is_declared(locals, ident.text))
					return !make_error(previous(), "Undeclared variable '" + ident.text + "'").has_error;
				emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(ident.text) });
				if (match_symbol("++") || match_symbol("--"))
				{
					bool inc = previous().text == "++";
					if (!is_declared(locals, ident.text))
						return !make_error(previous(), "Undeclared variable '" + ident.text + "'").has_error;
					emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(1) });
					emit(body, inc ? UdonInstruction::OpCode::ADD : UdonInstruction::OpCode::SUB);
					emit(body, UdonInstruction::OpCode::STORE_VAR, { make_string(ident.text) });
					emit(body, UdonInstruction::OpCode::LOAD_VAR, { make_string(ident.text) });
				}
				return parse_postfix(body, locals);
			}

			if (match_symbol("["))
			{
				int count = 0;
				if (match_symbol("]"))
				{
					std::vector<Value> ops;
					ops.push_back(make_string("array"));
					ops.push_back(make_int(0));
					emit(body, UdonInstruction::OpCode::CALL, ops);
					return parse_postfix(body, locals);
				}

				do
				{
					if (!parse_expression(body, locals))
						return false;
					count++;
				} while (match_symbol(","));

				if (!expect_symbol("]", "Expected ']' after array literal"))
					return false;

				std::vector<Value> ops;
				std::string callee = "array";
				if (count == 2)
					callee = "Vector2";
				else if (count == 3)
					callee = "Vector3";
				else if (count == 4)
					callee = "Vector4";
				ops.push_back(make_string(callee));
				ops.push_back(make_int(count));
				for (int i = 0; i < count; ++i)
					ops.push_back(make_string(""));
				emit(body, UdonInstruction::OpCode::CALL, ops);
				return parse_postfix(body, locals);
			}

			if (peek().type == Token::Type::Keyword && (peek().text == "true" || peek().text == "false"))
			{
				const bool val = advance().text == "true";
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_bool(val) });
				return parse_postfix(body, locals);
			}

			if (peek().type == Token::Type::Template)
			{
				Token templ = advance();
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(templ.template_content) });
				std::vector<Value> ops;
				ops.push_back(make_string(templ.text));
				ops.push_back(make_int(1));
				ops.push_back(make_string(""));
				emit(body, UdonInstruction::OpCode::CALL, ops);
				return parse_postfix(body, locals);
			}

			if (match_symbol("("))
			{
				if (!parse_expression(body, locals))
					return false;
				if (!expect_symbol(")", "Expected ')'"))
					return false;
				return parse_postfix(body, locals);
			}

			if (match_symbol("{"))
			{
				std::vector<std::string> keys;
				std::vector<bool> values_parsed;

				if (!match_symbol("}"))
				{
					do
					{
						std::string key;
						if (peek().type == Token::Type::Identifier)
						{
							key = advance().text;
						}
						else if (peek().type == Token::Type::String)
						{
							key = advance().text;
						}
						else if (peek().type == Token::Type::Number)
						{
							key = advance().text;
						}
						else
						{
							return !make_error(peek(), "Expected property name").has_error;
						}

						if (!expect_symbol(":", "Expected ':' after property name"))
							return false;

						if (!parse_expression(body, locals))
							return false;

						keys.push_back(key);

					} while (match_symbol(","));

					if (!expect_symbol("}", "Expected '}' after object literal"))
						return false;
				}

				for (const auto& k : keys)
					emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(k) });

				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_int(static_cast<s32>(keys.size())) });

				std::vector<Value> ops;
				ops.push_back(make_string("__object_literal"));
				ops.push_back(make_int(static_cast<s32>(keys.size() * 2 + 1))); // total arg count

				emit(body, UdonInstruction::OpCode::CALL, ops);
				return parse_postfix(body, locals);
			}
			if (peek().type == Token::Type::Template)
			{
				Token templ = advance();
				emit(body, UdonInstruction::OpCode::PUSH_LITERAL, { make_string(templ.template_content) });
				std::vector<Value> ops;
				ops.push_back(make_string(templ.text));
				ops.push_back(make_int(1));
				ops.push_back(make_string(""));
				emit(body, UdonInstruction::OpCode::CALL, ops);
				return parse_postfix(body, locals);
			}
			return !make_error(peek(), "Unexpected token in expression").has_error;
		}
	};

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

	bool pop_value(std::vector<Value>& stack, Value& out, CodeLocation& error)
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

} // namespace

static bool get_property_value(const UdonValue& obj, const std::string& name, UdonValue& out)
{
	using namespace udon_script_helpers;
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
	using namespace udon_script_helpers;
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
	if (obj.type == UdonValue::Type::Vector2 || obj.type == UdonValue::Type::Vector3 || obj.type == UdonValue::Type::Vector4)
	{
		s32 idx = static_cast<s32>(as_number(index));
		if (idx == 0)
			out = make_float(obj.type == UdonValue::Type::Vector2 ? obj.vec2_value.x : obj.type == UdonValue::Type::Vector3 ? obj.vec3_value.x
																															: obj.vec4_value.x);
		else if (idx == 1)
			out = make_float(obj.type == UdonValue::Type::Vector2 ? obj.vec2_value.y : obj.type == UdonValue::Type::Vector3 ? obj.vec3_value.y
																															: obj.vec4_value.y);
		else if (idx == 2 && (obj.type == UdonValue::Type::Vector3 || obj.type == UdonValue::Type::Vector4))
			out = make_float(obj.type == UdonValue::Type::Vector3 ? obj.vec3_value.z : obj.vec4_value.z);
		else if (idx == 3 && obj.type == UdonValue::Type::Vector4)
			out = make_float(obj.vec4_value.w);
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
	const std::unordered_map<std::string, UdonValue>* captured_locals,
	std::vector<UdonValue> args,
	std::unordered_map<std::string, UdonValue> named_args,
	UdonValue& return_value)
{
	using Value = UdonValue;
	CodeLocation ok{};
	ok.has_error = false;
	const bool has_variadic = !variadic_param.empty();

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

	std::unordered_map<std::string, Value> locals;
	if (captured_locals)
		locals = *captured_locals;

	size_t positional_index = 0;
	for (const auto& name : param_names)
	{
		if (has_variadic && name == variadic_param)
		{
			locals[name] = udon_script_helpers::make_none();
			continue;
		}
		auto nit = named_args.find(name);
		if (nit != named_args.end())
		{
			locals[name] = nit->second;
		}
		else if (positional_index < args.size())
		{
			locals[name] = args[positional_index++];
		}
		else
		{
			locals[name] = udon_script_helpers::make_none();
		}
	}

	if (has_variadic)
	{
		Value vargs = make_array();
		for (size_t i = positional_index; i < args.size(); ++i)
			array_set(vargs, std::to_string(i - positional_index), args[i]);
		locals[variadic_param] = vargs;
	}
	else if (!has_variadic && !named_args.empty() && args.size() > param_names.size())
	{
		ok.has_error = true;
		ok.opt_error_message = "Too many positional arguments";
		return ok;
	}

	std::vector<Value> eval_stack;

	auto pop_checked = [&](Value& out) -> bool
	{
		return pop_value(eval_stack, out, ok);
	};

	auto pop_two = [&](Value& lhs, Value& rhs) -> bool
	{
		if (!pop_checked(rhs) || !pop_checked(lhs))
			return false;
		return true;
	};

	auto fail = [&](const std::string& msg) -> bool
	{
		ok.has_error = true;
		ok.opt_error_message = msg;
		return false;
	};

	auto do_binary = [&](auto op_fn) -> bool
	{
		Value rhs{};
		Value lhs{};
		if (!pop_two(lhs, rhs))
			return false;
		Value result{};
		if (!op_fn(lhs, rhs, result))
			return fail("Invalid operands for arithmetic");
		eval_stack.push_back(result);
		return true;
	};

	size_t ip = 0;
	auto push_gc_root_and_collect = [&](const Value& v)
	{
		interp->stack.push_back(v);
		interp->collect_garbage();
		interp->stack.pop_back();
	};

	while (ip < code.size())
	{
		const auto& instr = code[ip];
		switch (instr.opcode)
		{
			case UdonInstruction::OpCode::PUSH_LITERAL:
				if (!instr.operands.empty())
					eval_stack.push_back(instr.operands[0]);
				break;
			case UdonInstruction::OpCode::LOAD_VAR:
			{
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				auto lit = locals.find(name);
				if (lit != locals.end())
				{
					eval_stack.push_back(lit->second);
				}
				else
				{
					auto git = interp->globals.find(name);
					if (git != interp->globals.end())
						eval_stack.push_back(git->second);
					else
						eval_stack.push_back(udon_script_helpers::make_none());
				}
				break;
			}
			case UdonInstruction::OpCode::STORE_VAR:
			{
				Value v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				const std::string name = !instr.operands.empty() ? instr.operands[0].string_value : "";
				if (locals.find(name) != locals.end())
					locals[name] = v;
				else
					interp->globals[name] = v;
				break;
			}
			case UdonInstruction::OpCode::ADD:
			case UdonInstruction::OpCode::SUB:
			case UdonInstruction::OpCode::MUL:
			case UdonInstruction::OpCode::DIV:
			{
				auto op = [&](const Value& lhs, const Value& rhs, Value& out) -> bool
				{
					if (instr.opcode == UdonInstruction::OpCode::ADD)
						return udon_script_helpers::add_values(lhs, rhs, out);
					if (instr.opcode == UdonInstruction::OpCode::SUB)
						return udon_script_helpers::sub_values(lhs, rhs, out);
					if (instr.opcode == UdonInstruction::OpCode::MUL)
						return udon_script_helpers::mul_values(lhs, rhs, out);
					return udon_script_helpers::div_values(lhs, rhs, out);
				};
				if (!do_binary(op))
					return ok;
				break;
			}
			case UdonInstruction::OpCode::NEGATE:
			{
				Value v{};
				if (!pop_checked(v))
					return ok;
				if (udon_script_helpers::is_numeric(v))
				{
					if (v.type == Value::Type::S32)
						v.s32_value = -v.s32_value;
					else
						v.f32_value = -v.f32_value;
				}
				else if (udon_script_helpers::is_vector(v))
				{
					if (v.type == Value::Type::Vector2)
						v.vec2_value = v.vec2_value * -1.0f;
					else if (v.type == Value::Type::Vector3)
						v.vec3_value = v.vec3_value * -1.0f;
					else
						v.vec4_value = v.vec4_value * -1.0f;
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
				Value prop;
				bool success = false;
				if (name == "[index]")
				{
					Value idx;
					Value obj;
					if (!pop_value(eval_stack, idx, ok))
						return ok;
					if (!pop_value(eval_stack, obj, ok))
						return ok;
					success = get_index_value(obj, idx, prop);
				}
				else
				{
					Value obj;
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
				Value value{};
				if (!pop_value(eval_stack, value, ok))
					return ok;

				if (name == "[index]")
				{
					Value idx;
					Value obj;
					if (!pop_value(eval_stack, idx, ok))
						return ok;
					if (!pop_value(eval_stack, obj, ok))
						return ok;

					if (obj.type != Value::Type::Array)
					{
						fail("Cannot index non-array");
						return ok;
					}

					std::string key = udon_script_helpers::key_from_value(idx);
					udon_script_helpers::array_set(obj, key, value);
				}
				else
				{
					Value obj;
					if (!pop_value(eval_stack, obj, ok))
						return ok;

					if (obj.type != Value::Type::Array)
					{
						fail("Cannot set property on non-array/object");
						return ok;
					}

					udon_script_helpers::array_set(obj, name, value);
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
				Value rhs{};
				Value lhs{};
				if (!pop_two(lhs, rhs))
					return ok;
				Value result{};
				bool success = false;
				if (instr.opcode == UdonInstruction::OpCode::EQ || instr.opcode == UdonInstruction::OpCode::NEQ)
				{
					success = udon_script_helpers::equal_values(lhs, rhs, result);
					if (instr.opcode == UdonInstruction::OpCode::NEQ)
					{
						result.s32_value = result.s32_value ? 0 : 1;
					}
				}
				else
				{
					success = udon_script_helpers::compare_values(lhs, rhs, instr.opcode, result);
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
				continue;
			}
			case UdonInstruction::OpCode::JUMP_IF_FALSE:
			{
				Value cond;
				if (!pop_value(eval_stack, cond, ok))
					return ok;
				if (!udon_script_helpers::is_truthy(cond))
				{
					if (instr.operands.empty())
					{
						ok.has_error = true;
						ok.opt_error_message = "Malformed JUMP_IF_FALSE";
						return ok;
					}
					ip = static_cast<size_t>(instr.operands[0].s32_value);
					continue;
				}
				break;
			}
			case UdonInstruction::OpCode::TO_BOOL:
			{
				Value v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				eval_stack.push_back(udon_script_helpers::bool_value(udon_script_helpers::is_truthy(v)));
				break;
			}
			case UdonInstruction::OpCode::LOGICAL_NOT:
			{
				Value v{};
				if (!pop_value(eval_stack, v, ok))
					return ok;
				eval_stack.push_back(udon_script_helpers::bool_value(!udon_script_helpers::is_truthy(v)));
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
				fn_obj->captured_locals = locals;

				Value v{};
				v.type = Value::Type::Function;
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

				std::vector<Value> call_args(static_cast<size_t>(arg_count));
				std::vector<std::string> names(static_cast<size_t>(arg_count));
				for (s32 idx = arg_count - 1; idx >= 0; --idx)
				{
					Value v{};
					if (!pop_value(eval_stack, v, ok))
						return ok;
					call_args[static_cast<size_t>(idx)] = v;
					if (static_cast<size_t>(idx) < arg_names.size())
						names[static_cast<size_t>(idx)] = arg_names[static_cast<size_t>(idx)];
				}

				std::vector<Value> positional;
				std::unordered_map<std::string, Value> named;
				for (size_t i = 0; i < call_args.size(); ++i)
				{
					if (!names[i].empty())
						named[names[i]] = call_args[i];
					else
						positional.push_back(call_args[i]);
				}

				Value call_result;
				CodeLocation inner_err{};
				inner_err.has_error = false;

				auto call_closure = [&](const Value& fn_val) -> bool
				{
					if (fn_val.type != Value::Type::Function || !fn_val.function)
						return false;
					if (!fn_val.function->handler.empty())
					{
						if (fn_val.function->handler == "html_template")
						{
							std::string rendered;
							std::unordered_map<std::string, UdonValue> replacements;
							if (!positional.empty() && positional[0].type == Value::Type::Array && positional[0].array_map)
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
									rendered.append(udon_script_helpers::value_to_string(it->second));
								pos = end + 1;
							}
							call_result = udon_script_helpers::make_string(rendered);
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
							const Value& symbol_val = positional[0];
							if (symbol_val.type != Value::Type::String)
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
									const Value& v = positional[i + 1];
									std::string t = arg_types[i];
									if (t == "s32")
									{
										if (v.type == Value::Type::S32)
											args.push_back(static_cast<double>(v.s32_value));
										else if (v.type == Value::Type::F32)
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
										if (v.type == Value::Type::F32)
											args.push_back(static_cast<double>(v.f32_value));
										else if (v.type == Value::Type::S32)
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
									const Value& v = positional[i];
									if (v.type == Value::Type::S32)
										args.push_back(static_cast<double>(v.s32_value));
									else if (v.type == Value::Type::F32)
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
								call_result = udon_script_helpers::make_int(static_cast<s32>(result));
							else
								call_result = udon_script_helpers::make_float(static_cast<f32>(result));
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
							call_result = udon_script_helpers::make_none();
							return true;
#else
							inner_err.has_error = true;
							inner_err.opt_error_message = "dl_close not supported on this platform";
							return true;
#endif
						}
						return false;
					}
					auto code_it = interp->instructions.find(fn_val.function->function_name);
					if (code_it == interp->instructions.end())
					{
						inner_err.has_error = true;
						inner_err.opt_error_message = "Function '" + fn_val.function->function_name + "' not found";
						return true;
					}
					auto param_it = interp->function_params.find(fn_val.function->function_name);
					std::vector<std::string> fn_params = (param_it != interp->function_params.end()) ? param_it->second : std::vector<std::string>();
					std::string var_param;
					auto var_it = interp->function_variadic.find(fn_val.function->function_name);
					if (var_it != interp->function_variadic.end())
						var_param = var_it->second;
					CodeLocation nested = execute_function(interp, code_it->second, fn_params, var_param, &fn_val.function->captured_locals, positional, named, call_result);
					if (nested.has_error)
						inner_err = nested;
					return true;
				};

				if (callee.empty())
				{
					Value callable;
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

				if (!positional.empty() && positional[0].type == Value::Type::Array && positional[0].array_map)
				{
					Value member_fn;
					if (udon_script_helpers::array_get(positional[0], callee, member_fn))
					{
						if (member_fn.type != Value::Type::Function || !member_fn.function)
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
					bool handled_builtin = udon_script_builtins::handle_builtin(interp, callee, positional, named, call_result, inner_err);
					if (handled_builtin)
					{
						if (inner_err.has_error)
							return inner_err;
						handled = true;
					}
				}

				if (!handled)
				{
					auto fn = interp->instructions.find(callee);
					if (fn != interp->instructions.end())
					{
						auto pit = interp->function_params.find(callee);
						std::vector<std::string> fn_params = (pit != interp->function_params.end()) ? pit->second : std::vector<std::string>();
						std::string var_param;
						auto var_it = interp->function_variadic.find(callee);
						if (var_it != interp->function_variadic.end())
							var_param = var_it->second;
						CodeLocation nested = execute_function(interp, fn->second, fn_params, var_param, nullptr, positional, named, call_result);
						if (nested.has_error)
							return nested;
						handled = true;
					}
				}

				if (!handled)
				{
					auto lit = locals.find(callee);
					if (lit != locals.end())
						handled = call_closure(lit->second);
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
					return_value = udon_script_helpers::make_none();
				}
				push_gc_root_and_collect(return_value);
				return ok;
			}
			case UdonInstruction::OpCode::POP:
			{
				Value tmp;
				if (!pop_value(eval_stack, tmp, ok))
					return ok;
				break;
			}
			case UdonInstruction::OpCode::NOP:
			case UdonInstruction::OpCode::HALT:
				return_value = udon_script_helpers::make_none();
				push_gc_root_and_collect(return_value);
				return ok;
		}
		++ip;
	}

	return_value = udon_script_helpers::make_none();
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
			if (lower == "function" || lower == "return" || lower == "var" || lower == "true" || lower == "false" || lower == "if" || lower == "else" || lower == "while" || lower == "for" || lower == "foreach" || lower == "in" || lower == "break" || lower == "continue" || lower == "switch" || lower == "case" || lower == "default")
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
	Parser parser(toks, instructions, function_params, function_variadic, event_handlers, module_global_init, declared_globals, chunk_globals, lambda_counter);
	CodeLocation res = parser.parse();
	if (res.has_error)
		return res;
	if (!module_global_init.empty())
	{
		std::string init_fn = "__globals_init_" + std::to_string(global_init_counter++);
		instructions[init_fn] = module_global_init;
		function_params[init_fn] = {};
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

	return execute_function(this, fn_it->second, param_names, variadic_param, nullptr, std::move(args), std::move(named_args), return_value);
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
	event_handlers.clear();
	globals.clear();
	stack.clear();
	for (auto* arr : heap_arrays)
		delete arr;
	heap_arrays.clear();
	for (auto* fn : heap_functions)
		delete fn;
	heap_functions.clear();
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
				case UdonInstruction::OpCode::LOAD_VAR:
					print_var("LOAD");
					break;
				case UdonInstruction::OpCode::STORE_VAR:
					print_var("STORE");
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

static void mark_value(UdonValue& v)
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
		for (auto& kv : v.function->captured_locals)
			mark_value(kv.second);
		return;
	}
}

void UdonInterpreter::collect_garbage()
{
	for (auto* arr : heap_arrays)
		arr->marked = false;
	for (auto* fn : heap_functions)
		fn->marked = false;

	for (auto& kv : globals)
		mark_value(kv.second);
	for (auto& v : stack)
		mark_value(v);

	std::vector<UdonValue::ManagedArray*> survivors;
	survivors.reserve(heap_arrays.size());
	for (auto* arr : heap_arrays)
	{
		if (arr->marked)
			survivors.push_back(arr);
		else
			delete arr;
	}
	heap_arrays.swap(survivors);

	std::vector<UdonValue::ManagedFunction*> live_functions;
	live_functions.reserve(heap_functions.size());
	for (auto* fn : heap_functions)
	{
		if (fn->marked)
			live_functions.push_back(fn);
		else
			delete fn;
	}
	heap_functions.swap(live_functions);
}
