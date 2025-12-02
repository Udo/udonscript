#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdio>
#include <cstdarg>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef unsigned char u8;
typedef char s8;
typedef char b8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;
typedef float f32;
typedef unsigned long u64;
typedef long s64;
typedef double f64;
typedef std::string string;

struct Matrix4
{
	f32 m[16];
	Matrix4()
	{
		for (int i = 0; i < 16; i++)
			m[i] = 0;
	}
};

struct Quaternion
{
	f32 x, y, z, w;
	Quaternion() : x(0), y(0), z(0), w(1) {}
	Quaternion(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

enum AlignmentX
{
	left,
	center,
	right
};
enum AlignmentY
{
	top,
	middle,
	bottom
};

#define PI M_PI
#define TERM_RED "\x1B[1;31m"
#define TERM_GREEN "\x1B[1;32m"
#define TERM_BLUE "\x1B[94m"
#define TERM_YELLOW "\x1B[1;33m"
#define TERM_CYAN "\x1B[1;36m"
#define TERM_MAGENTA "\x1B[1;35m"
#define TERM_GRAY "\x1B[1;30m"
#define TERM_WHITE "\x1B[1;37m"
#define TERM_NORMAL "\x1B[0m"
#define TERM_RESET "\x1B[0m"

typedef std::function<void()> FunctionVoid;
typedef std::function<void(void*)> FunctionCallback;

extern u32 current_frame_number;

namespace logx
{
	enum class Level
	{
		Info,
		Warn,
		Error
	};

	inline const char* level_str(Level l)
	{
		switch (l)
		{
			case Level::Info:
				return TERM_GRAY "[I]";
			case Level::Warn:
				return TERM_MAGENTA "[W]";
			case Level::Error:
				return TERM_RED "[E]";
		}
		return "?";
	}

	inline void vlog(Level lvl, const char* fmt, va_list ap)
	{
		std::fprintf(lvl == Level::Error ? stderr : stdout, "%s F%6i ", level_str(lvl), current_frame_number);
		std::vfprintf(lvl == Level::Error ? stderr : stdout, fmt, ap);
		std::fprintf(lvl == Level::Error ? stderr : stdout, TERM_RESET "\n");
	}

	inline void log(Level lvl, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		vlog(lvl, fmt, ap);
		va_end(ap);
	}
}

#define LOG_INFO(...)                              \
	do                                             \
	{                                              \
		logx::log(logx::Level::Info, __VA_ARGS__); \
	} while (0)
#define LOG_WARN(...)                              \
	do                                             \
	{                                              \
		logx::log(logx::Level::Warn, __VA_ARGS__); \
	} while (0)
#define LOG_ERROR(...)                              \
	do                                              \
	{                                               \
		logx::log(logx::Level::Error, __VA_ARGS__); \
	} while (0)
