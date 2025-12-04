#include "helpers.h"
#include <sstream>
#include <cmath>
#include <memory>
#include <functional>

UdonValue make_none()
{
	return UdonValue();
}

UdonValue make_int(s64 v)
{
	UdonValue val{};
	val.type = UdonValue::Type::Int;
	val.int_value = v;
	return val;
}

UdonValue make_float(f64 v)
{
	UdonValue val{};
	val.type = UdonValue::Type::Float;
	val.float_value = v;
	return val;
}

UdonValue make_bool(bool v)
{
	UdonValue val{};
	val.type = UdonValue::Type::Bool;
	val.int_value = v ? 1 : 0;
	return val;
}

UdonValue make_string(const std::string& s)
{
	UdonValue val{};
	val.type = UdonValue::Type::String;
	val.string_value = s;
	return val;
}

UdonValue make_array()
{
	UdonValue v;
	v.type = UdonValue::Type::Array;
	if (g_udon_current)
		v.array_map = g_udon_current->allocate_array();
	else
		v.array_map = new UdonValue::ManagedArray();
	return v;
}

UdonValue::ManagedArray::~ManagedArray()
{
	Entry* e = head;
	while (e)
	{
		Entry* next = e->next;
		delete e;
		e = next;
	}
}

void ensure_array(UdonValue& v)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
	{
		v = make_array();
	}
}

std::string key_from_value(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::Int:
			return std::to_string(v.int_value);
		case UdonValue::Type::Float:
		{
			std::ostringstream ss;
			ss << v.float_value;
			return ss.str();
		}
		case UdonValue::Type::String:
			return v.string_value;
		default:
			return value_to_string(v);
	}
}

std::string value_to_string(const UdonValue& v)
{
	std::ostringstream ss;
	switch (v.type)
	{
		case UdonValue::Type::Int:
			ss << v.int_value;
			break;
		case UdonValue::Type::Float:
			ss << v.float_value;
			break;
		case UdonValue::Type::Bool:
			ss << (v.int_value ? "true" : "false");
			break;
		case UdonValue::Type::String:
			ss << v.string_value;
			break;
		case UdonValue::Type::Array:
		{
			ss << "[";
			if (v.array_map)
			{
				size_t count = 0;
				array_foreach(v, [&](const std::string& k, const UdonValue& val)
				{
					if (count > 0)
						ss << ", ";
					ss << k << ": " << value_to_string(val);
					if (++count > 8)
					{
						ss << "...";
						return false;
					}
					return true;
				});
			}
			ss << "]";
			break;
		}
		case UdonValue::Type::Function:
			ss << "<function:" << (v.function ? v.function->function_name : "null") << ">";
			break;
		case UdonValue::Type::None:
			ss << "none";
			break;
		default:
			ss << "<ref>";
			break;
	}
	return ss.str();
}

bool is_numeric(const UdonValue& v)
{
	return v.type == UdonValue::Type::Int || v.type == UdonValue::Type::Float || v.type == UdonValue::Type::Bool || v.type == UdonValue::Type::None;
}

bool is_integer_type(const UdonValue& v)
{
	return v.type == UdonValue::Type::Int || v.type == UdonValue::Type::Bool;
}

std::string value_type_name(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::Int:
			return "Int";
		case UdonValue::Type::Float:
			return "Float";
		case UdonValue::Type::Bool:
			return "Bool";
		case UdonValue::Type::String:
			return "String";
		case UdonValue::Type::Array:
			return "Array";
		case UdonValue::Type::Function:
			return "Function";
		case UdonValue::Type::None:
			return "None";
		default:
			return "Any";
	}
}
double as_number(const UdonValue& v)
{
	if (v.type == UdonValue::Type::Int)
		return static_cast<double>(v.int_value);
	if (v.type == UdonValue::Type::Float)
		return static_cast<double>(v.float_value);
	if (v.type == UdonValue::Type::Bool)
		return static_cast<double>(v.int_value);
	return 0.0;
}

UdonValue wrap_number(double d, const UdonValue& lhs, const UdonValue& rhs)
{
	return (is_integer_type(lhs) && is_integer_type(rhs))
			   ? make_int(static_cast<s64>(d))
			   : make_float(static_cast<f64>(d));
}

UdonValue wrap_number_unary(double d, const UdonValue& src)
{
	return is_integer_type(src)
			   ? make_int(static_cast<s64>(d))
			   : make_float(static_cast<f64>(d));
}

bool binary_numeric(const UdonValue& lhs, const UdonValue& rhs, double (*fn)(double, double), UdonValue& out)
{
	if (!is_numeric(lhs) || !is_numeric(rhs))
		return false;
	out = wrap_number(fn(as_number(lhs), as_number(rhs)), lhs, rhs);
	return true;
}

bool array_get(const UdonValue& v, const std::string& key, UdonValue& out)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return false;
	auto it = v.array_map->index.find(key);
	if (it == v.array_map->index.end())
		return false;
	out = it->second->value;
	return true;
}

void array_set(UdonValue& v, const std::string& key, const UdonValue& value)
{
	ensure_array(v);
	auto it = v.array_map->index.find(key);
	if (it != v.array_map->index.end())
	{
		it->second->value = value;
		return;
	}
	auto* entry = new UdonValue::ManagedArray::Entry();
	entry->key = key;
	entry->value = value;
	entry->prev = v.array_map->tail;
	if (v.array_map->tail)
		v.array_map->tail->next = entry;
	v.array_map->tail = entry;
	if (!v.array_map->head)
		v.array_map->head = entry;
	v.array_map->index[key] = entry;
	v.array_map->size++;
}

bool array_delete(UdonValue& v, const std::string& key, UdonValue* out)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return false;
	auto it = v.array_map->index.find(key);
	if (it == v.array_map->index.end())
		return false;
	auto* entry = it->second;
	if (out)
		*out = entry->value;
	if (entry->prev)
		entry->prev->next = entry->next;
	else
		v.array_map->head = entry->next;
	if (entry->next)
		entry->next->prev = entry->prev;
	else
		v.array_map->tail = entry->prev;
	v.array_map->index.erase(it);
	if (v.array_map->size > 0)
		v.array_map->size--;
	delete entry;
	return true;
}

void array_clear(UdonValue& v)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return;
	auto* arr = v.array_map;
	auto* e = arr->head;
	while (e)
	{
		auto* next = e->next;
		delete e;
		e = next;
	}
	arr->index.clear();
	arr->head = arr->tail = nullptr;
	arr->size = 0;
}

size_t array_length(const UdonValue& v)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return 0;
	return v.array_map->size;
}

void array_foreach(const UdonValue& v, const std::function<bool(const std::string&, const UdonValue&)>& fn)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return;
	auto* entry = v.array_map->head;
	while (entry)
	{
		if (!fn(entry->key, entry->value))
			break;
		entry = entry->next;
	}
}

bool equal_values(const UdonValue& a, const UdonValue& b, UdonValue& out)
{
	if (is_numeric(a) && is_numeric(b))
	{
		if (is_integer_type(a) && is_integer_type(b))
			out = make_bool(a.int_value == b.int_value);
		else
			out = make_bool(as_number(a) == as_number(b));
		return true;
	}
	if (a.type == UdonValue::Type::String || b.type == UdonValue::Type::String)
	{
		out = make_bool(value_to_string(a) == value_to_string(b));
		return true;
	}
	out = make_bool(false);
	return true;
}

bool compare_values(const UdonValue& a, const UdonValue& b, UdonInstruction::OpCode op, UdonValue& out)
{
	if (!is_numeric(a) || !is_numeric(b))
		return false;

	if (is_integer_type(a) && is_integer_type(b))
	{
		const s64 lhs = a.int_value;
		const s64 rhs = b.int_value;
		bool result = false;
		switch (op)
		{
			case UdonInstruction::OpCode::LT:
				result = lhs < rhs;
				break;
			case UdonInstruction::OpCode::LTE:
				result = lhs <= rhs;
				break;
			case UdonInstruction::OpCode::GT:
				result = lhs > rhs;
				break;
			case UdonInstruction::OpCode::GTE:
				result = lhs >= rhs;
				break;
			default:
				return false;
		}
		out = make_bool(result);
		return true;
	}

	const double lhs = as_number(a);
	const double rhs = as_number(b);

	bool result;
	switch (op)
	{
		case UdonInstruction::OpCode::LT:
			result = lhs < rhs;
			break;
		case UdonInstruction::OpCode::LTE:
			result = lhs <= rhs;
			break;
		case UdonInstruction::OpCode::GT:
			result = lhs > rhs;
			break;
		case UdonInstruction::OpCode::GTE:
			result = lhs >= rhs;
			break;
		default:
			return false;
	}

	out = make_bool(result);
	return true;
}

bool is_truthy(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::Int:
			return v.int_value != 0;
		case UdonValue::Type::Float:
			return v.float_value != 0.0;
		case UdonValue::Type::Bool:
			return v.int_value != 0;
		case UdonValue::Type::String:
			return !v.string_value.empty();
		case UdonValue::Type::Array:
			return v.array_map && v.array_map->size > 0;
		case UdonValue::Type::Function:
			return v.function != nullptr;
		default:
			return false;
	}
}

bool add_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
{
	if (lhs.type == UdonValue::Type::String || rhs.type == UdonValue::Type::String)
	{
		out = make_string(value_to_string(lhs) + value_to_string(rhs));
		return true;
	}
	if (is_integer_type(lhs) && is_integer_type(rhs))
	{
		out = make_int(lhs.int_value + rhs.int_value);
		return true;
	}
	return binary_numeric(lhs, rhs, [](double a, double b)
	{ return a + b; },
		out);
}

bool sub_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
{
	if (is_integer_type(lhs) && is_integer_type(rhs))
	{
		out = make_int(lhs.int_value - rhs.int_value);
		return true;
	}
	return binary_numeric(lhs, rhs, [](double a, double b)
	{ return a - b; },
		out);
}

bool mul_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
{
	if (is_integer_type(lhs) && is_integer_type(rhs))
	{
		out = make_int(lhs.int_value * rhs.int_value);
		return true;
	}
	return binary_numeric(lhs, rhs, [](double a, double b)
	{ return a * b; },
		out);
}

bool div_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
{
	if (is_integer_type(lhs) && is_integer_type(rhs))
	{
		if (rhs.int_value == 0)
			return false;
		out = make_int(lhs.int_value / rhs.int_value);
		return true;
	}
	if (!is_numeric(lhs) || !is_numeric(rhs))
		return false;
	const double r = as_number(rhs);
	if (r == 0.0)
		return false;
	out = wrap_number(as_number(lhs) / r, lhs, rhs);
	return true;
}

bool mod_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
{
	if (is_integer_type(lhs) && is_integer_type(rhs))
	{
		if (rhs.int_value == 0)
			return false;
		out = make_int(lhs.int_value % rhs.int_value);
		return true;
	}
	if (!is_numeric(lhs) || !is_numeric(rhs))
		return false;
	const double r = as_number(rhs);
	if (r == 0.0)
		return false;
	out = wrap_number(fmod(as_number(lhs), r), lhs, rhs);
	return true;
}
