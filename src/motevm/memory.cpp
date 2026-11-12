#include <algorithm>
#include "memory.hpp"

bool Memory::overflow(dword offset, dword size)
{
    const qword end = static_cast<qword>(offset) + size;
    return end > MEMORY::ADDRESS_SPACE;
}

int Memory::getBlockStatus(std::size_t index)
{
    if (index >= MEMORY::BLOCK_COUNT)
        return MEMORY::RELEASED;
    return blocks[index].status;
}

int Memory::allocate(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= MEMORY::BLOCK_COUNT)
        return MEMORY::MEMORY_UNAVAILABLE;
    const std::size_t slot = static_cast<std::size_t>(index);
    if (blocks[slot].status == MEMORY::FREE)
        return MEMORY::OK; // idempotent
    blocks[slot].pointer = std::make_shared<std::array<byte, MEMORY::BLOCK_SIZE>>();
    blocks[slot].status = MEMORY::FREE;
    bases[slot] = blocks[slot].pointer->data();
    return MEMORY::OK;
}

void Memory::free(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= MEMORY::BLOCK_COUNT)
        return;
    const std::size_t slot = static_cast<std::size_t>(index);
    bases[slot] = nullptr;
    blocks[slot].pointer.reset();
    blocks[slot].status = MEMORY::RELEASED;
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
int Memory::writeData(std::span<const byte> buffer, std::size_t offset, std::size_t index)
{
    if (getBlockStatus(index) == MEMORY::RELEASED)
        return MEMORY::MEMORY_UNAVAILABLE;
    if (offset + buffer.size() > MEMORY::BLOCK_SIZE)
        return MEMORY::MEMORY_OVERFLOW;
    std::copy(buffer.begin(), buffer.end(),
              blocks[index].pointer->begin() + static_cast<std::ptrdiff_t>(offset));
    return MEMORY::OK;
}

int Memory::readData(std::span<byte> buffer, std::size_t offset, std::size_t index)
{
    if (getBlockStatus(index) == MEMORY::RELEASED)
        return MEMORY::MEMORY_UNAVAILABLE;
    if (offset + buffer.size() > MEMORY::BLOCK_SIZE)
        return MEMORY::MEMORY_OVERFLOW;
    std::copy(blocks[index].pointer->begin() + static_cast<std::ptrdiff_t>(offset),
              blocks[index].pointer->begin() + static_cast<std::ptrdiff_t>(offset + buffer.size()),
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
    const std::size_t blockIndex = static_cast<std::size_t>(offset >> 16);
    const std::size_t blockOffset = static_cast<std::size_t>(offset & 0xFFFFu);
    byte *data = reinterpret_cast<byte *>(&value);

    if (blockOffset + valueSize <= MEMORY::BLOCK_SIZE)
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
    const std::size_t blockIndex = static_cast<std::size_t>(offset >> 16);
    const std::size_t blockOffset = static_cast<std::size_t>(offset & 0xFFFFu);
    const byte *data = reinterpret_cast<const byte *>(&value);

    if (blockOffset + valueSize <= MEMORY::BLOCK_SIZE)
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
        const std::size_t blockIndex = static_cast<std::size_t>(position >> 16);
        const std::size_t blockOffset = static_cast<std::size_t>(position & 0xFFFFu);
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
        const std::size_t blockIndex = static_cast<std::size_t>(position >> 16);
        const std::size_t blockOffset = static_cast<std::size_t>(position & 0xFFFFu);
        const dword blockRemain = static_cast<dword>(MEMORY::BLOCK_SIZE) - static_cast<dword>(blockOffset);
        const dword chunk = std::min(blockRemain, size - read);

        int result = readData(buffer.subspan(read, chunk), blockOffset, blockIndex);
        if (result != MEMORY::OK)
            return result;
        read += chunk;
    }
    return MEMORY::OK;
}
