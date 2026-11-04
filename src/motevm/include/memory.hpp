#pragma once
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include "define.hpp"

// The 4-byte accessors are small enough that the cold slow-path call should
// never inhibit inlining; the interpreter's load/store cases depend on it.
#if defined(_MSC_VER)
#define MOTEVM_MEM_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define MOTEVM_MEM_INLINE inline __attribute__((always_inline))
#else
#define MOTEVM_MEM_INLINE inline
#endif

namespace MEMORY
{
    // memory constants
    const size_t BLOCK_SIZE = 65536;  // 64KiB
    const size_t BLOCK_COUNT = 65536; // 64KiB * 65536 = 4GiB
    const qword ADDRESS_SPACE = static_cast<qword>(BLOCK_COUNT) * BLOCK_SIZE;
    // memory block status
    const int RELEASED = 0;
    const int OCCUPIED = 1;
    const int FREE = 2;

    const int OK = 0;
    const int BLOCK_OCCUPIED = 1;
    const int MEMORY_UNAVAILABLE = 2;
    const int MEMORY_OVERFLOW = 3;
}

struct MemoryBlock
{
    std::shared_ptr<std::array<byte, MEMORY::BLOCK_SIZE>> pointer;
    int status;
    MemoryBlock() : pointer(nullptr), status(0) {}
    ~MemoryBlock()
    {
        pointer.reset();
    }
};

// Memory: heap control for motevm
class Memory
{
private:
    std::array<MemoryBlock, MEMORY::BLOCK_COUNT> blocks;
    // Raw block bases for the hot path. Non-null exactly while the block is
    // allocated; maintained by allocate()/free(). Never dereference without a
    // null check: a null entry means the cold path decides the exact fault.
    std::array<byte *, MEMORY::BLOCK_COUNT> bases{};

private:
    int loword(int value) { return value & 0xFFFF; }
    int hiword(int value) { return (value >> 16) & 0xFFFF; }
    int getBlockStatus(int index);
    bool overflow(dword offset, dword size);
    // write and read data to/from a single block, unsafe(check in host functions)
    int writeData(std::span<const byte> buffer, int offset, int index);
    int readData(std::span<byte> buffer, int offset, int index);
    int readSlow(dword &value, qword address);
    int writeSlow(dword value, qword address);

public: // only for construction and destruction
    Memory() {}
    ~Memory() {}
    // `bases` caches raw pointers into the blocks; copying would create two
    // owners whose allocate()/free() can invalidate each other's cache.
    Memory(const Memory &) = delete;
    Memory &operator=(const Memory &) = delete;
    Memory(Memory &&) = delete;
    Memory &operator=(Memory &&) = delete;

public: // other public methods
    int allocate(int index);
    void free(int index);
    std::size_t allocatedBlocks(void) const; // blocks with status != RELEASED

    // Single 4-byte access with a 64-bit address computation, so the 32-bit
    // base+displacement pair cannot wrap and alias low memory. The common
    // same-block path is one table load plus one memcpy; everything else
    // (unallocated block, cross-block access, out-of-range) goes cold.
    MOTEVM_MEM_INLINE int readAt(dword &value, dword base, dword displacement)
    {
        const qword address = static_cast<qword>(base) + displacement;
        if (address > MEMORY::ADDRESS_SPACE - sizeof(dword))
            return MEMORY::MEMORY_OVERFLOW;
        const dword block = static_cast<dword>(address >> 16);
        const dword offset = static_cast<dword>(address & 0xFFFFu);
        if (offset + sizeof(dword) <= MEMORY::BLOCK_SIZE)
        {
            byte *p = bases[block];
            if (p != nullptr)
            {
                std::memcpy(&value, p + offset, sizeof(dword));
                return MEMORY::OK;
            }
        }
        return readSlow(value, address);
    }

    MOTEVM_MEM_INLINE int writeAt(dword value, dword base, dword displacement)
    {
        const qword address = static_cast<qword>(base) + displacement;
        if (address > MEMORY::ADDRESS_SPACE - sizeof(dword))
            return MEMORY::MEMORY_OVERFLOW;
        const dword block = static_cast<dword>(address >> 16);
        const dword offset = static_cast<dword>(address & 0xFFFFu);
        if (offset + sizeof(dword) <= MEMORY::BLOCK_SIZE)
        {
            byte *p = bases[block];
            if (p != nullptr)
            {
                std::memcpy(p + offset, &value, sizeof(dword));
                return MEMORY::OK;
            }
        }
        return writeSlow(value, address);
    }

    // 32-bit read and write kept for host/API callers; both share the fast path.
    int write(const dword &buffer, dword offset);
    int read(dword &buffer, dword offset);
    int writes(std::span<const byte> buffer, dword offset, dword size);
    int reads(std::span<byte> buffer, dword offset, dword size);
};