#include <algorithm>
#include "memory.hpp"

bool Memory::overflow(dword offset, dword size)
{
    const qword end = static_cast<qword>(offset) + size;
    return end > MEMORY::ADDRESS_SPACE;
}

int Memory::getBlockStatus(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= MEMORY::BLOCK_COUNT)
        return MEMORY::RELEASED;
    return blocks[index].status;
}

int Memory::allocate(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= MEMORY::BLOCK_COUNT)
        return MEMORY::MEMORY_UNAVAILABLE;
    if (blocks[index].status == MEMORY::OCCUPIED)
        return MEMORY::BLOCK_OCCUPIED;
    if (blocks[index].status == MEMORY::FREE)
        return MEMORY::OK; // idempotent
    blocks[index].pointer = std::make_shared<std::array<byte, MEMORY::BLOCK_SIZE>>();
    blocks[index].status = MEMORY::FREE;
    bases[index] = blocks[index].pointer->data();
    return MEMORY::OK;
}

void Memory::free(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= MEMORY::BLOCK_COUNT)
        return;
    bases[index] = nullptr;
    blocks[index].pointer.reset();
    blocks[index].status = MEMORY::RELEASED;
}

std::size_t Memory::allocatedBlocks(void) const
{
    std::size_t count = 0;
    for (const MemoryBlock &block : blocks)
        if (block.status != MEMORY::RELEASED)
            ++count;
    return count;
}

// (internal implementation)to attention, read and write across blocks are not allowed.
int Memory::writeData(std::span<const byte> buffer, int offset, int index)
{
    int status = getBlockStatus(index);
    if (status == MEMORY::RELEASED)
        return MEMORY::MEMORY_UNAVAILABLE;
    else if (status == MEMORY::OCCUPIED)
        return MEMORY::BLOCK_OCCUPIED;
    std::copy(buffer.begin(), buffer.end(), blocks[index].pointer->begin() + offset);
    return MEMORY::OK;
}

int Memory::readData(std::span<byte> buffer, int offset, int index)
{
    int status = getBlockStatus(index);
    if (status == MEMORY::RELEASED)
        return MEMORY::MEMORY_UNAVAILABLE;
    else if (status == MEMORY::OCCUPIED)
        return MEMORY::BLOCK_OCCUPIED;
    std::copy(blocks[index].pointer->begin() + offset,
              blocks[index].pointer->begin() + offset + buffer.size(),
              buffer.begin());
    return MEMORY::OK;
}

int Memory::write(const dword &buffer, dword offset)
{
    return writeAt(buffer, offset, 0);
}

int Memory::read(dword &buffer, dword offset)
{
    return readAt(buffer, offset, 0);
}

int Memory::readSlow(dword &value, qword address)
{
    const dword valueSize = static_cast<dword>(sizeof(dword));
    if (address > MEMORY::ADDRESS_SPACE - valueSize)
        return MEMORY::MEMORY_OVERFLOW;

    const dword offset = static_cast<dword>(address);
    const int blockIndex = hiword(static_cast<int>(offset));
    const int blockOffset = loword(static_cast<int>(offset));
    byte *data = reinterpret_cast<byte *>(&value);

    if (static_cast<dword>(blockOffset) + valueSize <= MEMORY::BLOCK_SIZE)
        return readData(std::span<byte>(data, valueSize), blockOffset, blockIndex);

    // Handle the case where the value spans across two blocks
    const dword firstBlockSize = static_cast<dword>(MEMORY::BLOCK_SIZE) - static_cast<dword>(blockOffset);
    int result = readData(std::span<byte>(data, firstBlockSize), blockOffset, blockIndex);
    if (result != MEMORY::OK)
        return result;
    return readData(std::span<byte>(data + firstBlockSize, valueSize - firstBlockSize),
                    0, blockIndex + 1);
}

int Memory::writeSlow(dword value, qword address)
{
    const dword valueSize = static_cast<dword>(sizeof(dword));
    if (address > MEMORY::ADDRESS_SPACE - valueSize)
        return MEMORY::MEMORY_OVERFLOW;

    const dword offset = static_cast<dword>(address);
    const int blockIndex = hiword(static_cast<int>(offset));
    const int blockOffset = loword(static_cast<int>(offset));
    const byte *data = reinterpret_cast<const byte *>(&value);

    if (static_cast<dword>(blockOffset) + valueSize <= MEMORY::BLOCK_SIZE)
        return writeData(std::span<const byte>(data, valueSize), blockOffset, blockIndex);

    // Handle the case where the value spans across two blocks
    const dword firstBlockSize = static_cast<dword>(MEMORY::BLOCK_SIZE) - static_cast<dword>(blockOffset);
    int result = writeData(std::span<const byte>(data, firstBlockSize), blockOffset, blockIndex);
    if (result != MEMORY::OK)
        return result;
    return writeData(std::span<const byte>(data + firstBlockSize, valueSize - firstBlockSize),
                     0, blockIndex + 1);
}

int Memory::writes(std::span<const byte> buffer, dword offset, dword size)
{
    if (size > buffer.size())
        return MEMORY::MEMORY_UNAVAILABLE;
    if (overflow(offset, size))
        return MEMORY::MEMORY_OVERFLOW;

    dword written = 0;
    while (written < size)
    {
        const dword position = offset + written;
        const int blockIndex = hiword(position);
        const int blockOffset = loword(position);
        const dword blockRemain = static_cast<dword>(MEMORY::BLOCK_SIZE) - static_cast<dword>(blockOffset);
        const dword chunk = std::min(blockRemain, size - written);

        int result = writeData(buffer.subspan(written, chunk), blockOffset, blockIndex);
        if (result != MEMORY::OK)
            return result;
        written += chunk;
    }
    return MEMORY::OK;
}

int Memory::reads(std::span<byte> buffer, dword offset, dword size)
{
    if (size > buffer.size())
        return MEMORY::MEMORY_UNAVAILABLE;
    if (overflow(offset, size))
        return MEMORY::MEMORY_OVERFLOW;

    dword read = 0;
    while (read < size)
    {
        const dword position = offset + read;
        const int blockIndex = hiword(position);
        const int blockOffset = loword(position);
        const dword blockRemain = static_cast<dword>(MEMORY::BLOCK_SIZE) - static_cast<dword>(blockOffset);
        const dword chunk = std::min(blockRemain, size - read);

        int result = readData(buffer.subspan(read, chunk), blockOffset, blockIndex);
        if (result != MEMORY::OK)
            return result;
        read += chunk;
    }
    return MEMORY::OK;
}
