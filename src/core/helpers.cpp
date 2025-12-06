#include "helpers.h"
#include <sstream>
#include <cmath>
#include <memory>
#include <functional>
#include <limits>

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
				array_foreach(v, [&](const UdonValue& k, const UdonValue& val)
				{
					if (count > 0)
						ss << ", ";
					ss << value_to_string(k) << ": " << value_to_string(val);
					count++;
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
	return v.type == UdonValue::Type::Int || v.type == UdonValue::Type::Float || v.type == UdonValue::Type::Bool || v.type == UdonValue::Type::None || v.type == UdonValue::Type::String || v.type == UdonValue::Type::Array;
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
	if (v.type == UdonValue::Type::String)
	{
		try
		{
			return std::stod(v.string_value);
		}
		catch (...)
		{
			return 0.0;
		}
	}
	if (v.type == UdonValue::Type::Array)
		return static_cast<double>(array_length(v));
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

bool array_get(const UdonValue& v, const UdonValue& key_in, UdonValue& out)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return false;
	UdonValue key = key_in;
	if (!is_hashable_value(key))
		key = make_string(value_to_string(key_in));
	auto* found = v.array_map->index.find(key);
	if (!found || !(*found))
		return false;
	out = (*found)->value;
	return true;
}

void array_set(UdonValue& v, const UdonValue& key_in, const UdonValue& value)
{
	ensure_array(v);
	UdonValue key = key_in;
	if (!is_hashable_value(key))
		key = make_string(value_to_string(key_in));

	const size_t h = hash_value(key);
	if (auto* entry_ptr = v.array_map->index.find(key))
	{
		if (*entry_ptr)
			(*entry_ptr)->value = value;
		return;
	}

	auto* entry = new UdonValue::ManagedArray::Entry();
	entry->key = key;
	entry->value = value;
	entry->hash = h;
	entry->prev = v.array_map->tail;
	if (v.array_map->tail)
		v.array_map->tail->next = entry;
	v.array_map->tail = entry;
	if (!v.array_map->head)
		v.array_map->head = entry;
	v.array_map->index.set(key, entry);
	v.array_map->size++;
}

bool array_delete(UdonValue& v, const UdonValue& key_in, UdonValue* out)
{
	if (v.type != UdonValue::Type::Array || !v.array_map)
		return false;
	UdonValue key = key_in;
	if (!is_hashable_value(key))
		key = make_string(value_to_string(key_in));
	auto* entry_ptr = v.array_map->index.find(key);
	if (!entry_ptr || !(*entry_ptr))
		return false;
	auto* entry = *entry_ptr;
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
	v.array_map->index.erase(key);
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

void array_foreach(const UdonValue& v, const std::function<bool(const UdonValue&, const UdonValue&)>& fn)
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

namespace
{
	bool try_integral_double(double d, s64& out)
	{
		double int_part = 0.0;
		if (std::modf(d, &int_part) != 0.0)
			return false;
		if (int_part < static_cast<double>(std::numeric_limits<s64>::min()) || int_part > static_cast<double>(std::numeric_limits<s64>::max()))
			return false;
		out = static_cast<s64>(int_part);
		return true;
	}
}

bool is_hashable_value(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::Int:
		case UdonValue::Type::Bool:
		case UdonValue::Type::String:
			return true;
		case UdonValue::Type::Float:
			return !std::isnan(v.float_value);
		default:
			return false;
	}
}

size_t hash_value(const UdonValue& v)
{
	UDON_ASSERT(is_hashable_value(v));
	switch (v.type)
	{
		case UdonValue::Type::Int:
			return std::hash<s64>()(v.int_value);
		case UdonValue::Type::Bool:
			return std::hash<int>()(v.int_value ? 1 : 0);
		case UdonValue::Type::String:
			return std::hash<std::string>()(v.string_value);
		case UdonValue::Type::Float:
		{
			double d = v.float_value;
			if (d == 0.0)
				d = 0.0; // normalize -0
			s64 as_int = 0;
			if (try_integral_double(d, as_int))
				return std::hash<s64>()(as_int);
			return std::hash<double>()(d);
		}
		default:
			return 0;
	}
}

bool hashable_values_equal(const UdonValue& a, const UdonValue& b)
{
	auto is_numberish = [](const UdonValue& v) -> bool
	{
		return v.type == UdonValue::Type::Int || v.type == UdonValue::Type::Float || v.type == UdonValue::Type::Bool;
	};

	if (a.type == UdonValue::Type::String || b.type == UdonValue::Type::String)
		return a.type == UdonValue::Type::String && b.type == UdonValue::Type::String && a.string_value == b.string_value;

	if (is_numberish(a) && is_numberish(b))
	{
		if (is_integer_type(a) && is_integer_type(b))
			return a.int_value == b.int_value;
		return as_number(a) == as_number(b);
	}

	return false;
}

bool equal_values(const UdonValue& a, const UdonValue& b, UdonValue& out)
{
	auto is_number = [](const UdonValue& v) -> bool
	{
		return v.type == UdonValue::Type::Int || v.type == UdonValue::Type::Float || v.type == UdonValue::Type::Bool;
	};

	if (a.type == UdonValue::Type::None || b.type == UdonValue::Type::None)
	{
		out = make_bool(a.type == UdonValue::Type::None && b.type == UdonValue::Type::None);
		return true;
	}

	if (a.type == UdonValue::Type::Array || b.type == UdonValue::Type::Array)
	{
		out = make_bool(a.type == UdonValue::Type::Array && b.type == UdonValue::Type::Array && a.array_map == b.array_map);
		return true;
	}

	if (a.type == UdonValue::Type::String && b.type == UdonValue::Type::String)
	{
		out = make_bool(a.string_value == b.string_value);
		return true;
	}

	if (is_number(a) && is_number(b))
	{
		if (is_integer_type(a) && is_integer_type(b))
			out = make_bool(a.int_value == b.int_value);
		else
			out = make_bool(as_number(a) == as_number(b));
		return true;
	}

	out = make_bool(false);
	return true;
}

bool compare_values(const UdonValue& a, const UdonValue& b, Opcode op, UdonValue& out)
{
	auto is_number = [](const UdonValue& v) -> bool
	{
		return v.type == UdonValue::Type::Int || v.type == UdonValue::Type::Float || v.type == UdonValue::Type::Bool;
	};

	if (a.type == UdonValue::Type::Array || b.type == UdonValue::Type::Array)
		return false;

	double lhs = 0.0;
	double rhs = 0.0;

	if (a.type == UdonValue::Type::String || b.type == UdonValue::Type::String)
	{
		lhs = as_number(a);
		rhs = as_number(b);
	}
	else if (is_number(a) && is_number(b))
	{
		lhs = as_number(a);
		rhs = as_number(b);
	}
	else
	{
		return false;
	}

	bool result = false;
	switch (op)
	{
		case Opcode::LT:
			result = lhs < rhs;
			break;
		case Opcode::LTE:
			result = lhs <= rhs;
			break;
		case Opcode::GT:
			result = lhs > rhs;
			break;
		case Opcode::GTE:
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
