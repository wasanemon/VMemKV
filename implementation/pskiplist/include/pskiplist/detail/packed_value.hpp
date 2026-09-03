#pragma once

#include <cstdint>

namespace pskiplist {

enum class NodeState : uint8_t {
  kLive = 0,
  kTombstonedLinked = 1,
  kTombstonedUnlinked = 2,
};

class PackedValue {
 public:
  PackedValue() = default;
  explicit PackedValue(uint64_t bits) : bits_(bits) {}
  static auto live(uint64_t payload) -> PackedValue { return PackedValue(encode(NodeState::kLive, payload)); }

  [[nodiscard]] auto state() const -> NodeState { return static_cast<NodeState>(bits_ >> kPayloadBits); }
  [[nodiscard]] auto payload() const -> uint64_t { return bits_ & kPayloadMask; }
  [[nodiscard]] auto raw() const -> uint64_t { return bits_; }
  [[nodiscard]] auto with_state(NodeState state) const -> PackedValue { return PackedValue(encode(state, payload())); }

  static constexpr int kPayloadBits = 62;
  static constexpr uint64_t kPayloadMask = (uint64_t{1} << kPayloadBits) - 1;

 private:
  static auto encode(NodeState state, uint64_t payload) -> uint64_t {
    return (static_cast<uint64_t>(state) << kPayloadBits) | (payload & kPayloadMask);
  }
  uint64_t bits_ = 0;
};

}  // namespace pskiplist
