#include "manifest/Scanner.hpp"

#include "manifest/Database.hpp"
#include "manifest/Identifier.hpp"

namespace manifest {

Scanner::Scanner(Database& db, const Identifier& identifier)
    : db_(db), identifier_(identifier) {}

Scanner::Summary Scanner::scan(const std::filesystem::path& /*root*/, bool /*incremental*/) {
    return {};
}

} // namespace manifest
