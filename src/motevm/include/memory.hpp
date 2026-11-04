#pragma once
#include <array>
#include <memory>
#include <span>
#include "define.hpp"

namespace MEMORY
{
    // memory constants
    const size_t BLOCK_SIZE = 65536;  // 64KiB
    const size_t BLOCK_COUNT = 65536; // 64KiB * 65536 = 4GiB
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

private:
    int loword(int value) { return value & 0xFFFF; }
    int hiword(int value) { return (value >> 16) & 0xFFFF; }
    int getBlockStatus(int index);
    bool overflow(dword offset, dword size);
    // write and read data to/from a single block, unsafe(check in host functions)
    int writeData(std::span<const byte> buffer, int offset, int index);
    int readData(std::span<byte> buffer, int offset, int index);

public: // only for construction and destruction
    Memory() {}
    ~Memory() {}

public: // other public methods
    int allocate(int index);
    void free(int index);
    std::size_t allocatedBlocks(void) const; // blocks with status != RELEASED
    // to attention, avoid using read and write across blocks
    // 32-bit read and write, used for reading and writing most data
    int write(const dword &buffer, dword offset);
    int read(dword &buffer, dword offset);
    int writes(std::span<const byte> buffer, dword offset, dword size);
    int reads(std::span<byte> buffer, dword offset, dword size);
};