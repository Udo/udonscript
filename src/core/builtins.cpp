#include "helpers.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <cstdio>
#include <sys/stat.h>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iterator>
#include <unordered_set>
#include "jsx.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#include <unordered_set>

static bool parse_bool_string(const std::string& s, bool& out)
{
	std::string lowered;
	lowered.reserve(s.size());
	for (char c : s)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1")
	{
		out = true;
		return true;
	}
	if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0")
	{
		out = false;
		return true;
	}
	return false;
}

static bool parse_number_string(const std::string& s, double& out, bool& is_int)
{
	char* end = nullptr;
	out = std::strtod(s.c_str(), &end);
	if (end == s.c_str() || *end != '\0')
		return false;
	const bool has_dot = s.find('.') != std::string::npos;
	const bool has_exp = s.find('e') != std::string::npos || s.find('E') != std::string::npos;
	is_int = !(has_dot || has_exp);
	return true;
}

static std::string trim_string(const std::string& s, bool left, bool right)
{
	size_t start = 0;
	size_t end = s.size();
	if (left)
	{
		while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
			++start;
	}
	if (right)
	{
		while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
			--end;
	}
	return s.substr(start, end - start);
}

static std::string json_escape(const std::string& s)
{
	std::string out;
	for (char c : s)
	{
		switch (c)
		{
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				out.push_back(c);
				break;
		}
	}
	return out;
}

static std::string to_json(const UdonValue& v)
{
	switch (v.type)
	{
		case UdonValue::Type::String:
			return "\"" + json_escape(v.string_value) + "\"";
		case UdonValue::Type::Int:
			return std::to_string(v.int_value);
		case UdonValue::Type::Float:
		{
			std::ostringstream ss;
			ss << v.float_value;
			return ss.str();
		}
		case UdonValue::Type::Bool:
			return v.int_value ? "true" : "false";
		case UdonValue::Type::Array:
		{
			if (!v.array_map)
				return "null";
			std::ostringstream ss;
			ss << "{";
			bool first = true;
			array_foreach(v, [&](const std::string& k, const UdonValue& val)
			{
				if (!first)
					ss << ",";
				first = false;
				ss << "\"" << json_escape(k) << "\":" << to_json(val);
				return true;
			});
			ss << "}";
			return ss.str();
		}
		case UdonValue::Type::None:
		default:
			return "null";
	}
}

static std::string url_encode(const std::string& s)
{
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex;
	for (unsigned char c : s)
	{
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			escaped << c;
		else if (c == ' ')
			escaped << '+';
		else
			escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
	}
	return escaped.str();
}

static std::string url_decode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '+')
			out.push_back(' ');
		else if (s[i] == '%' && i + 2 < s.size())
		{
			std::string hex = s.substr(i + 1, 2);
			char ch = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
			out.push_back(ch);
			i += 2;
		}
		else
			out.push_back(s[i]);
	}
	return out;
}

static const std::string b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string to_base64_impl(const std::string& in)
{
	std::string out;
	int val = 0;
	int valb = -6;
	for (unsigned char c : in)
	{
		val = (val << 8) + c;
		valb += 8;
		while (valb >= 0)
		{
			out.push_back(b64_chars[(val >> valb) & 0x3F]);
			valb -= 6;
		}
	}
	if (valb > -6)
		out.push_back(b64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
	while (out.size() % 4)
		out.push_back('=');
	return out;
}

static std::string from_base64_impl(const std::string& in)
{
	std::vector<int> T(256, -1);
	for (int i = 0; i < 64; i++)
		T[b64_chars[i]] = i;
	std::string out;
	int val = 0;
	int valb = -8;
	for (unsigned char c : in)
	{
		if (T[c] == -1)
			break;
		val = (val << 6) + T[c];
		valb += 6;
		if (valb >= 0)
		{
			out.push_back(char((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	return out;
}

static const std::array<uint32_t, 256> crc32_table = []()
{
	std::array<uint32_t, 256> tbl{};
	for (uint32_t i = 0; i < 256; ++i)
	{
		uint32_t c = i;
		for (int j = 0; j < 8; ++j)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		tbl[i] = c;
	}
	return tbl;
}();

static uint32_t crc32(const std::string& data)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (unsigned char ch : data)
		crc = crc32_table[(crc ^ ch) & 0xFFu] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

struct Md5Ctx
{
	uint32_t h[4];
	uint64_t len = 0;
	std::string buffer;
};

static uint32_t md5_f(uint32_t x, uint32_t y, uint32_t z)
{
	return (x & y) | (~x & z);
}
static uint32_t md5_g(uint32_t x, uint32_t y, uint32_t z)
{
	return (x & z) | (y & ~z);
}
static uint32_t md5_h(uint32_t x, uint32_t y, uint32_t z)
{
	return x ^ y ^ z;
}
static uint32_t md5_i(uint32_t x, uint32_t y, uint32_t z)
{
	return y ^ (x | ~z);
}
static uint32_t md5_rot(uint32_t x, uint32_t c)
{
	return (x << c) | (x >> (32 - c));
}

static void md5_process_block(Md5Ctx& ctx, const unsigned char block[64])
{
	uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3];
	uint32_t x[16];
	for (int i = 0; i < 16; ++i)
		x[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
			   (static_cast<uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);

	auto R = [&](auto F, uint32_t& a_, uint32_t b_, uint32_t c_, uint32_t d_, uint32_t xk, uint32_t s, uint32_t ti)
	{
		a_ = b_ + md5_rot(a_ + F(b_, c_, d_) + xk + ti, s);
	};

	R(md5_f, a, b, c, d, x[0], 7, 0xd76aa478);
	R(md5_f, d, a, b, c, x[1], 12, 0xe8c7b756);
	R(md5_f, c, d, a, b, x[2], 17, 0x242070db);
	R(md5_f, b, c, d, a, x[3], 22, 0xc1bdceee);
	R(md5_f, a, b, c, d, x[4], 7, 0xf57c0faf);
	R(md5_f, d, a, b, c, x[5], 12, 0x4787c62a);
	R(md5_f, c, d, a, b, x[6], 17, 0xa8304613);
	R(md5_f, b, c, d, a, x[7], 22, 0xfd469501);
	R(md5_f, a, b, c, d, x[8], 7, 0x698098d8);
	R(md5_f, d, a, b, c, x[9], 12, 0x8b44f7af);
	R(md5_f, c, d, a, b, x[10], 17, 0xffff5bb1);
	R(md5_f, b, c, d, a, x[11], 22, 0x895cd7be);
	R(md5_f, a, b, c, d, x[12], 7, 0x6b901122);
	R(md5_f, d, a, b, c, x[13], 12, 0xfd987193);
	R(md5_f, c, d, a, b, x[14], 17, 0xa679438e);
	R(md5_f, b, c, d, a, x[15], 22, 0x49b40821);

	R(md5_g, a, b, c, d, x[1], 5, 0xf61e2562);
	R(md5_g, d, a, b, c, x[6], 9, 0xc040b340);
	R(md5_g, c, d, a, b, x[11], 14, 0x265e5a51);
	R(md5_g, b, c, d, a, x[0], 20, 0xe9b6c7aa);
	R(md5_g, a, b, c, d, x[5], 5, 0xd62f105d);
	R(md5_g, d, a, b, c, x[10], 9, 0x02441453);
	R(md5_g, c, d, a, b, x[15], 14, 0xd8a1e681);
	R(md5_g, b, c, d, a, x[4], 20, 0xe7d3fbc8);
	R(md5_g, a, b, c, d, x[9], 5, 0x21e1cde6);
	R(md5_g, d, a, b, c, x[14], 9, 0xc33707d6);
	R(md5_g, c, d, a, b, x[3], 14, 0xf4d50d87);
	R(md5_g, b, c, d, a, x[8], 20, 0x455a14ed);
	R(md5_g, a, b, c, d, x[13], 5, 0xa9e3e905);
	R(md5_g, d, a, b, c, x[2], 9, 0xfcefa3f8);
	R(md5_g, c, d, a, b, x[7], 14, 0x676f02d9);
	R(md5_g, b, c, d, a, x[12], 20, 0x8d2a4c8a);

	R(md5_h, a, b, c, d, x[5], 4, 0xfffa3942);
	R(md5_h, d, a, b, c, x[8], 11, 0x8771f681);
	R(md5_h, c, d, a, b, x[11], 16, 0x6d9d6122);
	R(md5_h, b, c, d, a, x[14], 23, 0xfde5380c);
	R(md5_h, a, b, c, d, x[1], 4, 0xa4beea44);
	R(md5_h, d, a, b, c, x[4], 11, 0x4bdecfa9);
	R(md5_h, c, d, a, b, x[7], 16, 0xf6bb4b60);
	R(md5_h, b, c, d, a, x[10], 23, 0xbebfbc70);
	R(md5_h, a, b, c, d, x[13], 4, 0x289b7ec6);
	R(md5_h, d, a, b, c, x[0], 11, 0xeaa127fa);
	R(md5_h, c, d, a, b, x[3], 16, 0xd4ef3085);
	R(md5_h, b, c, d, a, x[6], 23, 0x04881d05);
	R(md5_h, a, b, c, d, x[9], 4, 0xd9d4d039);
	R(md5_h, d, a, b, c, x[12], 11, 0xe6db99e5);
	R(md5_h, c, d, a, b, x[15], 16, 0x1fa27cf8);
	R(md5_h, b, c, d, a, x[2], 23, 0xc4ac5665);

	R(md5_i, a, b, c, d, x[0], 6, 0xf4292244);
	R(md5_i, d, a, b, c, x[7], 10, 0x432aff97);
	R(md5_i, c, d, a, b, x[14], 15, 0xab9423a7);
	R(md5_i, b, c, d, a, x[5], 21, 0xfc93a039);
	R(md5_i, a, b, c, d, x[12], 6, 0x655b59c3);
	R(md5_i, d, a, b, c, x[3], 10, 0x8f0ccc92);
	R(md5_i, c, d, a, b, x[10], 15, 0xffeff47d);
	R(md5_i, b, c, d, a, x[1], 21, 0x85845dd1);
	R(md5_i, a, b, c, d, x[8], 6, 0x6fa87e4f);
	R(md5_i, d, a, b, c, x[15], 10, 0xfe2ce6e0);
	R(md5_i, c, d, a, b, x[6], 15, 0xa3014314);
	R(md5_i, b, c, d, a, x[13], 21, 0x4e0811a1);
	R(md5_i, a, b, c, d, x[4], 6, 0xf7537e82);
	R(md5_i, d, a, b, c, x[11], 10, 0xbd3af235);
	R(md5_i, c, d, a, b, x[2], 15, 0x2ad7d2bb);
	R(md5_i, b, c, d, a, x[9], 21, 0xeb86d391);

	ctx.h[0] += a;
	ctx.h[1] += b;
	ctx.h[2] += c;
	ctx.h[3] += d;
}

static std::string md5(const std::string& data)
{
	Md5Ctx ctx{ { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u }, 0, "" };
	ctx.len = static_cast<uint64_t>(data.size()) * 8;
	ctx.buffer = data;

	ctx.buffer.push_back(static_cast<char>(0x80));
	while ((ctx.buffer.size() % 64) != 56)
		ctx.buffer.push_back(static_cast<char>(0x00));
	for (int i = 0; i < 8; ++i)
		ctx.buffer.push_back(static_cast<char>((ctx.len >> (8 * i)) & 0xFF));

	for (size_t i = 0; i < ctx.buffer.size(); i += 64)
		md5_process_block(ctx, reinterpret_cast<const unsigned char*>(ctx.buffer.data() + i));

	std::ostringstream ss;
	ss << std::hex << std::setfill('0');
	for (int i = 0; i < 4; ++i)
		ss << std::setw(2) << ((ctx.h[i] >> 0) & 0xFF) << std::setw(2) << ((ctx.h[i] >> 8) & 0xFF)
		   << std::setw(2) << ((ctx.h[i] >> 16) & 0xFF) << std::setw(2) << ((ctx.h[i] >> 24) & 0xFF);
	return ss.str();
}

struct Sha1Ctx
{
	uint32_t h[5];
	uint64_t len = 0;
	std::string buffer;
};

static uint32_t sha1_rot(uint32_t x, uint32_t s)
{
	return (x << s) | (x >> (32 - s));
}

static void sha1_process_block(Sha1Ctx& ctx, const unsigned char block[64])
{
	uint32_t w[80];
	for (int i = 0; i < 16; ++i)
	{
		w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
			   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | (static_cast<uint32_t>(block[i * 4 + 3]));
	}
	for (int i = 16; i < 80; ++i)
		w[i] = sha1_rot(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3], e = ctx.h[4];

	for (int i = 0; i < 80; ++i)
	{
		uint32_t f, k;
		if (i < 20)
		{
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		}
		else if (i < 40)
		{
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		}
		else if (i < 60)
		{
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		}
		else
		{
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		uint32_t temp = sha1_rot(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = sha1_rot(b, 30);
		b = a;
		a = temp;
	}

	ctx.h[0] += a;
	ctx.h[1] += b;
	ctx.h[2] += c;
	ctx.h[3] += d;
	ctx.h[4] += e;
}

static std::string sha1(const std::string& data)
{
	Sha1Ctx ctx{ { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u }, 0, "" };
	ctx.len = static_cast<uint64_t>(data.size()) * 8;
	ctx.buffer = data;
	ctx.buffer.push_back(static_cast<char>(0x80));
	while ((ctx.buffer.size() % 64) != 56)
		ctx.buffer.push_back(static_cast<char>(0x00));
	for (int i = 0; i < 8; ++i)
		ctx.buffer.push_back(static_cast<char>((ctx.len >> (56 - 8 * i)) & 0xFF));

	for (size_t i = 0; i < ctx.buffer.size(); i += 64)
		sha1_process_block(ctx, reinterpret_cast<const unsigned char*>(ctx.buffer.data() + i));

	std::ostringstream ss;
	ss << std::hex << std::setfill('0');
	for (int i = 0; i < 5; ++i)
		ss << std::setw(8) << ctx.h[i];
	return ss.str();
}

static int compare_for_sort(const UdonValue& a, const UdonValue& b)
{
	if (is_numeric(a) && is_numeric(b))
	{
		const double lhs = as_number(a);
		const double rhs = as_number(b);
		if (lhs < rhs)
			return -1;
		if (lhs > rhs)
			return 1;
		return 0;
	}
	const std::string sa = value_to_string(a);
	const std::string sb = value_to_string(b);
	if (sa < sb)
		return -1;
	if (sa > sb)
		return 1;
	return 0;
}

static UdonValue parse_form_data(const std::string& s, UdonInterpreter* interp)
{
	UdonValue out;
	out.type = UdonValue::Type::Array;
	out.array_map = interp->allocate_array();
	size_t pos = 0;
	while (pos < s.size())
	{
		size_t amp = s.find('&', pos);
		std::string pair = (amp == std::string::npos) ? s.substr(pos) : s.substr(pos, amp - pos);
		size_t eq = pair.find('=');
		std::string key = (eq == std::string::npos) ? pair : pair.substr(0, eq);
		std::string val = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
		key = url_decode(key);
		val = url_decode(val);
		array_set(out, key, make_string(val));
		if (amp == std::string::npos)
			break;
		pos = amp + 1;
	}
	return out;
}

struct JsonParser
{
	const std::string& s;
	size_t pos = 0;

	JsonParser(const std::string& str) : s(str) {}

	void skip_ws()
	{
		while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
			++pos;
	}

	bool parse_value(UdonValue& out)
	{
		skip_ws();
		if (pos >= s.size())
			return false;
		char c = s[pos];
		if (c == '"')
			return parse_string(out);
		if (c == '{')
			return parse_object(out);
		if (c == '[')
			return parse_array(out);
		if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+')
			return parse_number(out);
		if (s.compare(pos, 4, "true") == 0)
		{
			pos += 4;
			out = make_bool(true);
			return true;
		}
		if (s.compare(pos, 5, "false") == 0)
		{
			pos += 5;
			out = make_bool(false);
			return true;
		}
		if (s.compare(pos, 4, "null") == 0)
		{
			pos += 4;
			out = make_none();
			return true;
		}
		return false;
	}

	bool parse_string(UdonValue& out)
	{
		if (s[pos] != '"')
			return false;
		++pos;
		std::string val;
		while (pos < s.size())
		{
			char c = s[pos++];
			if (c == '"')
				break;
			if (c == '\\' && pos < s.size())
			{
				char esc = s[pos++];
				switch (esc)
				{
					case 'n':
						val.push_back('\n');
						break;
					case 'r':
						val.push_back('\r');
						break;
					case 't':
						val.push_back('\t');
						break;
					case '\\':
						val.push_back('\\');
						break;
					case '"':
						val.push_back('"');
						break;
					default:
						val.push_back(esc);
						break;
				}
			}
			else
			{
				val.push_back(c);
			}
		}
		out = make_string(val);
		return true;
	}

	bool parse_number(UdonValue& out)
	{
		size_t start = pos;
		if (s[pos] == '+' || s[pos] == '-')
			++pos;
		while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.'))
			++pos;
		std::string num = s.substr(start, pos - start);
		double d = std::atof(num.c_str());
		if (num.find('.') == std::string::npos)
			out = make_int(static_cast<s64>(d));
		else
			out = make_float(static_cast<f64>(d));
		return true;
	}

	bool parse_array(UdonValue& out)
	{
		if (s[pos] != '[')
			return false;
		++pos;
		out = make_array();
		int idx = 0;
		skip_ws();
		if (pos < s.size() && s[pos] == ']')
		{
			++pos;
			return true;
		}
		while (pos < s.size())
		{
			UdonValue val;
			if (!parse_value(val))
				return false;
			array_set(out, std::to_string(idx++), val);
			skip_ws();
			if (pos < s.size() && s[pos] == ',')
			{
				++pos;
				continue;
			}
			if (pos < s.size() && s[pos] == ']')
			{
				++pos;
				return true;
			}
			return false;
		}
		return false;
	}

	bool parse_object(UdonValue& out)
	{
		if (s[pos] != '{')
			return false;
		++pos;
		out = make_array();
		skip_ws();
		if (pos < s.size() && s[pos] == '}')
		{
			++pos;
			return true;
		}
		while (pos < s.size())
		{
			UdonValue key;
			if (!parse_string(key))
				return false;
			skip_ws();
			if (pos >= s.size() || s[pos] != ':')
				return false;
			++pos;
			UdonValue val;
			if (!parse_value(val))
				return false;
			array_set(out, key.string_value, val);
			skip_ws();
			if (pos < s.size() && s[pos] == ',')
			{
				++pos;
				skip_ws();
				continue;
			}
			if (pos < s.size() && s[pos] == '}')
			{
				++pos;
				return true;
			}
			return false;
		}
		return false;
	}
};

void register_builtins(UdonInterpreter* interp)
{
	auto unary = [interp](const std::string& name, double (*fn)(double))
	{
		interp->register_function(name, "x:number", "number", [fn, name](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 1 || !is_numeric(positional[0]))
			{
				err.has_error = true;
				err.opt_error_message = name + " expects 1 numeric argument";
				return true;
			}
			out = wrap_number_unary(fn(as_number(positional[0])), positional[0]);
			return true;
		});
	};

	auto binary = [interp](const std::string& name, double (*fn)(double, double))
	{
		interp->register_function(name, "a:number, b:number", "number", [fn, name](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2 || !is_numeric(positional[0]) || !is_numeric(positional[1]))
			{
				err.has_error = true;
				err.opt_error_message = name + " expects 2 numeric arguments";
				return true;
			}
			out = wrap_number(fn(as_number(positional[0]), as_number(positional[1])), positional[0], positional[1]);
			return true;
		});
	};

	interp->register_function("array", "values:any...", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		out.type = UdonValue::Type::Array;
		out.array_map = interp->allocate_array();
		int idx = 0;
		for (const auto& v : positional)
		{
			array_set(out, std::to_string(idx++), v);
		}
		return true;
	});

	interp->register_function("__object_literal", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty())
		{
			err.has_error = true;
			err.opt_error_message = "__object_literal: internal error - no arguments";
			return true;
		}

		if (positional.back().type != UdonValue::Type::Int)
		{
			err.has_error = true;
			err.opt_error_message = "__object_literal: internal error - invalid count";
			return true;
		}

		const s64 count = positional.back().int_value;
		if (count < 0)
		{
			err.has_error = true;
			err.opt_error_message = "__object_literal: internal error - negative count";
			return true;
		}

		if (positional.size() != static_cast<size_t>(count * 2 + 1))
		{
			err.has_error = true;
			err.opt_error_message = "__object_literal: internal error - arg count mismatch";
			return true;
		}

		out.type = UdonValue::Type::Array;
		out.array_map = interp->allocate_array();

		for (s64 i = 0; i < count; i++)
		{
			const UdonValue& key = positional[static_cast<size_t>(count + i)];
			const UdonValue& UdonValue = positional[static_cast<size_t>(i)];

			std::string key_str = key_from_value(key);

			array_set(out, key_str, UdonValue);
		}

		return true;
	});

	interp->register_function("print", "values:any...", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		std::ostringstream ss;
		bool first = true;
		for (const auto& v : positional)
		{
			if (!first)
				ss << " ";
			first = false;
			ss << value_to_string(v);
		}
		std::cout << ss.str() << std::endl;
		out = make_none();
		return true;
	});

	interp->register_function("puts", "values:any...", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		std::ostringstream ss;
		for (const auto& v : positional)
		{
			ss << value_to_string(v);
		}
		std::cout << ss.str();
		out = make_none();
		return true;
	});

	interp->register_function("__gc_collect", "budget_ms?:int", "none", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		u32 budget = 0;
		if (!positional.empty())
		{
			if (positional[0].type != UdonValue::Type::Int)
			{
				err.has_error = true;
				err.opt_error_message = "__gc_collect expects an optional integer budget (ms)";
				return true;
			}
			const s64 val = positional[0].int_value;
			if (val > 0)
				budget = static_cast<u32>(val);
		}
		interp->collect_garbage(nullptr, nullptr, budget);
		out = make_none();
		return true;
	});

	interp->register_function("__gc_stats", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		out = make_array();
		array_set(out, "envs", make_int(static_cast<s64>(interp->heap_environments.size())));
		array_set(out, "arrays", make_int(static_cast<s64>(interp->heap_arrays.size())));
		array_set(out, "functions", make_int(static_cast<s64>(interp->heap_functions.size())));
		array_set(out, "stack_roots", make_int(static_cast<s64>(interp->stack.size())));
		array_set(out, "active_env_root_sets", make_int(static_cast<s64>(interp->active_env_roots.size())));
		array_set(out, "active_value_root_sets", make_int(static_cast<s64>(interp->active_value_roots.size())));
		array_set(out, "gc_runs", make_int(static_cast<s64>(interp->gc_runs)));
		array_set(out, "gc_ms", make_int(static_cast<s64>(interp->gc_time_ms)));
		return true;
	});

	interp->register_function("globals", "", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		out = make_array();
		for (const auto& kv : interp->globals)
			array_set(out, kv.first, kv.second);
		return true;
	});

	auto register_alias = [interp](const std::string& alias, const std::string& target)
	{
		auto it = interp->builtins.find(target);
		if (it != interp->builtins.end())
			interp->builtins[alias] = it->second;
	};

	interp->register_function("keys", "arr:any", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty())
		{
			err.has_error = true;
			err.opt_error_message = "keys expects an array";
			return true;
		}

		out.type = UdonValue::Type::Array;
		out.array_map = interp->allocate_array();

		int idx = 0;
		if (positional[0].type == UdonValue::Type::Array && positional[0].array_map)
		{
			std::vector<std::string> key_list;
			key_list.reserve(array_length(positional[0]));
			array_foreach(positional[0], [&](const std::string& k, const UdonValue&)
			{
				key_list.push_back(k);
				return true;
			});
			std::sort(key_list.begin(), key_list.end(), [](const std::string& a, const std::string& b)
			{
				char* end_a = nullptr;
				char* end_b = nullptr;
				const s64 ia = std::strtoll(a.c_str(), &end_a, 10);
				const s64 ib = std::strtoll(b.c_str(), &end_b, 10);
				const bool a_num = end_a && *end_a == '\0';
				const bool b_num = end_b && *end_b == '\0';
				if (a_num && b_num)
					return ia < ib;
				if (a_num != b_num)
					return a_num; // numeric keys before non-numeric
				return a < b;
			});
			for (const auto& key : key_list)
			{
				std::string idx_str = std::to_string(idx);
				array_set(out, idx_str, make_string(key));
				idx++;
			}
		}
		else if (positional[0].type == UdonValue::Type::String)
		{
			for (size_t i = 0; i < positional[0].string_value.size(); ++i)
				array_set(out, std::to_string(idx++), make_string(std::to_string(i)));
		}
		else
		{
			err.has_error = true;
			err.opt_error_message = "keys expects an array";
		}

		return true;
	});

	interp->register_function("sort", "arr:any, options?:any", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty() || positional[0].type != UdonValue::Type::Array || !positional[0].array_map)
		{
			err.has_error = true;
			err.opt_error_message = "sort expects (array, [options])";
			return true;
		}

		const UdonValue options = (positional.size() >= 2) ? positional[1] : make_none();
		auto get_opt = [&options](const std::string& key, UdonValue& out_val) -> bool
		{
			if (options.type != UdonValue::Type::Array || !options.array_map)
				return false;
			if (!array_get(options, key, out_val))
				return false;
			return true;
		};

		bool reverse = false;
		bool keep_keys = false;
		bool by_key = false;
		UdonValue key_fn;
		bool has_key_fn = false;

		UdonValue opt;
		if (get_opt("reverse", opt))
			reverse = is_truthy(opt);
		if (get_opt("keep_keys", opt))
			keep_keys = is_truthy(opt);
		if (get_opt("by", opt) && opt.type == UdonValue::Type::String)
			by_key = (opt.string_value == "key");
		if (get_opt("key", opt))
		{
			if (opt.type != UdonValue::Type::Function || !opt.function)
			{
				err.has_error = true;
				err.opt_error_message = "sort options.key must be a function";
				return true;
			}
			key_fn = opt;
			has_key_fn = true;
		}

		struct Entry
		{
			std::string key;
			UdonValue value;
			UdonValue sort_value;
			size_t original_index = 0;
		};

		std::vector<Entry> entries;
		entries.reserve(array_length(positional[0]));
		size_t original_idx = 0;
		array_foreach(positional[0], [&](const std::string& k, const UdonValue& v)
		{
			Entry e;
			e.key = k;
			e.value = v;
			e.original_index = original_idx++;

			UdonValue base = by_key ? make_string(k) : v;
			if (has_key_fn)
			{
				std::vector<UdonValue> args;
				args.push_back(base);
				UdonValue key_out;
				CodeLocation call_err = interp->invoke_function(key_fn, args, {}, key_out);
				if (call_err.has_error)
				{
					err = call_err;
					return true;
				}
				e.sort_value = key_out;
			}
			else
			{
				e.sort_value = base;
			}
			entries.push_back(e);
			return true;
		});

		std::sort(entries.begin(), entries.end(), [reverse](const Entry& a, const Entry& b)
		{
			const int cmp = compare_for_sort(a.sort_value, b.sort_value);
			if (cmp == 0)
				return a.original_index < b.original_index;
			return reverse ? (cmp > 0) : (cmp < 0);
		});

		out = make_array();
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const Entry& e = entries[i];
			if (keep_keys)
				array_set(out, e.key, e.value);
			else
				array_set(out, std::to_string(i), e.value);
		}
		return true;
	});

	interp->register_function("ksort", "arr:any, options?:any", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty() || positional[0].type != UdonValue::Type::Array || !positional[0].array_map)
		{
			err.has_error = true;
			err.opt_error_message = "ksort expects (array, [options])";
			return true;
		}
		bool reverse = false;
		if (positional.size() >= 2 && positional[1].type == UdonValue::Type::Array)
		{
			UdonValue opt;
			if (array_get(positional[1], "reverse", opt))
				reverse = is_truthy(opt);
		}
		std::vector<std::pair<std::string, UdonValue>> entries;
		entries.reserve(array_length(positional[0]));
		array_foreach(positional[0], [&](const std::string& k, const UdonValue& v)
		{
			entries.emplace_back(k, v);
			return true;
		});

		auto key_cmp = [](const std::string& a, const std::string& b) -> bool
		{
			char* end_a = nullptr;
			char* end_b = nullptr;
			const s64 ia = std::strtoll(a.c_str(), &end_a, 10);
			const s64 ib = std::strtoll(b.c_str(), &end_b, 10);
			const bool a_num = end_a && *end_a == '\0';
			const bool b_num = end_b && *end_b == '\0';
			if (a_num && b_num)
				return ia < ib;
			if (a_num != b_num)
				return a_num;
			return a < b;
		};

		std::sort(entries.begin(), entries.end(), [&](const auto& lhs, const auto& rhs)
		{
			if (lhs.first == rhs.first)
				return false;
			return reverse ? key_cmp(rhs.first, lhs.first) : key_cmp(lhs.first, rhs.first);
		});

		out = make_array();
		for (const auto& kv : entries)
			array_set(out, kv.first, kv.second);
		return true;
	});

	interp->register_function("array_get", "arr:any, key:any", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() < 2)
		{
			err.has_error = true;
			err.opt_error_message = "array_get expects (array, key)";
			return true;
		}

		std::string key_str = key_from_value(positional[1]);
		if (positional[0].type == UdonValue::Type::Array)
		{
			if (!array_get(positional[0], key_str, out))
				out = make_none();
		}
		else if (positional[0].type == UdonValue::Type::String)
		{
			try
			{
				const s64 idx = std::stoll(key_str);
				if (idx >= 0 && static_cast<size_t>(idx) < positional[0].string_value.size())
					out = make_string(std::string(1, positional[0].string_value[static_cast<size_t>(idx)]));
				else
					out = make_none();
			}
			catch (...)
			{
				out = make_none();
			}
		}
		else
		{
			out = make_none();
		}

		return true;
	});

	interp->register_function("load_from_file", "path:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "load_from_file expects (path)";
			return true;
		}
		std::string path = value_to_string(positional[0]);
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			err.has_error = true;
			err.opt_error_message = "Could not read file: " + path;
			return true;
		}
		std::ostringstream ss;
		ss << file.rdbuf();
		out = make_string(ss.str());
		return true;
	});

	interp->register_function("save_to_file", "path:string, data:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2)
		{
			err.has_error = true;
			err.opt_error_message = "save_to_file expects (path, data)";
			return true;
		}
		std::string path = value_to_string(positional[0]);
		std::ofstream file(path, std::ios::binary);
		if (!file)
		{
			err.has_error = true;
			err.opt_error_message = "Could not write file: " + path;
			return true;
		}
		file << value_to_string(positional[1]);
		file.close();
		out = make_none();
		return true;
	});

	interp->register_function("read_entire_file", "path:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "read_entire_file expects (path)";
			return true;
		}
		std::string path = value_to_string(positional[0]);
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			err.has_error = true;
			err.opt_error_message = "Could not read file: " + path;
			return true;
		}
		std::ostringstream ss;
		ss << file.rdbuf();
		out = make_string(ss.str());
		return true;
	});

	interp->register_function("write_entire_file", "path:string, data:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2)
		{
			err.has_error = true;
			err.opt_error_message = "write_entire_file expects (path, data)";
			return true;
		}
		std::string path = value_to_string(positional[0]);
		std::ofstream file(path, std::ios::binary);
		if (!file)
		{
			err.has_error = true;
			err.opt_error_message = "Could not write file: " + path;
			return true;
		}
		file << value_to_string(positional[1]);
		file.close();
		out = make_none();
		return true;
	});

	interp->register_function("dl_open", "path:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
#if !defined(__unix__) && !defined(__APPLE__)
		(void)interp;
		(void)positional;
		(void)out;
		err.has_error = true;
		err.opt_error_message = "dl_open is only supported on POSIX platforms";
		return true;
#else
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "dl_open expects a single string path";
			return true;
		}
		std::string path = positional[0].string_value;
		void* handle = dlopen(path.c_str(), RTLD_NOW);
		if (!handle)
		{
			err.has_error = true;
			err.opt_error_message = std::string("dl_open failed: ") + dlerror();
			return true;
		}
		s32 handle_id = interp->register_dl_handle(handle);

		out = make_array();
		array_set(out, "_handle", make_int(handle_id));

		struct DlHandleCtx
		{
			s32 handle_id = -1;
		};
		auto ctx = std::make_shared<DlHandleCtx>();
		ctx->handle_id = handle_id;

		auto make_handler = [&](auto fn) -> UdonValue
		{
			UdonValue fnv{};
			fnv.type = UdonValue::Type::Function;
			fnv.function = interp->allocate_function();
			fnv.function->user_data = ctx;
			fnv.function->native_handler = fn;
			return fnv;
		};

		auto call_handler = [ctx](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err) -> bool
		{
#if defined(__unix__) || defined(__APPLE__)
			if (positional.size() < 1)
			{
				err.has_error = true;
				err.opt_error_message = "dl_call expects (symbol, args...)";
				return true;
			}
			const UdonValue& symbol_val = positional[0];
			if (symbol_val.type != UdonValue::Type::String)
			{
				err.has_error = true;
				err.opt_error_message = "dl_call symbol must be a string";
				return true;
			}
			void* handle = interp->get_dl_handle(ctx->handle_id);
			if (!handle)
			{
				err.has_error = true;
				err.opt_error_message = "dl_call: invalid handle";
				return true;
			}
			std::string sig_text = symbol_val.string_value;
			std::string sym_name = sig_text;
			std::vector<std::string> arg_types;
			std::string ret_type = "float";
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
				err.has_error = true;
				err.opt_error_message = "dl_call: symbol not found";
				return true;
			}

			std::vector<double> args;
			if (!arg_types.empty())
			{
				if (positional.size() - 1 != arg_types.size())
				{
					err.has_error = true;
					err.opt_error_message = "dl_call: argument count mismatch";
					return true;
				}
				for (size_t i = 0; i < arg_types.size(); ++i)
				{
					const UdonValue& v = positional[i + 1];
					std::string t = arg_types[i];
					if (t == "int" || t == "s32" || t == "s64")
					{
						if (v.type == UdonValue::Type::Int)
							args.push_back(static_cast<double>(v.int_value));
						else if (v.type == UdonValue::Type::Float)
							args.push_back(static_cast<double>(v.float_value));
						else
						{
							err.has_error = true;
							err.opt_error_message = "dl_call: expected int argument";
							return true;
						}
					}
					else if (t == "float" || t == "f32" || t == "f64" || t == "double")
					{
						if (v.type == UdonValue::Type::Float)
							args.push_back(static_cast<double>(v.float_value));
						else if (v.type == UdonValue::Type::Int)
							args.push_back(static_cast<double>(v.int_value));
						else
						{
							err.has_error = true;
							err.opt_error_message = "dl_call: expected float argument";
							return true;
						}
					}
					else
					{
						err.has_error = true;
						err.opt_error_message = "dl_call: unsupported argument type '" + t + "'";
						return true;
					}
				}
			}
			else
			{
				for (size_t i = 1; i < positional.size(); ++i)
				{
					const UdonValue& v = positional[i];
					if (v.type == UdonValue::Type::Int)
						args.push_back(static_cast<double>(v.int_value));
					else if (v.type == UdonValue::Type::Float)
						args.push_back(static_cast<double>(v.float_value));
					else
					{
						err.has_error = true;
						err.opt_error_message = "dl_call only supports numeric arguments";
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
					err.has_error = true;
					err.opt_error_message = "dl_call supports up to 4 arguments";
					return true;
			}
			if (ret_type == "int" || ret_type == "s32" || ret_type == "s64")
				out = make_int(static_cast<s64>(result));
			else
				out = make_float(static_cast<f64>(result));
			return true;
#else
			(void)interp;
			(void)positional;
			(void)out;
			err.has_error = true;
			err.opt_error_message = "dl_call not supported on this platform";
			return true;
#endif
		};

		auto close_handler = [ctx](UdonInterpreter* interp, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err) -> bool
		{
#if defined(__unix__) || defined(__APPLE__)
			if (!interp->close_dl_handle(ctx->handle_id))
			{
				err.has_error = true;
				err.opt_error_message = "dl_close: invalid handle";
				return true;
			}
			out = make_none();
			return true;
#else
			(void)interp;
			(void)out;
			err.has_error = true;
			err.opt_error_message = "dl_close not supported on this platform";
			return true;
#endif
		};

		array_set(out, "call", make_handler(call_handler));
		array_set(out, "close", make_handler(close_handler));
		return true;
#endif
	});

	interp->register_function("file_size", "path:string", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "file_size expects (path)";
			return true;
		}
		std::string path = value_to_string(positional[0]);
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
		{
			err.has_error = true;
			err.opt_error_message = "Could not access file: " + path;
			return true;
		}
		std::streampos size = file.tellg();
		out = make_int(static_cast<s64>(size));
		return true;
	});

	interp->register_function("file_time", "path:string", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "file_time expects (path)";
			return true;
		}
		std::string path = value_to_string(positional[0]);
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
		{
			err.has_error = true;
			err.opt_error_message = "Could not access file: " + path;
			return true;
		}
		out = make_int(static_cast<s64>(st.st_mtime));
		return true;
	});

	interp->register_function("import", "path:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "import expects a single string path";
			return true;
		}

		std::string path = positional[0].string_value;
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			err.has_error = true;
			err.opt_error_message = "import: could not open '" + path + "'";
			return true;
		}
		std::ostringstream ss;
		ss << file.rdbuf();
		std::string source = ss.str();

		std::unique_ptr<UdonInterpreter> sub = std::make_unique<UdonInterpreter>();
		sub->builtins = interp->builtins; // share host-registered builtins

		CodeLocation compile_res = sub->compile(source);
		if (compile_res.has_error)
		{
			err = compile_res;
			return true;
		}

		s32 sub_id = interp->register_imported_interpreter(std::move(sub));

		out = make_array();
		UdonInterpreter* sub_ref = interp->get_imported_interpreter(sub_id);
		if (sub_ref)
		{
			for (const auto& kv : sub_ref->globals)
			{
				array_set(out, kv.first, kv.second);
			}
			struct ImportForwardCtx
			{
				s32 sub_id = -1;
				std::string fn;
			};
			for (const auto& kv : sub_ref->instructions)
			{
				const std::string& name = kv.first;
				if (name.rfind("__", 0) == 0)
					continue;
				auto ctx = std::make_shared<ImportForwardCtx>();
				ctx->sub_id = sub_id;
				ctx->fn = name;

				UdonValue fn_val;
				fn_val.type = UdonValue::Type::Function;
				fn_val.function = interp->allocate_function();
				fn_val.function->template_body = name;
				fn_val.function->user_data = ctx;
				fn_val.function->native_handler = [ctx](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>& named, UdonValue& out, CodeLocation& inner_err) -> bool
				{
					UdonInterpreter* sub = interp->get_imported_interpreter(ctx->sub_id);
					if (!sub)
					{
						inner_err.has_error = true;
						inner_err.opt_error_message = "import_forward: invalid module";
						return true;
					}
					CodeLocation nested = sub->run(ctx->fn, positional, named, out);
					if (nested.has_error)
						inner_err = nested;
					return true;
				};
				array_set(out, name, fn_val);
			}
		}
		return true;
	});

	interp->register_function("shell", "parts:any...", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty())
		{
			err.has_error = true;
			err.opt_error_message = "shell expects at least one argument";
			return true;
		}

		std::ostringstream command_builder;
		for (size_t i = 0; i < positional.size(); i++)
		{
			if (i > 0)
				command_builder << " ";
			command_builder << value_to_string(positional[i]);
		}
		std::string command = command_builder.str();

		FILE* pipe = popen(command.c_str(), "r");
		if (!pipe)
		{
			err.has_error = true;
			err.opt_error_message = "Failed to execute command: " + command;
			return true;
		}

		std::ostringstream result;
		char buffer[256];
		while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
		{
			result << buffer;
		}

		int exit_code = pclose(pipe);
		(void)exit_code; // Ignore exit code for now

		std::string output = result.str();
		if (!output.empty() && output.back() == '\n')
			output.pop_back();

		out = make_string(output);
		return true;
	});

	interp->register_function("to_shellarg", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_shellarg expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);

		std::string escaped = "'";
		for (char c : s)
		{
			if (c == '\'')
				escaped += "'\\''";
			else
				escaped += c;
		}
		escaped += "'";

		out = make_string(escaped);
		return true;
	});

	interp->register_function("to_htmlsafe", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_htmlsafe expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);

		std::string escaped;
		for (char c : s)
		{
			switch (c)
			{
				case '&':
					escaped += "&amp;";
					break;
				case '<':
					escaped += "&lt;";
					break;
				case '>':
					escaped += "&gt;";
					break;
				case '"':
					escaped += "&quot;";
					break;
				case '\'':
					escaped += "&#39;";
					break;
				default:
					escaped += c;
					break;
			}
		}

		out = make_string(escaped);
		return true;
	});

	interp->register_function("to_sqlarg", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_sqlarg expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);

		std::string escaped;
		for (char c : s)
		{
			if (c == '\'')
				escaped += "''";
			else
				escaped += c;
		}

		out = make_string(escaped);
		return true;
	});

	interp->register_function("split", "s:string, delim:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2)
		{
			err.has_error = true;
			err.opt_error_message = "split expects (string, delim)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::string delim = value_to_string(positional[1]);
		out.type = UdonValue::Type::Array;
		out.array_map = interp->allocate_array();
		if (delim.empty())
		{
			for (size_t i = 0; i < s.size(); ++i)
				array_set(out, std::to_string(i), make_string(std::string(1, s[i])));
			return true;
		}
		size_t idx = 0;
		size_t pos = 0;
		while (true)
		{
			size_t next = s.find(delim, pos);
			std::string chunk = (next == std::string::npos) ? s.substr(pos) : s.substr(pos, next - pos);
			array_set(out, std::to_string(idx++), make_string(chunk));
			if (next == std::string::npos)
				break;
			pos = next + delim.size();
		}
		return true;
	});

	interp->register_function("glyphs", "s:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "glyphs expects (string)";
			return true;
		}
		const std::string& s = value_to_string(positional[0]);
		out.type = UdonValue::Type::Array;
		out.array_map = interp->allocate_array();
		size_t idx = 0;
		for (size_t i = 0; i < s.size();)
		{
			unsigned char c = static_cast<unsigned char>(s[i]);
			size_t len = 1;
			if ((c & 0x80) == 0)
				len = 1;
			else if ((c & 0xE0) == 0xC0)
				len = 2;
			else if ((c & 0xF0) == 0xE0)
				len = 3;
			else if ((c & 0xF8) == 0xF0)
				len = 4;
			if (i + len > s.size())
				len = 1;
			std::string glyph = s.substr(i, len);
			array_set(out, std::to_string(idx++), make_string(glyph));
			i += len;
		}
		return true;
	});

	interp->register_function("join", "arr:array, delim:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2 || positional[0].type != UdonValue::Type::Array)
		{
			err.has_error = true;
			err.opt_error_message = "join expects (array, delim)";
			return true;
		}
		std::string delim = value_to_string(positional[1]);
		std::vector<std::pair<int, std::string>> elems;
		array_foreach(positional[0], [&](const std::string& k, const UdonValue& v)
		{
			int idx = 0;
			try
			{
				idx = std::stoll(k);
			}
			catch (...)
			{
				return true;
			}
			elems.emplace_back(idx, value_to_string(v));
			return true;
		});
		std::sort(elems.begin(), elems.end(), [](auto& a, auto& b)
		{ return a.first < b.first; });
		std::ostringstream ss;
		for (size_t i = 0; i < elems.size(); ++i)
		{
			if (i)
				ss << delim;
			ss << elems[i].second;
		}
		out = make_string(ss.str());
		return true;
	});

	interp->register_function("concat", "parts:any...", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		std::ostringstream ss;
		for (const auto& v : positional)
			ss << value_to_string(v);
		out = make_string(ss.str());
		return true;
	});

	interp->register_function("chr", "code:int", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "chr expects (code)";
			return true;
		}
		int code = static_cast<int>(as_number(positional[0]));
		if (code < 0 || code > 255)
			code = code & 0xFF;
		std::string s(1, static_cast<char>(code));
		out = make_string(s);
		return true;
	});

	unary("sqrt", std::sqrt);
	unary("abs", std::fabs);
	unary("sin", std::sin);
	unary("cos", std::cos);
	unary("tan", std::tan);
	unary("asin", std::asin);
	unary("acos", std::acos);
	unary("atan", std::atan);
	unary("floor", std::floor);
	unary("ceil", std::ceil);
	unary("round", std::round);
	unary("exp", std::exp);
	unary("log", std::log);
	unary("log10", std::log10);

	binary("pow", std::pow);
	binary("atan2", std::atan2);
	binary("min", [](double a, double b)
	{ return a < b ? a : b; });
	binary("max", [](double a, double b)
	{ return a > b ? a : b; });

	auto binary_int = [interp](const std::string& name, s64 (*fn)(s64, s64))
	{
		interp->register_function(name, "a:int, b:int", "int", [fn, name](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
		{
			if (positional.size() != 2 || !is_integer_type(positional[0]) || !is_integer_type(positional[1]))
			{
				err.has_error = true;
				err.opt_error_message = name + " expects 2 integer arguments";
				return true;
			}
			out = make_int(fn(positional[0].int_value, positional[1].int_value));
			return true;
		});
	};

	binary_int("bit_and", [](s64 a, s64 b)
	{ return a & b; });
	binary_int("bit_or", [](s64 a, s64 b)
	{ return a | b; });
	binary_int("bit_xor", [](s64 a, s64 b)
	{ return a ^ b; });
	interp->register_function("bit_not", "x:int", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || !is_integer_type(positional[0]))
		{
			err.has_error = true;
			err.opt_error_message = "bit_not expects 1 integer argument";
			return true;
		}
		out = make_int(~positional[0].int_value);
		return true;
	});
	binary_int("bit_shl", [](s64 a, s64 b)
	{ return a << b; });
	binary_int("bit_shr", [](s64 a, s64 b)
	{ return a >> b; });

	interp->register_function("crc32", "data:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "crc32 expects (string)";
			return true;
		}
		std::ostringstream ss;
		ss << std::hex << std::setfill('0') << std::setw(8) << crc32(positional[0].string_value);
		out = make_string(ss.str());
		return true;
	});

	interp->register_function("md5", "data:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "md5 expects (string)";
			return true;
		}
		out = make_string(md5(positional[0].string_value));
		return true;
	});

	interp->register_function("sha1", "data:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "sha1 expects (string)";
			return true;
		}
		out = make_string(sha1(positional[0].string_value));
		return true;
	});

	interp->register_function("to_base", "value:number, digits:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2 || positional[1].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "to_base expects (number, digits_string)";
			return true;
		}
		std::string digits = positional[1].string_value;
		const size_t base = digits.size();
		if (base < 2)
		{
			err.has_error = true;
			err.opt_error_message = "to_base requires at least 2 digits";
			return true;
		}
		s64 v = static_cast<s64>(as_number(positional[0]));
		bool neg = v < 0;
		if (neg)
			v = -v;
		if (v == 0)
		{
			std::string s(1, digits[0]);
			if (neg)
				s = "-" + s;
			out = make_string(s);
			return true;
		}
		std::string result;
		while (v > 0)
		{
			size_t idx = static_cast<size_t>(v % base);
			result.push_back(digits[idx]);
			v /= static_cast<s64>(base);
		}
		if (neg)
			result.push_back('-');
		std::reverse(result.begin(), result.end());
		out = make_string(result);
		return true;
	});

	interp->register_function("from_base", "value:string, digits:string", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2 || positional[0].type != UdonValue::Type::String || positional[1].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "from_base expects (string, digits_string)";
			return true;
		}
		std::string s = positional[0].string_value;
		std::string digits = positional[1].string_value;
		const size_t base = digits.size();
		if (base < 2)
		{
			err.has_error = true;
			err.opt_error_message = "from_base requires at least 2 digits";
			return true;
		}
		std::unordered_map<char, int> val;
		for (size_t i = 0; i < base; ++i)
			val[digits[i]] = static_cast<int>(i);
		bool neg = false;
		size_t pos = 0;
		if (!s.empty() && s[0] == '-')
		{
			neg = true;
			pos = 1;
		}
		s64 acc = 0;
		for (; pos < s.size(); ++pos)
		{
			auto it = val.find(s[pos]);
			if (it == val.end())
			{
				err.has_error = true;
				err.opt_error_message = "Invalid digit in from_base input";
				return true;
			}
			acc = acc * static_cast<s64>(base) + it->second;
		}
		if (neg)
			acc = -acc;
		out = make_int(acc);
		return true;
	});

	interp->register_function("length", "UdonValue:any", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "len expects 1 argument";
			return true;
		}
		const auto& v = positional[0];
		if (v.type == UdonValue::Type::String)
			out = make_int(static_cast<s64>(v.string_value.size()));
		else if (v.type == UdonValue::Type::Array && v.array_map)
			out = make_int(static_cast<s64>(array_length(v)));
		else
			out = make_int(0);
		return true;
	});
	register_alias("len", "length");

	interp->register_function("$html", "template:string", "function", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "$html expects a single string template";
			return true;
		}

		const std::string tmpl = positional[0].string_value;
		auto* fn_obj = interp->allocate_function();
		fn_obj->template_body = tmpl;
		fn_obj->native_handler = [tmpl](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>& named, UdonValue& out, CodeLocation& inner_err) -> bool
		{
			std::unordered_map<std::string, UdonValue> replacements;
			if (!positional.empty() && positional[0].type == UdonValue::Type::Array && positional[0].array_map)
			{
				array_foreach(positional[0], [&](const std::string& k, const UdonValue& v)
				{
					replacements[k] = v;
					return true;
				});
			}
			for (const auto& kv : named)
				replacements[kv.first] = kv.second;

			std::string rendered;
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
					rendered.append(value_to_string(it->second));
				pos = end + 1;
			}
			out = make_string(rendered);
			(void)inner_err;
			return true;
		};

		out.type = UdonValue::Type::Function;
		out.function = fn_obj;
		return true;
	});

	interp->register_function("$jsx", "template:string, components:any?, options:any?", "function", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty() || positional.size() > 3 || positional[0].type != UdonValue::Type::String)
		{
			err.has_error = true;
			err.opt_error_message = "$jsx expects (template, [components], [options])";
			return true;
		}

		std::string parse_err;
		auto tmpl = jsx_compile(positional[0].string_value, parse_err);
		if (!tmpl)
		{
			err.has_error = true;
			err.opt_error_message = "$jsx parse error: " + parse_err;
			return true;
		}

		auto convert_map = [](const UdonValue& v) -> std::unordered_map<std::string, UdonValue>
		{
			if (v.type != UdonValue::Type::Array || !v.array_map)
				return {};
			std::unordered_map<std::string, UdonValue> out;
			array_foreach(v, [&](const std::string& k, const UdonValue& val)
			{
				out[k] = val;
				return true;
			});
			return out;
		};

		std::unordered_map<std::string, UdonValue> components = (positional.size() >= 2) ? convert_map(positional[1]) : std::unordered_map<std::string, UdonValue>{};
		std::unordered_map<std::string, UdonValue> options = (positional.size() >= 3) ? convert_map(positional[2]) : std::unordered_map<std::string, UdonValue>{};

		struct JsxClosureData
		{
			std::shared_ptr<JsxTemplate> tmpl;
			std::unordered_map<std::string, UdonValue> components;
			std::unordered_map<std::string, UdonValue> options;
		};

		auto data = std::make_shared<JsxClosureData>();
		data->tmpl = tmpl;
		data->components = components;
		data->options = options;

		auto* fn_obj = interp->allocate_function();
		fn_obj->template_body = positional[0].string_value;
		fn_obj->user_data = data;
		for (const auto& kv : components)
			fn_obj->rooted_values.push_back(kv.second);
		for (const auto& kv : options)
			fn_obj->rooted_values.push_back(kv.second);
		fn_obj->native_handler = [data](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>& named, UdonValue& out, CodeLocation& inner_err)
		{
			std::unordered_map<std::string, UdonValue> props;
			if (!positional.empty() && positional[0].type == UdonValue::Type::Array && positional[0].array_map)
			{
				array_foreach(positional[0], [&](const std::string& k, const UdonValue& val)
				{
					props[k] = val;
					return true;
				});
			}
			for (const auto& kv : named)
				props[kv.first] = kv.second;

			CodeLocation render_err{};
			std::string rendered = jsx_render(*data->tmpl, props, data->components, data->options, interp, render_err);
			if (render_err.has_error)
			{
				inner_err = render_err;
				return true;
			}
			out = make_string(rendered);
			return true;
		};

		out.type = UdonValue::Type::Function;
		out.function = fn_obj;
		return true;
	});

	interp->register_function("substr", "s:string, start:int, count:int", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() < 2 || positional.size() > 3)
		{
			err.has_error = true;
			err.opt_error_message = "substr expects (string, start, [count])";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		const s64 str_len = static_cast<s64>(s.size());
		s64 start = static_cast<s64>(as_number(positional[1]));

		if (start < 0)
		{
			start = str_len + start;
			if (start < 0)
			{
				out = make_string("");
				return true;
			}
		}

		if (start >= str_len)
		{
			out = make_string("");
			return true;
		}

		if (positional.size() == 3)
		{
			s64 length = static_cast<s64>(as_number(positional[2]));

			if (length < 0)
			{
				length = str_len - start + length;
				if (length < 0)
				{
					out = make_string("");
					return true;
				}
			}

			if (start + length > str_len)
			{
				length = str_len - start;
			}

			out = make_string(s.substr(static_cast<size_t>(start), static_cast<size_t>(length)));
		}
		else
		{
			out = make_string(s.substr(static_cast<size_t>(start)));
		}

		return true;
	});

	interp->register_function("replace", "s:string, old:string, new:string, count:int", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() < 3 || positional.size() > 4)
		{
			err.has_error = true;
			err.opt_error_message = "replace expects (string, old, new, [count])";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::string from = value_to_string(positional[1]);
		std::string to = value_to_string(positional[2]);
		int count = -1;
		if (positional.size() == 4)
			count = static_cast<int>(as_number(positional[3]));
		if (from.empty())
		{
			out = make_string(s);
			return true;
		}
		size_t pos = 0;
		int replaced = 0;
		while ((count < 0 || replaced < count))
		{
			pos = s.find(from, pos);
			if (pos == std::string::npos)
				break;
			s.replace(pos, from.size(), to);
			pos += to.size();
			++replaced;
		}
		out = make_string(s);
		return true;
	});

	interp->register_function("starts_with", "s:string, prefix:string", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2)
		{
			err.has_error = true;
			err.opt_error_message = "starts_with expects (string, prefix)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::string pref = value_to_string(positional[1]);
		bool res = s.size() >= pref.size() && s.compare(0, pref.size(), pref) == 0;
		out = make_bool(res);
		return true;
	});

	interp->register_function("ends_with", "s:string, suffix:string", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2)
		{
			err.has_error = true;
			err.opt_error_message = "ends_with expects (string, suffix)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::string suf = value_to_string(positional[1]);
		bool res = s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		out = make_bool(res);
		return true;
	});

	interp->register_function("find", "s:string, needle:string, start:int", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() < 2 || positional.size() > 3)
		{
			err.has_error = true;
			err.opt_error_message = "find expects (string, needle, [start])";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::string needle = value_to_string(positional[1]);
		size_t start = 0;
		if (positional.size() == 3)
		{
			const s64 st = static_cast<s64>(as_number(positional[2]));
			if (st > 0)
				start = static_cast<size_t>(st);
		}
		size_t pos = s.find(needle, start);
		if (pos == std::string::npos)
			out = make_int(-1);
		else
			out = make_int(static_cast<s64>(pos));
		return true;
	});

	interp->register_function("ord", "s:string", "int", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "ord expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		if (s.empty())
		{
			out = make_int(0);
			return true;
		}
		out = make_int(static_cast<s64>(static_cast<unsigned char>(s[0])));
		return true;
	});

	interp->register_function("contains", "hay:any, needle:any", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2)
		{
			err.has_error = true;
			err.opt_error_message = "contains expects (haystack, needle)";
			return true;
		}
		const auto& hay = positional[0];
		const auto& needle = positional[1];
		bool found = false;
		if (hay.type == UdonValue::Type::String)
		{
			found = value_to_string(hay).find(value_to_string(needle)) != std::string::npos;
		}
		else if (hay.type == UdonValue::Type::Array && hay.array_map)
		{
			array_foreach(hay, [&](const std::string&, const UdonValue& val)
			{
				UdonValue tmp;
				if (equal_values(val, needle, tmp) && tmp.int_value)
				{
					found = true;
					return false;
				}
				return true;
			});
		}
		out = make_bool(found);
		return true;
	});

	interp->register_function("to_upper", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_upper expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
		{ return std::toupper(c); });
		out = make_string(s);
		return true;
	});

	interp->register_function("to_lower", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_lower expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
		{ return std::tolower(c); });
		out = make_string(s);
		return true;
	});

	interp->register_function("trim", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "trim expects (string)";
			return true;
		}
		std::string s = value_to_string(positional[0]);
		out = make_string(trim_string(s, true, true));
		return true;
	});

	auto to_int_fn = [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_int expects 1 argument";
			return true;
		}
		if (is_numeric(positional[0]))
			out = make_int(static_cast<s64>(as_number(positional[0])));
		else if (positional[0].type == UdonValue::Type::String)
		{
			double d;
			bool is_int;
			if (parse_number_string(positional[0].string_value, d, is_int))
				out = make_int(static_cast<s64>(d));
			else
				out = make_int(0);
		}
		else
			out = make_int(0);
		return true;
	};
	interp->register_function("to_int", "value:any", "int", to_int_fn);

	auto to_float_fn = [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_float expects 1 argument";
			return true;
		}
		if (is_numeric(positional[0]))
			out = make_float(static_cast<f64>(as_number(positional[0])));
		else if (positional[0].type == UdonValue::Type::String)
		{
			double d;
			bool is_int;
			if (parse_number_string(positional[0].string_value, d, is_int))
				out = make_float(static_cast<f64>(d));
			else
				out = make_float(0.0);
		}
		else
			out = make_float(0.0);
		return true;
	};
	interp->register_function("to_float", "value:any", "float", to_float_fn);

	interp->register_function("to_string", "UdonValue:any", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_string expects 1 argument";
			return true;
		}
		out = make_string(value_to_string(positional[0]));
		return true;
	});

	interp->register_function("to_bool", "UdonValue:any", "bool", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_bool expects 1 argument";
			return true;
		}
		if (positional[0].type == UdonValue::Type::String)
		{
			bool b;
			if (parse_bool_string(positional[0].string_value, b))
				out = make_bool(b);
			else
				out = make_bool(is_truthy(positional[0]));
		}
		else
			out = make_bool(is_truthy(positional[0]));
		return true;
	});

	interp->register_function("typeof", "UdonValue:any", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "typeof expects 1 argument";
			return true;
		}
		out = make_string(value_type_name(positional[0]));
		return true;
	});

	interp->register_function("range", "start:int, stop:int, step:int", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty() || positional.size() > 3)
		{
			err.has_error = true;
			err.opt_error_message = "range expects (stop) or (start, stop, [step])";
			return true;
		}
		s64 start = 0;
		s64 stop = 0;
		s64 step = 1;
		if (positional.size() == 1)
		{
			stop = positional[0].int_value;
		}
		else
		{
			start = positional[0].int_value;
			stop = positional[1].int_value;
			if (positional.size() == 3)
				step = positional[2].int_value;
		}
		if (step == 0)
			step = 1;
		out.type = UdonValue::Type::Array;
		out.array_map = interp->allocate_array();
		s64 idx = 0;
		if (step > 0)
		{
			for (s64 v = start; v < stop; v += step)
				array_set(out, std::to_string(idx++), make_int(v));
		}
		else
		{
			for (s64 v = start; v > stop; v += step)
				array_set(out, std::to_string(idx++), make_int(v));
		}
		return true;
	});

	static std::mt19937 rng(static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count()));
	interp->register_function("rand", "", "float", [](UdonInterpreter*, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		std::uniform_real_distribution<double> dist(0.0, 1.0);
		out = make_float(dist(rng));
		return true;
	});

	interp->register_function("push", "arr:array, UdonValue:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2 || positional[0].type != UdonValue::Type::Array)
		{
			err.has_error = true;
			err.opt_error_message = "push expects (array, UdonValue)";
			return true;
		}
		int idx = static_cast<int>(array_length(positional[0]));
		array_set(const_cast<UdonValue&>(positional[0]), std::to_string(idx), positional[1]);
		out = make_none();
		return true;
	});

	interp->register_function("pop", "arr:array, key:any", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.empty() || positional[0].type != UdonValue::Type::Array)
		{
			err.has_error = true;
			err.opt_error_message = "pop expects (array, [key])";
			return true;
		}
		UdonValue arr = positional[0];
		std::string key;
		if (positional.size() >= 2)
			key = key_from_value(positional[1]);
		else
		{
			s64 max_idx = -1;
			array_foreach(arr, [&](const std::string& k, const UdonValue&)
			{
				try
				{
					s64 parsed = std::stoll(k);
					if (parsed > max_idx)
						max_idx = parsed;
				}
				catch (...)
				{
				}
				return true;
			});
			if (max_idx >= 0)
				key = std::to_string(max_idx);
		}
		if (key.empty())
		{
			out = make_none();
			return true;
		}
		if (!array_delete(arr, key, &out))
			out = make_none();
		return true;
	});

	interp->register_function("delete", "arr:array, key:any", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2 || positional[0].type != UdonValue::Type::Array)
		{
			err.has_error = true;
			err.opt_error_message = "delete expects (array, key)";
			return true;
		}
		UdonValue arr = positional[0];
		std::string key = key_from_value(positional[1]);
		if (!array_delete(arr, key, &out))
			out = make_none();
		return true;
	});

	interp->register_function("shift", "arr:array", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1 || positional[0].type != UdonValue::Type::Array)
		{
			err.has_error = true;
			err.opt_error_message = "shift expects (array)";
			return true;
		}
		UdonValue arr = positional[0];
		std::vector<s64> indices;
		array_foreach(arr, [&](const std::string& k, const UdonValue&)
		{
			try
			{
				indices.push_back(std::stoll(k));
			}
			catch (...)
			{
			}
			return true;
		});
		if (indices.empty())
		{
			out = make_none();
			return true;
		}
		std::sort(indices.begin(), indices.end());
		std::string first_key = std::to_string(indices.front());
		if (!array_delete(arr, first_key, &out))
			out = make_none();

		std::vector<std::pair<std::string, UdonValue>> rebuild;
		array_foreach(arr, [&](const std::string& k, const UdonValue& v)
		{
			try
			{
				(void)std::stoll(k);
				return true;
			}
			catch (...)
			{
			}
			rebuild.push_back({ k, v });
			return true;
		});
		for (size_t i = 1; i < indices.size(); ++i)
		{
			std::string orig = std::to_string(indices[i]);
			UdonValue val;
			if (array_get(arr, orig, val))
				rebuild.push_back({ std::to_string(i - 1), val });
		}
		array_clear(arr);
		for (const auto& kv : rebuild)
			array_set(arr, kv.first, kv.second);
		return true;
	});

	interp->register_function("unshift", "arr:array, UdonValue:any", "none", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 2 || positional[0].type != UdonValue::Type::Array)
		{
			err.has_error = true;
			err.opt_error_message = "unshift expects (array, UdonValue)";
			return true;
		}
		UdonValue arr = positional[0];
		std::vector<s64> indices;
		array_foreach(arr, [&](const std::string& k, const UdonValue&)
		{
			try
			{
				indices.push_back(std::stoll(k));
			}
			catch (...)
			{
			}
			return true;
		});
		std::sort(indices.begin(), indices.end());

		std::vector<std::pair<std::string, UdonValue>> rebuild;
		array_foreach(arr, [&](const std::string& k, const UdonValue& v)
		{
			try
			{
				(void)std::stoll(k);
				return true;
			}
			catch (...)
			{
			}
			rebuild.push_back({ k, v });
			return true;
		});
		rebuild.push_back({ "0", positional[1] });
		for (size_t i = 0; i < indices.size(); ++i)
		{
			std::string orig = std::to_string(indices[i]);
			UdonValue val;
			if (array_get(arr, orig, val))
				rebuild.push_back({ std::to_string(i + 1), val });
		}
		array_clear(arr);
		for (const auto& kv : rebuild)
			array_set(arr, kv.first, kv.second);
		out = make_none();
		return true;
	});

	interp->register_function("time", "", "int", [](UdonInterpreter*, const std::vector<UdonValue>&, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation&)
	{
		using namespace std::chrono;
		auto now = system_clock::now();
		auto secs = duration_cast<seconds>(now.time_since_epoch()).count();
		out = make_int(static_cast<s64>(secs));
		return true;
	});

	interp->register_function("to_json", "UdonValue:any", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_json expects (UdonValue)";
			return true;
		}
		out = make_string(to_json(positional[0]));
		return true;
	});

	interp->register_function("from_json", "s:string", "any", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "from_json expects (string)";
			return true;
		}
		JsonParser parser(value_to_string(positional[0]));
		if (!parser.parse_value(out))
		{
			err.has_error = true;
			err.opt_error_message = "Failed to parse JSON";
		}
		return true;
	});
	interp->register_function("to_uri", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_uri expects (string)";
			return true;
		}
		out = make_string(url_encode(value_to_string(positional[0])));
		return true;
	});

	interp->register_function("from_uri", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "from_uri expects (string)";
			return true;
		}
		out = make_string(url_decode(value_to_string(positional[0])));
		return true;
	});

	interp->register_function("to_base64", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "to_base64 expects (string)";
			return true;
		}
		out = make_string(to_base64_impl(value_to_string(positional[0])));
		return true;
	});

	interp->register_function("from_base64", "s:string", "string", [](UdonInterpreter*, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "from_base64 expects (string)";
			return true;
		}
		out = make_string(from_base64_impl(value_to_string(positional[0])));
		return true;
	});

	interp->register_function("parse_formdata", "s:string", "array", [](UdonInterpreter* interp, const std::vector<UdonValue>& positional, const std::unordered_map<std::string, UdonValue>&, UdonValue& out, CodeLocation& err)
	{
		if (positional.size() != 1)
		{
			err.has_error = true;
			err.opt_error_message = "parse_formdata expects (string)";
			return true;
		}
		out = parse_form_data(value_to_string(positional[0]), interp);
		return true;
	});
}
