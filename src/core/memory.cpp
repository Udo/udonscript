#include "memory.hpp"
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>
#include <cstring>
#include <typeinfo>
#include <string>
#include <vector>

#define mfree free

Arena::Arena(u64 size, std::string name)
{
	capacity = size;
	offset = 0;
	this->name = name;
	data = malloc(size);
	generation = 0;
}

Arena::~Arena()
{
	allow_free = false;
	reset();
	mfree(data);
	data = nullptr;
	capacity = 0;
	offset = 0;
}

void Arena::reset()
{
	offset = 0;
	generation++;
	for (const Allocation& alloc : overflow_allocations)
	{
		mfree(alloc.ptr);
	}
	overflow_allocations.clear();
	free_list.clear();
}

u64 next_power_of_two(u64 n)
{
	if (n == 0)
		return 1;
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	n |= n >> 32;
	return n + 1;
}

Arena::Allocation find_in_free_list(std::vector<Arena::Allocation>& free_list, u64 size)
{
	for (auto it = free_list.begin(); it != free_list.end(); ++it)
	{
		if (it->size >= size)
		{
			Arena::Allocation allocation = *it;
			free_list.erase(it);
			return allocation;
		}
	}
	return { nullptr, 0 };
}

static inline void write_allocation_header(void* header_ptr, u64 size)
{
	if (!header_ptr)
		return;
	*reinterpret_cast<u64*>(header_ptr) = size;
}

void* Arena::alloc(u64 size, const char* dbg_name)
{
	size = next_power_of_two(size);
	if (dbg_name)
	{
		f64 free_space = 100.0 * ((f64)capacity - (f64)offset) / (f64)capacity;
	}
	const u64 size_with_header = sizeof(u64) + size;
	if (offset + size_with_header > capacity)
	{
		void* ptr = malloc(size);
		if (!ptr)
			return nullptr;
		overflow_allocations.push_back({ ptr, size });
		return ptr;
	}
	if (enable_free_list)
	{
		Arena::Allocation free_block = find_in_free_list(free_list, size);
		if (free_block.ptr)
		{
			void* header_ptr = static_cast<char*>(free_block.ptr) - sizeof(u64);
			write_allocation_header(header_ptr, free_block.size);
			return free_block.ptr;
		}
	}
	void* ptr = static_cast<char*>(data) + offset;
	write_allocation_header(ptr, size);
	void* user_ptr = static_cast<char*>(ptr) + sizeof(u64);
	offset += size_with_header + (8 - (size_with_header % 8)) % 8;
	return user_ptr;
}

u64 Arena::used() const
{
	u64 overflow_size = 0;
	for (const Allocation& alloc : overflow_allocations)
	{
		overflow_size += alloc.size;
	}
	return offset + overflow_size;
}

void Arena::free(void* ptr)
{
	if (!ptr)
		return;
	if (!allow_free)
		return;
	if (data == nullptr && overflow_allocations.empty())
		return;
	if (enable_free_list)
	{
		if (contains(ptr))
		{
			u64* size_ptr = static_cast<u64*>(static_cast<void*>(
				static_cast<char*>(ptr) - sizeof(u64)));
			u64 size = *size_ptr;
			free_list.push_back({ ptr, size });
			return;
		}
	}
	if (overflow_allocations.empty())
		return;
	for (auto it = overflow_allocations.begin(); it != overflow_allocations.end(); ++it)
	{
		if (it->ptr == ptr)
		{
			mfree(ptr);
			overflow_allocations.erase(it);
			return;
		}
	}
}

bool Arena::contains(const void* ptr) const
{
	if (data == nullptr)
		return false;
	const char* cdata = static_cast<const char*>(data);
	const char* cptr = static_cast<const char*>(ptr);
	return cptr >= cdata && cptr < (cdata + capacity);
}

bool Arena::owns(const void* ptr) const
{
	if (contains(ptr))
		return true;
	for (const Allocation& alloc : overflow_allocations)
	{
		if (alloc.ptr == ptr)
			return true;
	}
	return false;
}
