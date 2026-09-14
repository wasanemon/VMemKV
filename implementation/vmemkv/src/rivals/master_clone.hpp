// master_clone.hpp — Shared building→build→atomic_rename→clone skeleton for rivals.
#pragma once

#include <concepts>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include "rival_common.hpp"

namespace vmemkv::rivals {

// Shared tag selecting the clone-from-master constructor.
struct CloneFromMasterTag {};

// Policy shape for a master/clone backend (RocksDB form as model):
// - static bool master_exists(const std::string&)
// - static void prepare_building(const std::string&)
// - template KeyFn/ValueFn static void build_at(building, key_count, make_key, make_value)
// - static void finish_build(building, master, label)
// - static void clone_from(source, dest)
template <typename Policy>
concept MasterClonePolicy = requires(const std::string &p, const std::string &b, const std::string &m) {
  { Policy::master_exists(m) } -> std::convertible_to<bool>;
  { Policy::prepare_building(b) } -> std::same_as<void>;
  { Policy::finish_build(b, m, "") } -> std::same_as<void>;
  { Policy::clone_from(p, p) } -> std::same_as<void>;
};

// Generic ensure_master_built: existence check, then build at ".building"
// sibling and atomically rename into place.
template <MasterClonePolicy Policy, typename KeyFn, typename ValueFn>
inline void ensure_master_built_generic(const std::string &master_path,
                                        std::size_t key_count,
                                        KeyFn &&make_key,
                                        ValueFn &&make_value,
                                        const char *label) {
  if (Policy::master_exists(master_path)) {
    return;
  }
  const std::string building = building_path(master_path);
  Policy::prepare_building(building);
  Policy::build_at(building, key_count, std::forward<KeyFn>(make_key), std::forward<ValueFn>(make_value));
  Policy::finish_build(building, master_path, label);
}

// Rival-side policy surface preparing a future StoreAdapter concept migration
// (StoreAdapter itself is outside owned scope and unchanged here).
struct RivalPolicy {
  static void checkpoint() noexcept {}
  static auto defragment() noexcept -> bool { return false; }
};

template <typename T>
concept IsRivalStore = requires {
  { T::kIsRival } -> std::convertible_to<bool>;
};

}  // namespace vmemkv::rivals
