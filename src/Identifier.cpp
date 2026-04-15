#include "manifest/Identifier.hpp"

namespace manifest {

Identifier::Identifier(std::optional<std::filesystem::path> tosec_json)
    : tosec_json_(std::move(tosec_json)) {}

void Identifier::identify(DiskRecord& /*record*/) const {
}

} // namespace manifest
