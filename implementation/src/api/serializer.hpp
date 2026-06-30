// serializer.hpp -- Common serialization and encoding utilities.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <cstring>
#include <concepts>

namespace kvs_detail
{

// Encode integral keys as ordered big-endian bytes using CPU-intrinsic byte swap.
template <typename Int>
inline std::array<std::byte, sizeof(Int)> encode_integral_key(Int value) noexcept
{
    using BareInt = std::remove_cv_t<Int>;
    static_assert(std::is_integral_v<BareInt>);
    static_assert(sizeof(BareInt) <= sizeof(uint64_t));

    using Unsigned = std::make_unsigned_t<BareInt>;
    Unsigned encoded = static_cast<Unsigned>(value);
    if constexpr (std::is_signed_v<BareInt>)
    {
        constexpr Unsigned sign_bit =
            Unsigned{1} << (sizeof(BareInt) * 8 - 1);
        encoded ^= sign_bit;
    }

    if constexpr (std::endian::native == std::endian::little)
    {
        encoded = std::byteswap(encoded);
    }

    std::array<std::byte, sizeof(BareInt)> out{};
    std::memcpy(out.data(), &encoded, sizeof(BareInt));
    return out;
}

// KeySerializer (Big Endian for lexicographical ordering)
template <typename T, typename Enable = void>
struct KeySerializer {
    static auto serialize(const T& val) {
        return serialize_kvs(val); // ADL
    }
};

template <typename T>
struct KeySerializer<T, std::enable_if_t<
    std::is_same_v<std::decay_t<T>, std::span<const std::byte>> ||
    std::is_same_v<std::decay_t<T>, std::vector<std::byte>> ||
    std::is_same_v<std::decay_t<T>, std::vector<char>>
>> {
    static std::span<const std::byte> serialize(const T& val) noexcept {
        return std::as_bytes(std::span(val));
    }
};

template <typename T>
struct KeySerializer<T, std::enable_if_t<
    std::is_convertible_v<std::decay_t<T>, std::string_view>
>> {
    static std::span<const std::byte> serialize(const T& val) noexcept {
        std::string_view view(val);
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(view.data()), view.size());
    }
};

template <typename T>
struct KeySerializer<T, std::enable_if_t<std::is_integral_v<std::decay_t<T>>>> {
    static auto serialize(T val) noexcept {
        return encode_integral_key(val);
    }
};

// ValueSerializer (Little Endian for raw data compatibility, no sign bit flipping)
template <typename T, typename Enable = void>
struct ValueSerializer {
    static auto serialize(const T& val) {
        return serialize_kvs(val); // ADL
    }
};

template <typename T>
struct ValueSerializer<T, std::enable_if_t<
    std::is_same_v<std::decay_t<T>, std::span<const std::byte>> ||
    std::is_same_v<std::decay_t<T>, std::vector<std::byte>> ||
    std::is_same_v<std::decay_t<T>, std::vector<char>>
>> {
    static std::span<const std::byte> serialize(const T& val) noexcept {
        return std::as_bytes(std::span(val));
    }
};

template <typename T>
struct ValueSerializer<T, std::enable_if_t<
    std::is_convertible_v<std::decay_t<T>, std::string_view>
>> {
    static std::span<const std::byte> serialize(const T& val) noexcept {
        std::string_view view(val);
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(view.data()), view.size());
    }
};

template <typename T>
struct ValueSerializer<T, std::enable_if_t<std::is_integral_v<std::decay_t<T>>>> {
    static auto serialize(T val) noexcept {
        std::array<std::byte, 8> encoded{};
        uint64_t v = static_cast<uint64_t>(val);
        for (size_t i = 0; i < 8; ++i)
            encoded[i] = static_cast<std::byte>((v >> (i * 8)) & 0xffu);
        return encoded;
    }
};

// Common lifetime coercion wrapper that invokes the callback with coerced span.
template <typename Serialized, typename Fn>
inline decltype(auto) invoke_with_span(Serialized &&serialized, Fn &&fn)
{
    if constexpr (std::same_as<std::decay_t<Serialized>, std::span<const std::byte>>) {
        return std::forward<Fn>(fn)(serialized);
    } else {
        return std::forward<Fn>(fn)(std::span<const std::byte>(serialized.data(), serialized.size()));
    }
}

// Lifetime extension wrappers
template <typename T, typename Fn>
decltype(auto) with_key_serialized(T &&val, Fn &&fn)
{
    auto serialized = KeySerializer<std::decay_t<T>>::serialize(std::forward<T>(val));
    return invoke_with_span(serialized, std::forward<Fn>(fn));
}

template <typename T, typename Fn>
decltype(auto) with_val_serialized(T &&val, Fn &&fn)
{
    auto serialized = ValueSerializer<std::decay_t<T>>::serialize(std::forward<T>(val));
    return invoke_with_span(serialized, std::forward<Fn>(fn));
}

} // namespace kvs_detail
