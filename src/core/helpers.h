#include "types.h"
#include "udonscript.h"

UdonValue make_none();
UdonValue make_int(s32 v);
UdonValue make_float(f32 v);
UdonValue make_bool(bool v);
UdonValue make_string(const std::string& s);
UdonValue make_array();
void ensure_array(UdonValue& v);
std::string key_from_value(const UdonValue& v);
std::string value_to_string(const UdonValue& v);
bool is_numeric(const UdonValue& v);
bool is_integer_type(const UdonValue& v);
std::string value_type_name(const UdonValue& v);
double as_number(const UdonValue& v);
UdonValue wrap_number(double d, const UdonValue& lhs, const UdonValue& rhs);
UdonValue wrap_number_unary(double d, const UdonValue& src);
bool binary_numeric(const UdonValue& lhs, const UdonValue& rhs, double (*fn)(double, double), UdonValue& out);
bool array_get(const UdonValue& v, const std::string& key, UdonValue& out);
void array_set(UdonValue& v, const std::string& key, const UdonValue& value);
bool equal_values(const UdonValue& a, const UdonValue& b, UdonValue& out);
bool compare_values(const UdonValue& a, const UdonValue& b, UdonInstruction::OpCode op, UdonValue& out);
bool is_truthy(const UdonValue& v);
bool add_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
bool sub_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
bool mul_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
bool div_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
bool mod_values(const UdonValue& lhs, const UdonValue& rhs, UdonValue& out);
