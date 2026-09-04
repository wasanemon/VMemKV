#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace pskiplist {

// RAII POSIX file opened, sized, and mapped MAP_SHARED for direct in-place mutation. Sized
// once at construction and never grown. A brand-new file is zero-extended to size_bytes; an
// existing one must already be exactly that size.
class MmapFile {
 public:
  MmapFile(const std::filesystem::path &path, size_t size_bytes) : size_(size_bytes) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
      throw std::system_error(errno, std::generic_category(), "open(" + path.string() + ")");
    }

    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
      const int err = errno;
      ::close(fd_);
      throw std::system_error(err, std::generic_category(), "fstat(" + path.string() + ")");
    }

    if (st.st_size == 0) {
      if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        const int err = errno;
        ::close(fd_);
        throw std::system_error(err, std::generic_category(), "ftruncate(" + path.string() + ")");
      }
    } else {
      reused_ = true;
      if (static_cast<size_t>(st.st_size) != size_) {
        ::close(fd_);
        throw std::invalid_argument("pskiplist: " + path.string() +
                                    " exists with a size that does not match capacity_bytes");
      }
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
  // True if `path` already existed with content (i.e. wasn't freshly created) —
  // PSkipList uses this to decide whether to run recovery instead of a fresh init.
  [[nodiscard]] auto reused() const -> bool { return reused_; }

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
  bool reused_ = false;
};

}  // namespace pskiplist
