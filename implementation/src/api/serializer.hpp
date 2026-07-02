// serializer.hpp -- Common serialization and encoding utilities.
#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kvs_detail {

inline constexpr std::size_t kEncodedValueBytes = 8;

// Encode integral keys as ordered big-endian bytes using CPU-intrinsic byte swap.
template <typename Int>
inline auto encode_integral_key(Int value) noexcept -> std::array<std::byte, sizeof(Int)> {
  using BareInt = std::remove_cv_t<Int>;
  static_assert(std::is_integral_v<BareInt>);
  static_assert(sizeof(BareInt) <= sizeof(uint64_t));

  using Unsigned = std::make_unsigned_t<BareInt>;
  auto encoded = static_cast<Unsigned>(value);
  if constexpr (std::is_signed_v<BareInt>) {
    constexpr Unsigned sign_bit = Unsigned{1} << (sizeof(BareInt) * 8 - 1);
    encoded ^= sign_bit;
  }

  if constexpr (std::endian::native == std::endian::little) {
    encoded = std::byteswap(encoded);
  }

  std::array<std::byte, sizeof(BareInt)> out{};
  std::memcpy(out.data(), &encoded, sizeof(BareInt));
  return out;
}

// KeySerializer (Big Endian for lexicographical ordering)
template <typename T, typename Enable = void>
struct KeySerializer {
  static auto serialize(const T& value) {
    return serialize_kvs(value);  // ADL
  }
};

template <typename T>
struct KeySerializer<T,
                     std::enable_if_t<std::is_same_v<std::decay_t<T>, std::span<const std::byte>> ||
                                      std::is_same_v<std::decay_t<T>, std::vector<std::byte>> ||
                                      std::is_same_v<std::decay_t<T>, std::vector<char>>>> {
  static auto serialize(const T& value) noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span(value));
  }
};

template <typename T>
struct KeySerializer<T, std::enable_if_t<std::is_convertible_v<std::decay_t<T>, std::string_view>>> {
  static auto serialize(const T& value) noexcept -> std::span<const std::byte> {
    std::string_view view(value);
    return {reinterpret_cast<const std::byte*>(view.data()), view.size()};
  }
};

template <typename T>
struct KeySerializer<T, std::enable_if_t<std::is_integral_v<std::decay_t<T>>>> {
  static auto serialize(T val) noexcept { return encode_integral_key(val); }
};

// ValueSerializer (Little Endian for raw data compatibility, no sign bit flipping)
template <typename T, typename Enable = void>
struct ValueSerializer {
  static auto serialize(const T& value) {
    return serialize_kvs(value);  // ADL
  }
};

template <typename T>
struct ValueSerializer<T,
                       std::enable_if_t<std::is_same_v<std::decay_t<T>, std::span<const std::byte>> ||
                                        std::is_same_v<std::decay_t<T>, std::vector<std::byte>> ||
                                        std::is_same_v<std::decay_t<T>, std::vector<char>>>> {
  static auto serialize(const T& value) noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span(value));
  }
};

template <typename T>
struct ValueSerializer<T, std::enable_if_t<std::is_convertible_v<std::decay_t<T>, std::string_view>>> {
  static auto serialize(const T& value) noexcept -> std::span<const std::byte> {
    std::string_view view(value);
    return {reinterpret_cast<const std::byte*>(view.data()), view.size()};
  }
};

template <typename T>
struct ValueSerializer<T, std::enable_if_t<std::is_integral_v<std::decay_t<T>>>> {
  static auto serialize(T value) noexcept {
    constexpr uint64_t kByteMask = 0xffU;
    std::array<std::byte, kEncodedValueBytes> encoded{};
    auto encoded_value = static_cast<uint64_t>(value);
    for (size_t index = 0; index < kEncodedValueBytes; ++index) {
      encoded[index] = static_cast<std::byte>((encoded_value >> (index * kEncodedValueBytes)) & kByteMask);
    }
    return encoded;
  }
};

// Common lifetime coercion wrapper that invokes the callback with coerced span.
template <typename Serialized, typename Callback>
inline auto invoke_with_span(Serialized&& serialized, Callback&& callback) -> decltype(auto) {
  if constexpr (std::same_as<std::decay_t<Serialized>, std::span<const std::byte>>) {
    return std::forward<Callback>(callback)(serialized);
  } else {
    return std::forward<Callback>(callback)(std::span<const std::byte>(serialized.data(), serialized.size()));
  }
}

// Lifetime extension wrappers
template <typename T, typename Callback>
auto with_key_serialized(T&& value, Callback&& callback) -> decltype(auto) {
  auto serialized = KeySerializer<std::decay_t<T>>::serialize(std::forward<T>(value));
  return invoke_with_span(serialized, std::forward<Callback>(callback));
}

template <typename T, typename Callback>
auto with_val_serialized(T&& value, Callback&& callback) -> decltype(auto) {
  auto serialized = ValueSerializer<std::decay_t<T>>::serialize(std::forward<T>(value));
  return invoke_with_span(serialized, std::forward<Callback>(callback));
}

}  // namespace kvs_detail
