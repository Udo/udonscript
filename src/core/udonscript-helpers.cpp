#include "udonscript-internal.h"
#include <sstream>
#include <cmath>
#include <memory>

namespace udon_script_helpers
{

	UdonValue make_none()
	{
		return UdonValue();
	}

	UdonValue make_int(s32 v)
	{
		UdonValue val;
		val.type = UdonValue::Type::S32;
		val.s32_value = v;
		return val;
	}

	UdonValue make_float(f32 v)
	{
		UdonValue val;
		val.type = UdonValue::Type::F32;
		val.f32_value = v;
		return val;
	}

	UdonValue make_bool(bool v)
	{
		UdonValue val;
		val.type = UdonValue::Type::Bool;
		val.s32_value = v ? 1 : 0;
		return val;
	}

	UdonValue make_string(const std::string& s)
	{
		UdonValue val;
		val.type = UdonValue::Type::String;
		val.string_value = s;
		return val;
	}

	UdonValue make_vec2(f32 x, f32 y)
	{
		UdonValue val;
		val.type = UdonValue::Type::Vector2;
		val.vec2_value = Vector2(x, y);
		return val;
	}

	UdonValue make_vec3(f32 x, f32 y, f32 z)
	{
		UdonValue val;
		val.type = UdonValue::Type::Vector3;
		val.vec3_value = Vector3(x, y, z);
		return val;
	}

	UdonValue make_vec4(f32 x, f32 y, f32 z, f32 w)
	{
		UdonValue val;
		val.type = UdonValue::Type::Vector4;
		val.vec4_value = Vector4(x, y, z, w);
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
			case UdonValue::Type::S32:
				return std::to_string(v.s32_value);
			case UdonValue::Type::F32:
			{
				std::ostringstream ss;
				ss << v.f32_value;
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
			case UdonValue::Type::S32:
				ss << v.s32_value;
				break;
			case UdonValue::Type::F32:
				ss << v.f32_value;
				break;
			case UdonValue::Type::Bool:
				ss << (v.s32_value ? "true" : "false");
				break;
			case UdonValue::Type::String:
				ss << v.string_value;
				break;
			case UdonValue::Type::Vector2:
				ss << "vec2(" << v.vec2_value.x << "," << v.vec2_value.y << ")";
				break;
			case UdonValue::Type::Vector3:
				ss << "vec3(" << v.vec3_value.x << "," << v.vec3_value.y << "," << v.vec3_value.z << ")";
				break;
			case UdonValue::Type::Vector4:
				ss << "vec4(" << v.vec4_value.x << "," << v.vec4_value.y << "," << v.vec4_value.z << "," << v.vec4_value.w << ")";
				break;
			case UdonValue::Type::Array:
			{
				ss << "[";
				if (v.array_map)
				{
					size_t count = 0;
					for (const auto& kv : v.array_map->values)
					{
						if (count > 0)
							ss << ", ";
						ss << kv.first << ": " << value_to_string(kv.second);
						if (++count > 8)
						{
							ss << "...";
							break;
						}
					}
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
		return v.type == UdonValue::Type::S32 || v.type == UdonValue::Type::F32 || v.type == UdonValue::Type::Bool || v.type == UdonValue::Type::None;
	}

	bool is_vector(const UdonValue& v)
	{
		return v.type == UdonValue::Type::Vector2 || v.type == UdonValue::Type::Vector3 || v.type == UdonValue::Type::Vector4;
	}

	int vector_dimension(const UdonValue& v)
	{
		switch (v.type)
		{
			case UdonValue::Type::Vector2:
				return 2;
			case UdonValue::Type::Vector3:
				return 3;
			case UdonValue::Type::Vector4:
				return 4;
			default:
				return 0;
		}
	}

	std::string value_type_name(const UdonValue& v)
	{
		switch (v.type)
		{
			case UdonValue::Type::S32:
				return "S32";
			case UdonValue::Type::F32:
				return "F32";
			case UdonValue::Type::Bool:
				return "Bool";
			case UdonValue::Type::String:
				return "String";
			case UdonValue::Type::Vector2:
				return "Vector2";
			case UdonValue::Type::Vector3:
				return "Vector3";
			case UdonValue::Type::Vector4:
				return "Vector4";
			case UdonValue::Type::Array:
				return "Array";
			case UdonValue::Type::Function:
				return "Function";
			default:
				return "Any";
		}
	}
	double as_number(const UdonValue& v)
	{
		if (v.type == UdonValue::Type::S32)
			return static_cast<double>(v.s32_value);
		if (v.type == UdonValue::Type::F32)
			return static_cast<double>(v.f32_value);
		if (v.type == UdonValue::Type::Bool)
			return static_cast<double>(v.s32_value);
		return 0.0;
	}

	UdonValue wrap_number(double d, const UdonValue& lhs, const UdonValue& rhs)
	{
		const bool wants_int = (lhs.type == UdonValue::Type::S32 || lhs.type == UdonValue::Type::Bool) && (rhs.type == UdonValue::Type::S32 || rhs.type == UdonValue::Type::Bool);
		if (wants_int)
			return make_int(static_cast<s32>(d));
		return make_float(static_cast<f32>(d));
	}

	UdonValue wrap_number_unary(double d, const UdonValue& src)
	{
		if (src.type == UdonValue::Type::S32 || src.type == UdonValue::Type::Bool)
			return make_int(static_cast<s32>(d));
		return make_float(static_cast<f32>(d));
	}

	static bool binary_numeric(const UdonValue& lhs, const UdonValue& rhs, double (*fn)(double, double), UdonValue& out)
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
		auto it = v.array_map->values.find(key);
		if (it == v.array_map->values.end())
			return false;
		out = it->second;
		return true;
	}

	void array_set(UdonValue& v, const std::string& key, const UdonValue& value)
	{
		ensure_array(v);
		v.array_map->values[key] = value;
	}

	UdonValue bool_value(bool v)
	{
		return make_bool(v);
	}

	bool equal_values(const UdonValue& a, const UdonValue& b, UdonValue& out)
	{
		if (is_numeric(a) && is_numeric(b))
		{
			out = bool_value(as_number(a) == as_number(b));
			return true;
		}
		if (is_vector(a) && is_vector(b) && vector_dimension(a) == vector_dimension(b))
		{
			bool eq = false;
			if (a.type == UdonValue::Type::Vector2)
				eq = a.vec2_value == b.vec2_value;
			else if (a.type == UdonValue::Type::Vector3)
				eq = a.vec3_value == b.vec3_value;
			else
				eq = a.vec4_value == b.vec4_value;
			out = bool_value(eq);
			return true;
		}
		if (a.type == UdonValue::Type::String || b.type == UdonValue::Type::String)
		{
			out = bool_value(value_to_string(a) == value_to_string(b));
			return true;
		}
		out = bool_value(false);
		return true;
	}

	bool compare_values(const UdonValue& a, const UdonValue& b, UdonInstruction::OpCode op, UdonValue& out)
	{
		if (!is_numeric(a) || !is_numeric(b))
			return false;
		double lhs = as_number(a);
		double rhs = as_number(b);
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
		out = bool_value(result);
		return true;
	}

	bool is_truthy(const UdonValue& v)
	{
		switch (v.type)
		{
			case UdonValue::Type::S32:
				return v.s32_value != 0;
			case UdonValue::Type::F32:
				return v.f32_value != 0.0f;
			case UdonValue::Type::Bool:
				return v.s32_value != 0;
			case UdonValue::Type::String:
				return !v.string_value.empty();
			case UdonValue::Type::Array:
				return v.array_map && !v.array_map->values.empty();
			case UdonValue::Type::Vector2:
				return v.vec2_value != Vector2(0.0f, 0.0f);
			case UdonValue::Type::Vector3:
				return v.vec3_value != Vector3(0.0f, 0.0f, 0.0f);
			case UdonValue::Type::Vector4:
				return v.vec4_value != Vector4(0.0f, 0.0f, 0.0f, 0.0f);
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
		if (is_vector(lhs))
		{
			int dim = vector_dimension(lhs);
			if (is_vector(rhs) && vector_dimension(rhs) == dim)
			{
				if (lhs.type == UdonValue::Type::Vector2)
					out = make_vec2(lhs.vec2_value.x + rhs.vec2_value.x, lhs.vec2_value.y + rhs.vec2_value.y);
				else if (lhs.type == UdonValue::Type::Vector3)
					out = make_vec3(lhs.vec3_value.x + rhs.vec3_value.x, lhs.vec3_value.y + rhs.vec3_value.y, lhs.vec3_value.z + rhs.vec3_value.z);
				else
					out = make_vec4(lhs.vec4_value.x + rhs.vec4_value.x, lhs.vec4_value.y + rhs.vec4_value.y, lhs.vec4_value.z + rhs.vec4_value.z, lhs.vec4_value.w + rhs.vec4_value.w);
				return true;
			}
			if (is_numeric(rhs))
			{
				float s = static_cast<float>(as_number(rhs));
				if (lhs.type == UdonValue::Type::Vector2)
					out = make_vec2(lhs.vec2_value.x + s, lhs.vec2_value.y + s);
				else if (lhs.type == UdonValue::Type::Vector3)
					out = make_vec3(lhs.vec3_value.x + s, lhs.vec3_value.y + s, lhs.vec3_value.z + s);
				else
					out = make_vec4(lhs.vec4_value.x + s, lhs.vec4_value.y + s, lhs.vec4_value.z + s, lhs.vec4_value.w + s);
				return true;
			}
		}
		if (is_vector(rhs))
		{
			return add_values(rhs, lhs, out);
		}
		return binary_numeric(lhs, rhs, [](double a, double b)
		{ return a + b; },
			out);
	}

	bool sub_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
	{
		if (is_vector(lhs))
		{
			int dim = vector_dimension(lhs);
			if (is_vector(rhs) && vector_dimension(rhs) == dim)
			{
				if (lhs.type == UdonValue::Type::Vector2)
					out = make_vec2(lhs.vec2_value.x - rhs.vec2_value.x, lhs.vec2_value.y - rhs.vec2_value.y);
				else if (lhs.type == UdonValue::Type::Vector3)
					out = make_vec3(lhs.vec3_value.x - rhs.vec3_value.x, lhs.vec3_value.y - rhs.vec3_value.y, lhs.vec3_value.z - rhs.vec3_value.z);
				else
					out = make_vec4(lhs.vec4_value.x - rhs.vec4_value.x, lhs.vec4_value.y - rhs.vec4_value.y, lhs.vec4_value.z - rhs.vec4_value.z, lhs.vec4_value.w - rhs.vec4_value.w);
				return true;
			}
			if (is_numeric(rhs))
			{
				float s = static_cast<float>(as_number(rhs));
				if (lhs.type == UdonValue::Type::Vector2)
					out = make_vec2(lhs.vec2_value.x - s, lhs.vec2_value.y - s);
				else if (lhs.type == UdonValue::Type::Vector3)
					out = make_vec3(lhs.vec3_value.x - s, lhs.vec3_value.y - s, lhs.vec3_value.z - s);
				else
					out = make_vec4(lhs.vec4_value.x - s, lhs.vec4_value.y - s, lhs.vec4_value.z - s, lhs.vec4_value.w - s);
				return true;
			}
			return false;
		}
		if (is_vector(rhs))
		{
			return false;
		}
		return binary_numeric(lhs, rhs, [](double a, double b)
		{ return a - b; },
			out);
	}

	bool mul_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
	{
		if (is_vector(lhs))
		{
			if (!is_numeric(rhs))
				return false;
			float s = static_cast<float>(as_number(rhs));
			if (lhs.type == UdonValue::Type::Vector2)
				out = make_vec2(lhs.vec2_value.x * s, lhs.vec2_value.y * s);
			else if (lhs.type == UdonValue::Type::Vector3)
				out = make_vec3(lhs.vec3_value.x * s, lhs.vec3_value.y * s, lhs.vec3_value.z * s);
			else
				out = make_vec4(lhs.vec4_value.x * s, lhs.vec4_value.y * s, lhs.vec4_value.z * s, lhs.vec4_value.w * s);
			return true;
		}
		if (is_vector(rhs))
		{
			return mul_values(rhs, lhs, out);
		}
		return binary_numeric(lhs, rhs, [](double a, double b)
		{ return a * b; },
			out);
	}

	bool div_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out)
	{
		if (is_vector(lhs))
		{
			if (!is_numeric(rhs))
				return false;
			double denom = as_number(rhs);
			if (denom == 0.0)
				return false;
			float s = static_cast<float>(denom);
			if (lhs.type == UdonValue::Type::Vector2)
				out = make_vec2(lhs.vec2_value.x / s, lhs.vec2_value.y / s);
			else if (lhs.type == UdonValue::Type::Vector3)
				out = make_vec3(lhs.vec3_value.x / s, lhs.vec3_value.y / s, lhs.vec3_value.z / s);
			else
				out = make_vec4(lhs.vec4_value.x / s, lhs.vec4_value.y / s, lhs.vec4_value.z / s, lhs.vec4_value.w / s);
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

} // namespace udon_script_helpers
