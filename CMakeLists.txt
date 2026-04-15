cmake_minimum_required(VERSION 3.20)
project(ManifeST LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Qt 6 ---
# Widgets pulls in Core + Gui. The engine subset only uses Core types.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Vendored Atari disk engine (curated subset) ---
# The upstream engine lives in atari-disk-engine/ (gitignored, kept locally
# for reference). We vendor only the two files we need, unmodified, into
# third_party/atari-engine/.
add_library(atari_engine STATIC
    third_party/atari-engine/AtariDiskEngine.cpp
)
target_include_directories(atari_engine PUBLIC third_party/atari-engine)
target_link_libraries(atari_engine PUBLIC Qt6::Core)

# --- SQLite amalgamation (drop sqlite3.c / sqlite3.h into third_party/sqlite3/) ---
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)

# --- OpenSSL (SHA1) ---
find_package(OpenSSL REQUIRED)

# --- ManifeST core library (shared between GUI and CLI) ---
add_library(manifest_lib STATIC
    src/DiskReader.cpp
    src/MetadataExtractor.cpp
    src/Identifier.cpp
    src/Database.cpp
    src/Scanner.cpp
    src/HatariLauncher.cpp
)
target_include_directories(manifest_lib PUBLIC include)
target_link_libraries(manifest_lib
    PUBLIC  Qt6::Core
    PRIVATE atari_engine
    PRIVATE sqlite3
    PRIVATE OpenSSL::Crypto
)

# --- GUI library ---
add_library(manifest_gui STATIC
    src/gui/MainWindow.cpp
    src/gui/DiskTableModel.cpp
)
target_link_libraries(manifest_gui
    PUBLIC  manifest_lib
    PUBLIC  Qt6::Widgets
)

# --- CLI library (readline-driven query shell) ---
add_library(manifest_cli STATIC
    src/cli/QueryCLI.cpp
)
target_link_libraries(manifest_cli
    PUBLIC  manifest_lib
    PRIVATE readline
)

# --- manifest binary: routes between GUI and CLI ---
add_executable(manifest src/main.cpp)
target_link_libraries(manifest
    PRIVATE manifest_gui
    PRIVATE manifest_cli
)

# --- Tests ---
enable_testing()
add_subdirectory(tests)
