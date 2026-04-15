#include "manifest/Database.hpp"

namespace manifest {

struct Database::Impl {
    ::sqlite3* db{nullptr};
};

Database::Database(const std::filesystem::path& /*db_path*/)
    : impl_(std::make_unique<Impl>()) {}

Database::~Database() = default;

void Database::upsertDisk(DiskRecord& /*record*/) {
}

void Database::upsertFiles(const DiskRecord& /*record*/) {
}

std::vector<DiskRecord> Database::queryByTitle(const std::string& /*term*/) const {
    return {};
}

std::optional<DiskRecord> Database::queryByHash(const std::string& /*image_hash*/) const {
    return std::nullopt;
}

} // namespace manifest
