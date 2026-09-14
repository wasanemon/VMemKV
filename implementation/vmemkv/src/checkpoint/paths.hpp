// paths.hpp - Sibling-path derivation for one store's checkpoint files.
#pragma once

#include <filesystem>

namespace vmemkv {

// Sibling manifest path for a given T2 flat-file path.
inline auto derive_manifest_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".manifest"};
}

// T1 checkpoint / T2 checkpoint file paths. Each is a single path reused across
// every checkpoint this store ever commits: the T1 file is fully rewritten each
// cycle via temp+rename, and the T2 file is the store's one persistent data file,
// appended to in place. Neither is trusted by a reader until the manifest names the
// generation that last wrote them.
inline auto derive_t1_chk_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".t1chk"};
}
inline auto derive_t2_chk_path(const std::filesystem::path &t2_path) -> std::filesystem::path {
  return {t2_path.string() + ".t2chk"};
}

}  // namespace vmemkv
