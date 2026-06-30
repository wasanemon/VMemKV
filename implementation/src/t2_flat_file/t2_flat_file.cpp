#include "t2_flat_file.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cassert>
#include <cstring>
#include <system_error>
#include <limits>
#include <algorithm>
#include <vmemkv/config.hpp>

namespace vmemkv {

T2FlatFile::T2FlatFile(const std::filesystem::path &path, uint64_t bytes_capacity)
    : path_(path),
      t2_bytes_capacity_(bytes_capacity)
{
    t2_bytes_used_.store(0, std::memory_order_release);
    map_file(path, create_empty_file(path, bytes_capacity));
}



T2RecordView T2FlatFile::at(uint64_t payload, const std::shared_ptr<const T2Memory> &mem) const noexcept
{
    const std::byte *record_base = resolve_record(payload, mem);
    const auto *header = reinterpret_cast<const ValueRecordHeader *>(record_base);
    const std::byte *key_begin = reinterpret_cast<const std::byte *>(header + 1);
    const std::byte *value_begin = key_begin + header->key_len;

    return T2RecordView{
        header,
        std::span<const std::byte>(key_begin, header->key_len),
        std::span<const std::byte>(value_begin, header->value_len),
    };
}

uint64_t T2FlatFile::append(std::span<const std::byte> key, std::span<const std::byte> value)
{
    const uint64_t raw_required = sizeof(ValueRecordHeader) + key.size() + value.size();
    // Align all record sizes to 8 bytes. This avoids unaligned memory access penalties 
    // on modern CPU architectures and allows callers to cast fields safely.
    const uint64_t required = vmemkv::align_up(raw_required);

    const uint64_t offset = t2_bytes_used_.fetch_add(required, std::memory_order_relaxed);

    if (offset + required > t2_bytes_capacity_)
    {
        throw std::runtime_error("T2 storage capacity exceeded");
    }

    std::shared_ptr<const T2Memory> mem = get_memory();
    std::byte *record_base = mem->base + offset;

    auto *header = reinterpret_cast<ValueRecordHeader *>(record_base);
    header->key_len = static_cast<uint32_t>(key.size());
    header->value_len = static_cast<uint32_t>(value.size());
    header->alloc_len = static_cast<uint32_t>(value.size());
    header->flags = 0;
    header->version = 0;

    std::byte *cursor = reinterpret_cast<std::byte *>(header + 1);
    std::memcpy(cursor, key.data(), key.size());
    cursor += key.size();

    if (!value.empty()) {
        std::memcpy(cursor, value.data(), value.size());
        cursor += value.size();
    }

    const uint64_t padding = required - raw_required;
    if (padding > 0)
        std::memset(cursor, 0, padding);

    return offset;
}



bool T2FlatFile::update_value_at(uint64_t payload, std::span<const std::byte> value) noexcept
{
    std::shared_ptr<const T2Memory> mem = get_memory();
    auto *header = reinterpret_cast<ValueRecordHeader *>(
        const_cast<std::byte *>(resolve_record(payload, mem)));

    if (value.size() > header->alloc_len)
        return false;

    std::byte *value_begin =
        reinterpret_cast<std::byte *>(header + 1) + header->key_len;

    if (!value.empty())
        std::memcpy(value_begin, value.data(), value.size());

    header->value_len = static_cast<uint32_t>(value.size());
    ++header->version;
    return true;
}

void T2FlatFile::swap_memory(const std::shared_ptr<const T2Memory> &new_mem, uint64_t bytes_used)
{
    t2_mem_.store(new_mem, std::memory_order_release);
    t2_bytes_used_.store(bytes_used, std::memory_order_release);
    t2_bytes_capacity_ = new_mem->capacity;
}

void T2FlatFile::map_file(const std::filesystem::path &path, uint64_t bytes_capacity)
{
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "open");

    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "fstat");
    }

    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) < bytes_capacity)
    {
        ::close(fd);
        throw std::invalid_argument("T2 file is smaller than capacity");
    }

    void *mapped = ::mmap(nullptr, static_cast<size_t>(bytes_capacity),
                          PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    const int mmap_errno = errno;
    ::close(fd);
    if (mapped == MAP_FAILED)
    {
        throw std::system_error(mmap_errno, std::generic_category(), "mmap");
    }

    t2_mem_.store(std::make_shared<const T2Memory>(static_cast<std::byte *>(mapped), bytes_capacity),
                  std::memory_order_release);
    t2_bytes_capacity_ = bytes_capacity;
}

uint64_t T2FlatFile::create_empty_file(const std::filesystem::path &path, uint64_t bytes_capacity)
{
    if (bytes_capacity == 0)
        throw std::invalid_argument("T2Store capacity must be greater than zero");
    if (bytes_capacity > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::invalid_argument("T2Store capacity is too large for mmap");
    if (bytes_capacity > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::invalid_argument("T2Store capacity is too large for ftruncate");

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "open");

    if (::ftruncate(fd, static_cast<off_t>(bytes_capacity)) != 0)
    {
        const int err = errno;
        ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw std::system_error(err, std::generic_category(), "ftruncate");
    }

    ::close(fd);
    return bytes_capacity;
}

} // namespace vmemkv
