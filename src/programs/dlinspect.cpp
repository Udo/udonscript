#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#ifdef UDON_HAS_LIBDW
#include <dwarf.h>
#include <elfutils/libdwfl.h>
#include <elfutils/libdw.h>

namespace
{
	struct Parameter
	{
		std::string name;
		std::string type;
	};

	struct FunctionSignature
	{
		std::string name;
		std::string return_type;
		std::vector<Parameter> parameters;
		bool is_variadic = false;
	};

	std::string get_attr_string(Dwarf_Die* die, unsigned int attr_name)
	{
		Dwarf_Attribute attr;
		if (dwarf_attr_integrate(die, attr_name, &attr) == nullptr)
		{
			return {};
		}

		const char* value = dwarf_formstring(&attr);
		return value ? std::string(value) : std::string();
	}

	std::string format_anon(const char* kind, Dwarf_Die* die)
	{
		std::ostringstream oss;
		oss << kind << " <anon@0x" << std::hex << dwarf_dieoffset(die) << ">";
		return oss.str();
	}

	std::string describe_type(Dwarf_Die* die, int depth = 0)
	{
		if (die == nullptr)
		{
			return "void";
		}

		if (depth > 16)
		{
			return "...";
		}

		const int tag = dwarf_tag(die);
		switch (tag)
		{
			case DW_TAG_base_type:
			{
				std::string name = get_attr_string(die, DW_AT_name);
				if (!name.empty())
				{
					return name;
				}

				Dwarf_Attribute encoding_attr;
				Dwarf_Word encoding = 0;
				if (dwarf_attr(die, DW_AT_encoding, &encoding_attr) != nullptr &&
					dwarf_formudata(&encoding_attr, &encoding) == 0)
				{
					switch (encoding)
					{
						case DW_ATE_boolean:
							return "bool";
						case DW_ATE_float:
							return "float";
						case DW_ATE_signed:
						case DW_ATE_signed_char:
							return "int";
						case DW_ATE_unsigned:
						case DW_ATE_unsigned_char:
							return "unsigned int";
						default:
							break;
					}
				}

				return "<base>";
			}
			case DW_TAG_const_type:
			{
				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
										? dwarf_formref_die(&type_attr, &ref)
										: nullptr;
				return "const " + describe_type(target, depth + 1);
			}
			case DW_TAG_volatile_type:
			{
				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
										? dwarf_formref_die(&type_attr, &ref)
										: nullptr;
				return "volatile " + describe_type(target, depth + 1);
			}
			case DW_TAG_pointer_type:
			{
				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
										? dwarf_formref_die(&type_attr, &ref)
										: nullptr;
				return describe_type(target, depth + 1) + "*";
			}
			case DW_TAG_reference_type:
			{
				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
										? dwarf_formref_die(&type_attr, &ref)
										: nullptr;
				return describe_type(target, depth + 1) + "&";
			}
			case DW_TAG_rvalue_reference_type:
			{
				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
										? dwarf_formref_die(&type_attr, &ref)
										: nullptr;
				return describe_type(target, depth + 1) + "&&";
			}
			case DW_TAG_typedef:
			{
				std::string name = get_attr_string(die, DW_AT_name);
				if (!name.empty())
				{
					return name;
				}

				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
										? dwarf_formref_die(&type_attr, &ref)
										: nullptr;
				return describe_type(target, depth + 1);
			}
			case DW_TAG_enumeration_type:
			{
				std::string name = get_attr_string(die, DW_AT_name);
				return !name.empty() ? "enum " + name : format_anon("enum", die);
			}
			case DW_TAG_structure_type:
			{
				std::string name = get_attr_string(die, DW_AT_name);
				return !name.empty() ? "struct " + name : format_anon("struct", die);
			}
			case DW_TAG_class_type:
			{
				std::string name = get_attr_string(die, DW_AT_name);
				return !name.empty() ? "class " + name : format_anon("class", die);
			}
			case DW_TAG_union_type:
			{
				std::string name = get_attr_string(die, DW_AT_name);
				return !name.empty() ? "union " + name : format_anon("union", die);
			}
			case DW_TAG_array_type:
			{
				Dwarf_Attribute type_attr;
				Dwarf_Die ref;
				Dwarf_Die* base = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
									  ? dwarf_formref_die(&type_attr, &ref)
									  : nullptr;
				std::string base_name = describe_type(base, depth + 1);

				std::vector<std::string> dims;
				Dwarf_Die child;
				if (dwarf_child(die, &child) == 0)
				{
					do
					{
						if (dwarf_tag(&child) == DW_TAG_subrange_type)
						{
							Dwarf_Attribute count_attr;
							Dwarf_Attribute upper_attr;
							Dwarf_Word count = 0;
							if (dwarf_attr_integrate(&child, DW_AT_count, &count_attr) != nullptr &&
								dwarf_formudata(&count_attr, &count) == 0)
							{
								dims.push_back(std::to_string(count));
							}
							else if (dwarf_attr_integrate(&child, DW_AT_upper_bound, &upper_attr) != nullptr &&
									 dwarf_formudata(&upper_attr, &count) == 0)
							{
								dims.push_back(std::to_string(count + 1));
							}
						}
					} while (dwarf_siblingof(&child, &child) == 0);
				}

				if (dims.empty())
				{
					return base_name + "[]";
				}

				std::ostringstream oss;
				oss << base_name;
				for (const auto& dim : dims)
				{
					oss << "[" << dim << "]";
				}
				return oss.str();
			}
			case DW_TAG_subroutine_type:
			{
				std::ostringstream oss;

				Dwarf_Attribute ret_attr;
				Dwarf_Die ret_mem;
				Dwarf_Die* ret_die = (dwarf_attr_integrate(die, DW_AT_type, &ret_attr) != nullptr)
										 ? dwarf_formref_die(&ret_attr, &ret_mem)
										 : nullptr;
				oss << "(";

				bool first = true;
				Dwarf_Die child;
				if (dwarf_child(die, &child) == 0)
				{
					do
					{
						if (dwarf_tag(&child) == DW_TAG_formal_parameter)
						{
							Dwarf_Attribute param_attr;
							Dwarf_Die param_mem;
							Dwarf_Die* param_die = (dwarf_attr_integrate(&child, DW_AT_type, &param_attr) != nullptr)
													   ? dwarf_formref_die(&param_attr, &param_mem)
													   : nullptr;
							if (!first)
							{
								oss << ", ";
							}
							oss << describe_type(param_die, depth + 1);
							first = false;
						}
						else if (dwarf_tag(&child) == DW_TAG_unspecified_parameters)
						{
							if (!first)
							{
								oss << ", ";
							}
							oss << "...";
							first = false;
						}
					} while (dwarf_siblingof(&child, &child) == 0);
				}

				oss << ") -> " << describe_type(ret_die, depth + 1);
				return oss.str();
			}
			default:
				break;
		}

		return format_anon("type", die);
	}

	std::string describe_return_type(Dwarf_Die* die)
	{
		Dwarf_Attribute type_attr;
		Dwarf_Die ref;
		Dwarf_Die* target = (dwarf_attr_integrate(die, DW_AT_type, &type_attr) != nullptr)
								? dwarf_formref_die(&type_attr, &ref)
								: nullptr;
		return describe_type(target);
	}

	bool is_declaration_only(Dwarf_Die* die)
	{
		Dwarf_Attribute decl_attr;
		bool is_decl = false;
		if (dwarf_attr_integrate(die, DW_AT_declaration, &decl_attr) != nullptr)
		{
			dwarf_formflag(&decl_attr, &is_decl);
		}
		return is_decl;
	}

	FunctionSignature read_function(Dwarf_Die* die)
	{
		FunctionSignature sig;
		sig.name = get_attr_string(die, DW_AT_name);
		if (sig.name.empty())
		{
			sig.name = get_attr_string(die, DW_AT_linkage_name);
		}
		if (sig.name.empty())
		{
			sig.name = get_attr_string(die, DW_AT_MIPS_linkage_name);
		}

		if (sig.name.empty())
		{
			std::ostringstream oss;
			oss << "<anon@0x" << std::hex << dwarf_dieoffset(die) << ">";
			sig.name = oss.str();
		}

		sig.return_type = describe_return_type(die);

		Dwarf_Die child;
		int param_index = 0;
		if (dwarf_child(die, &child) == 0)
		{
			do
			{
				const int child_tag = dwarf_tag(&child);
				if (child_tag == DW_TAG_formal_parameter)
				{
					Parameter param;
					param.name = get_attr_string(&child, DW_AT_name);
					if (param.name.empty())
					{
						param.name = "arg" + std::to_string(param_index);
					}

					Dwarf_Attribute type_attr;
					Dwarf_Die type_mem;
					Dwarf_Die* type_die = (dwarf_attr_integrate(&child, DW_AT_type, &type_attr) != nullptr)
											  ? dwarf_formref_die(&type_attr, &type_mem)
											  : nullptr;
					param.type = describe_type(type_die);
					sig.parameters.push_back(std::move(param));
					++param_index;
				}
				else if (child_tag == DW_TAG_unspecified_parameters)
				{
					sig.is_variadic = true;
				}
			} while (dwarf_siblingof(&child, &child) == 0);
		}

		return sig;
	}

	void dump_function(const FunctionSignature& sig)
	{
		std::cout << sig.return_type << " " << sig.name << "(";
		for (size_t i = 0; i < sig.parameters.size(); ++i)
		{
			const auto& param = sig.parameters[i];
			if (i > 0)
			{
				std::cout << ", ";
			}
			std::cout << param.type;
			if (!param.name.empty())
			{
				std::cout << " " << param.name;
			}
		}

		if (sig.is_variadic)
		{
			if (!sig.parameters.empty())
			{
				std::cout << ", ";
			}
			std::cout << "...";
		}

		std::cout << ")\n";
	}

	void walk_die_tree(Dwarf_Die* die, std::vector<FunctionSignature>& out)
	{
		if (die == nullptr)
		{
			return;
		}

		if (dwarf_tag(die) == DW_TAG_subprogram && !is_declaration_only(die))
		{
			out.push_back(read_function(die));
		}

		Dwarf_Die child;
		if (dwarf_child(die, &child) == 0)
		{
			do
			{
				walk_die_tree(&child, out);
			} while (dwarf_siblingof(&child, &child) == 0);
		}
	}

	int inspect_object(const std::string& path)
	{
		Dwfl_Callbacks callbacks = {};
		callbacks.find_elf = dwfl_build_id_find_elf;
		callbacks.find_debuginfo = dwfl_standard_find_debuginfo;
		callbacks.section_address = dwfl_offline_section_address;

		Dwfl* dwfl = dwfl_begin(&callbacks);
		if (dwfl == nullptr)
		{
			std::cerr << "Failed to initialize libdwfl: " << dwfl_errmsg(dwfl_errno()) << "\n";
			return 1;
		}

		Dwfl_Module* module = dwfl_report_offline(dwfl, path.c_str(), path.c_str(), -1);
		if (module == nullptr)
		{
			std::cerr << "Failed to open object '" << path << "': " << dwfl_errmsg(dwfl_errno()) << "\n";
			dwfl_end(dwfl);
			return 1;
		}

		if (dwfl_report_end(dwfl, nullptr, nullptr) != 0)
		{
			std::cerr << "Failed to finalize DWARF report: " << dwfl_errmsg(dwfl_errno()) << "\n";
			dwfl_end(dwfl);
			return 1;
		}

		Dwarf_Addr bias = 0;
		Dwarf* dwarf = dwfl_module_getdwarf(module, &bias);
		if (dwarf == nullptr)
		{
			std::cerr << "No DWARF info found in '" << path << "'.\n";
			dwfl_end(dwfl);
			return 1;
		}

		std::vector<FunctionSignature> functions;
		Dwarf_Off offset = 0;
		Dwarf_Off next_offset = 0;
		size_t header_size = 0;
		while (dwarf_nextcu(dwarf, offset, &next_offset, &header_size, nullptr, nullptr, nullptr) == 0)
		{
			Dwarf_Die cu_die;
			if (dwarf_offdie(dwarf, offset + header_size, &cu_die) != nullptr)
			{
				walk_die_tree(&cu_die, functions);
			}

			offset = next_offset;
		}

		if (functions.empty())
		{
			std::cout << "No functions with debug info were found in '" << path << "'.\n";
			dwfl_end(dwfl);
			return 0;
		}

		for (const auto& fn : functions)
		{
			dump_function(fn);
		}

		dwfl_end(dwfl);
		return 0;
	}
} // namespace

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::cerr << "Usage: " << argv[0] << " <shared-object>\n";
		return 1;
	}

	return inspect_object(argv[1]);
}

#else

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	std::cerr << "dlinspect was built without libdw support; DWARF inspection is unavailable.\n";
	return 1;
}

#endif
