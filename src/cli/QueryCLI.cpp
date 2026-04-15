#include "manifest/QueryCLI.hpp"

#include "manifest/Database.hpp"

namespace manifest {

QueryCLI::QueryCLI(Database& db) : db_(db) {}

int QueryCLI::run() {
    return 0;
}

} // namespace manifest
