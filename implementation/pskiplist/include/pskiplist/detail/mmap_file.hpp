#pragma once

#include <cstddef>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace pskiplist {

// RAII POSIX file opened, sized, and mapped MAP_SHARED for direct in-place mutation.
// Sized once at construction and never grown (2.6節). The file is always truncated to
// zero first, so construction always starts from a fresh, fully-zeroed mapping —
// reopening an existing file's prior contents is recovery's concern, not this class's.
class MmapFile {
 public:
  MmapFile(const std::filesystem::path &path, size_t size_bytes) : size_(size_bytes) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
      throw std::system_error(errno, std::generic_category(), "open(" + path.string() + ")");
    }
    if (::ftruncate(fd_, 0) != 0 || ::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
      const int err = errno;
      ::close(fd_);
      throw std::system_error(err, std::generic_category(), "ftruncate(" + path.string() + ")");
    }
    data_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
      const int err = errno;
      ::close(fd_);
      throw std::system_error(err, std::generic_category(), "mmap(" + path.string() + ")");
    }
  }

  ~MmapFile() {
    if (data_ != nullptr && data_ != MAP_FAILED) {
      ::munmap(data_, size_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  MmapFile(const MmapFile &) = delete;
  auto operator=(const MmapFile &) -> MmapFile & = delete;

  [[nodiscard]] auto data() const -> void * { return data_; }
  [[nodiscard]] auto size() const -> size_t { return size_; }

  // Blocks until the mapping's dirty pages are durable on the underlying storage (3章).
  void sync() const {
    if (::msync(data_, size_, MS_SYNC) != 0) {
      throw std::system_error(errno, std::generic_category(), "msync");
    }
  }

 private:
  int fd_ = -1;
  void *data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace pskiplist
