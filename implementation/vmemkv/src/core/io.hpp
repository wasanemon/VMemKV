// io.hpp - Exact-length fd read/write helpers.
#pragma once

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <system_error>

namespace vmemkv {

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
