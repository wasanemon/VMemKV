// master_clone.hpp — Shared clone-from-master tag for rivals.
#pragma once

#include <filesystem>
#include <string>

#include "rival_common.hpp"

namespace vmemkv::rivals {

// Shared tag selecting the clone-from-master constructor.
struct CloneFromMasterTag {};

}  // namespace vmemkv::rivals
