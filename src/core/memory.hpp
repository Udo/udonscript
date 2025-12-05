#pragma once

#include "types.h"
#include <string>

struct Arena
{
	struct Allocation
	{
		void* ptr;
		u64 size;
	};

	u64 capacity;
	u64 offset;
	void* data;
	std::string name;
	u64 generation;
	std::vector<Allocation> overflow_allocations;
	std::vector<Allocation> free_list;
	bool enable_free_list = true;
	bool allow_free = true;
	u64 used() const;

	Arena(u64 size, std::string name = "unnamed_arena");
	~Arena();
	void reset();
	void* alloc(u64 size, const char* dbg_name = "<data>");
	template <typename T, typename... Args>
	T* alloc(Args&&... args)
	{
		void* mem = alloc(sizeof(T), typeid(T).name());
		if (!mem)
			return nullptr;
		if constexpr (sizeof...(Args) == 0)
			return new (mem) T(); // value-initialize members
		else
			return new (mem) T(std::forward<Args>(args)...);
	}

	void free(void* ptr);
	bool contains(const void* ptr) const;
	bool owns(const void* ptr) const;
};
