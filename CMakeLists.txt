cmake_minimum_required(VERSION 3.20)
project(ManifeST LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Atari disk engine ---
add_subdirectory(atari-disk-engine engine)

# --- SQLite amalgamation ---
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)

# --- OpenSSL (for SHA1) ---
find_package(OpenSSL REQUIRED)

# --- ManifeST library ---
add_library(manifest_lib STATIC
    src/DiskReader.cpp
    src/MetadataExtractor.cpp
    src/Identifier.cpp
    src/Database.cpp
    src/Scanner.cpp
    src/QueryCLI.cpp
)
target_include_directories(manifest_lib PUBLIC include)
target_link_libraries(manifest_lib
    PRIVATE atari_disk_engine
    PRIVATE sqlite3
    PRIVATE OpenSSL::Crypto
    PRIVATE readline
)

# --- manifest binary ---
add_executable(manifest src/main.cpp)
target_link_libraries(manifest PRIVATE manifest_lib)

# --- Tests ---
enable_testing()
add_subdirectory(tests)
