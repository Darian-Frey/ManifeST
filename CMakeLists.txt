cmake_minimum_required(VERSION 3.20)
project(ManifeST LANGUAGES CXX C)

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
    third_party/atari-engine/src/AtariDiskEngine.cpp
)
target_include_directories(atari_engine PUBLIC third_party/atari-engine/include)
target_link_libraries(atari_engine PUBLIC Qt6::Core)

# The engine has no Q_OBJECT macros; skip MOC to save build time.
set_target_properties(atari_engine PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# --- SQLite amalgamation (drop sqlite3.c / sqlite3.h into third_party/sqlite3/) ---
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)
# Enable FTS5 (virtual table `disks_fts` with the trigram tokenizer).
target_compile_definitions(sqlite3 PUBLIC SQLITE_ENABLE_FTS5)
# Pure C; MOC/UIC/RCC would be a no-op but skip to keep the build log clean.
set_target_properties(sqlite3 PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

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
    src/MultiDiskDetector.cpp
    src/DatImporter.cpp
    src/MenuDiskCatalog.cpp
    src/MenuImporter.cpp
    src/GameStringScanner.cpp
    # Q_OBJECT headers — listed so AUTOMOC finds them.
    include/manifest/Scanner.hpp
)
target_include_directories(manifest_lib PUBLIC include)
target_link_libraries(manifest_lib
    PUBLIC  Qt6::Core
    PRIVATE atari_engine
    PRIVATE sqlite3
    PRIVATE OpenSSL::Crypto
)

# --- GUI library ---
# Q_OBJECT headers are listed explicitly so AUTOMOC picks them up (they live
# under include/, not alongside the .cpp files).
add_library(manifest_gui STATIC
    src/gui/MainWindow.cpp
    src/gui/DiskTableModel.cpp
    src/gui/FilterProxy.cpp
    include/manifest/gui/MainWindow.hpp
    include/manifest/gui/DiskTableModel.hpp
    include/manifest/gui/FilterProxy.hpp
)
target_link_libraries(manifest_gui
    PUBLIC  manifest_lib
    PUBLIC  Qt6::Widgets
)

# --- CLI library (readline-driven query shell) ---
# readline is linked in Phase 5 when QueryCLI is actually implemented.
# Until then it's an optional dependency so the build stays green without
# libreadline-dev installed.
add_library(manifest_cli STATIC
    src/cli/QueryCLI.cpp
)
target_link_libraries(manifest_cli PUBLIC manifest_lib)

find_library(READLINE_LIB readline)
if(READLINE_LIB)
    target_link_libraries(manifest_cli PRIVATE ${READLINE_LIB})
    target_compile_definitions(manifest_cli PRIVATE MANIFEST_HAVE_READLINE=1)
endif()

# --- manifest binary: routes between GUI and CLI ---
add_executable(manifest src/main.cpp)
target_link_libraries(manifest
    PRIVATE manifest_gui
    PRIVATE manifest_cli
)

# --- Install / packaging (Linux) -------------------------------------------
include(GNUInstallDirs)
install(TARGETS manifest RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/manifest.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

# --- Tests ---
enable_testing()
add_subdirectory(tests)
