#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <utility>

namespace Memory {

// Linear Allocator (Bump Allocator)
// Very fast allocation, O(1), no deallocation (reset all at once).
// Ideal for temporary per-frame allocations (e.g., render commands, physics contacts).
class LinearAllocator {
public:
    LinearAllocator(size_t size_bytes) : total_size(size_bytes), offset(0) {
        start_ptr = new uint8_t[size_bytes];
    }
    
    ~LinearAllocator() {
        delete[] start_ptr;
    }

    void* allocate(size_t size, size_t alignment = sizeof(void*)) {
        size_t adjustment = get_adjustment(offset, alignment);
        
        if (offset + adjustment + size > total_size) {
            throw std::bad_alloc();
        }
        
        uintptr_t aligned_address = reinterpret_cast<uintptr_t>(start_ptr) + offset + adjustment;
        offset += adjustment + size;
        
        return reinterpret_cast<void*>(aligned_address);
    }
    
    template<typename T, typename... Args>
    T* allocate_object(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    void reset() {
        offset = 0;
    }

    size_t get_used_memory() const { return offset; }
    size_t get_total_memory() const { return total_size; }

private:
    uint8_t* start_ptr;
    size_t total_size;
    size_t offset;
    
    inline size_t get_adjustment(size_t current_offset, size_t alignment) {
        size_t adjustment = alignment - (current_offset & (alignment - 1));
        return adjustment == alignment ? 0 : adjustment;
    }
};

// Pool Allocator
// Fast O(1) allocation and deallocation.
// Perfect for fixed-size objects like ECS components or Task objects.
class PoolAllocator {
public:
    PoolAllocator(size_t object_size, size_t object_count) 
        : object_size(object_size), total_size(object_size * object_count), free_list(nullptr) {
        
        // Ensure object_size is at least the size of a pointer
        if (this->object_size < sizeof(void*)) {
            this->object_size = sizeof(void*);
        }
        
        start_ptr = new uint8_t[total_size];
        
        // Initialize free list
        free_list = reinterpret_cast<void**>(start_ptr);
        void** current = free_list;
        
        for (size_t i = 0; i < object_count - 1; ++i) {
            uintptr_t next_address = reinterpret_cast<uintptr_t>(current) + this->object_size;
            *current = reinterpret_cast<void*>(next_address);
            current = reinterpret_cast<void**>(*current);
        }
        *current = nullptr;
    }
    
    ~PoolAllocator() {
        delete[] start_ptr;
    }

    void* allocate() {
        if (free_list == nullptr) {
            throw std::bad_alloc(); // Pool is full
        }
        
        void* p = free_list;
        free_list = reinterpret_cast<void**>(*free_list);
        return p;
    }

    void deallocate(void* p) {
        if (p == nullptr) return;
        
        // Push to the front of the free list
        *(reinterpret_cast<void**>(p)) = free_list;
        free_list = reinterpret_cast<void**>(p);
    }
    
    template<typename T, typename... Args>
    T* allocate_object(Args&&... args) {
        assert(sizeof(T) <= object_size);
        void* mem = allocate();
        return new (mem) T(std::forward<Args>(args)...);
    }
    
    template<typename T>
    void deallocate_object(T* p) {
        if (p) {
            p->~T();
            deallocate(p);
        }
    }

private:
    uint8_t* start_ptr;
    size_t object_size;
    size_t total_size;
    void** free_list;
};

} // namespace Memory
