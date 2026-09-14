// io.hpp - Exact-length fd read/write helpers.
#pragma once

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <system_error>

namespace vmemkv {

// RAII owner of one file descriptor. Closes on destruction, movable, non-copyable.
struct FdGuard {
  int fd = -1;
  FdGuard() noexcept = default;
  explicit FdGuard(int fd_in) noexcept : fd(fd_in) {}
  ~FdGuard() noexcept {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  FdGuard(const FdGuard &) = delete;
  auto operator=(const FdGuard &) -> FdGuard & = delete;
  FdGuard(FdGuard &&other) noexcept : fd(other.fd) { other.fd = -1; }
  auto operator=(FdGuard &&other) noexcept -> FdGuard & {
    if (this != &other) {
      if (fd >= 0) {
        ::close(fd);
      }
      fd = other.fd;
      other.fd = -1;
    }
    return *this;
  }
};

// Writes exactly `size` bytes or throws. Leaves `file_descriptor` open.
inline void write_all_exact(int file_descriptor, const void *data, size_t size, const char *what) {
  const auto *bytes = static_cast<const std::byte *>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::write(file_descriptor, bytes + offset, size - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(), what);
    }
    if (written == 0) {
      throw std::system_error(EIO, std::generic_category(), what);
    }
    offset += static_cast<size_t>(written);
  }
}

// Reads exactly `size` bytes at `offset` or throws. Leaves `file_descriptor` open.
inline void pread_exact(int file_descriptor, void *data, size_t size, off_t offset, const char *what) {
  auto *bytes = static_cast<std::byte *>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t got = ::pread(file_descriptor, bytes + done, size - done, offset + static_cast<off_t>(done));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(), what);
    }
    if (got == 0) {
      throw std::system_error(EIO, std::generic_category(), what);
    }
    done += static_cast<size_t>(got);
  }
}

// Writes exactly `size` bytes at `offset` or throws. Leaves `file_descriptor` open.
inline void pwrite_all(int file_descriptor, const void *data, size_t size, off_t offset, const char *what) {
  const auto *bytes = static_cast<const std::byte *>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t written = ::pwrite(file_descriptor, bytes + done, size - done, offset + static_cast<off_t>(done));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(), what);
    }
    if (written == 0) {
      throw std::system_error(EIO, std::generic_category(), what);
    }
    done += static_cast<size_t>(written);
  }
}

}  // namespace vmemkv
