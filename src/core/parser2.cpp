#include "parser2.h"
#include "parser.h"
#include "udonscript2.h"

Parser2::Parser2(UdonInterpreter& interp_ref,
	const std::vector<Token>& toks,
	const std::unordered_set<std::string>& chunk_globals_ref)
	: interp(interp_ref), tokens(toks), chunk_globals(chunk_globals_ref)
{
}

CodeLocation Parser2::parse()
{
	std::vector<UdonInstruction> module_global_init;
	CodeLocation res{};
	Parser parser(
		tokens,
		interp.instructions,
		interp.function_params,
		interp.function_variadic,
		interp.function_param_slots,
		interp.function_frame_sizes,
		interp.function_variadic_slot,
		interp.event_handlers,
		module_global_init,
		interp.declared_globals,
		interp.declared_global_order,
		chunk_globals,
		interp.lambda_counter,
		false /* emit_scope_ops */);
	res = parser.parse();
	if (res.has_error)
		return res;

	interp.rebuild_global_slots();

	auto populate_context_global = [&]()
	{
		UdonValue ctx{};
		ctx.type = UdonValue::Type::Array;
		ctx.array_map = interp.allocate_array();
		for (const auto& pair : interp.context_info)
		{
			UdonValue arr{};
			arr.type = UdonValue::Type::Array;
			arr.array_map = interp.allocate_array();
			s32 index = 0;
			for (const auto& line : pair.second)
			{
				array_set(arr, std::to_string(index++), make_string(line));
			}
			array_set(ctx, pair.first, arr);
		}
		interp.globals["context"] = ctx;
		auto slot = interp.get_global_slot("context");
		if (slot >= 0)
		{
			if (interp.global_slots.size() <= static_cast<size_t>(slot))
				interp.global_slots.resize(static_cast<size_t>(slot) + 1, make_none());
			interp.global_slots[static_cast<size_t>(slot)] = ctx;
		}
	};
	populate_context_global();

	interp.functions_v2.clear();

	auto build_us2_for = [&](const std::string& name, const std::vector<UdonInstruction>& body, CodeLocation& out_err) -> bool
	{
		US2Function fn{};
		size_t frame_size = 0;
		auto fs_it = interp.function_frame_sizes.find(name);
		if (fs_it != interp.function_frame_sizes.end())
			frame_size = fs_it->second;
		if (!compile_to_us2(name, body, frame_size, fn, out_err))
			return false;
		auto param_it = interp.function_params.find(name);
		if (param_it != interp.function_params.end() && param_it->second)
			fn.params = *param_it->second;
		auto var_it = interp.function_variadic.find(name);
		if (var_it != interp.function_variadic.end())
			fn.variadic = !var_it->second.empty();
		auto ps_it = interp.function_param_slots.find(name);
		if (ps_it != interp.function_param_slots.end() && ps_it->second)
			fn.param_slots = *ps_it->second;
		auto vs_it = interp.function_variadic_slot.find(name);
		if (vs_it != interp.function_variadic_slot.end())
			fn.variadic_slot = vs_it->second;
		fn.name = name;
		interp.functions_v2[name] = std::move(fn);
		return true;
	};

	for (const auto& kv : interp.instructions)
	{
		if (!kv.second)
			continue;
		if (!build_us2_for(kv.first, *kv.second, res))
			return res;
	}

	if (!module_global_init.empty())
	{
		std::string init_fn = "__globals_init_" + std::to_string(interp.global_init_counter++);
		interp.instructions[init_fn] = std::make_shared<std::vector<UdonInstruction>>(module_global_init);
		interp.function_params[init_fn] = std::make_shared<std::vector<std::string>>();
		interp.function_param_slots[init_fn] = std::make_shared<std::vector<s32>>();
		interp.function_frame_sizes[init_fn] = 0;
		interp.function_variadic_slot[init_fn] = -1;
		if (!build_us2_for(init_fn, module_global_init, res))
			return res;
		UdonValue dummy;
		CodeLocation init_res = interp.run(init_fn, {}, dummy);
		if (init_res.has_error)
			return init_res;
	}

	return res;
}
