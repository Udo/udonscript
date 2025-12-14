#pragma once

#include "types.h"
#include <string>
#include <memory>
#include <typeinfo>

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

template <typename T>
struct TypedArena
{
	using value_type = T;
	Arena* arena = nullptr;

	TypedArena() = default;
	explicit TypedArena(Arena* a) : arena(a) {}

	template <class U>
	TypedArena(const TypedArena<U>& other) noexcept : arena(other.arena) {}

	T* allocate(std::size_t n)
	{
		if (!arena)
			throw std::bad_alloc();
		void* p = arena->alloc(static_cast<u64>(n * sizeof(T)));
		if (!p)
			throw std::bad_alloc();
		return static_cast<T*>(p);
	}

	void deallocate(T*, std::size_t) noexcept {}
};

struct ArenaResetGuard
{
	Arena& arena;
	explicit ArenaResetGuard(Arena& a) : arena(a)
	{
		arena.reset();
	}
	~ArenaResetGuard()
	{
		arena.reset();
	}
};
