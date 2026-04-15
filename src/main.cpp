#include "manifest/Database.hpp"
#include "manifest/DiskReader.hpp"
#include "manifest/Identifier.hpp"
#include "manifest/MetadataExtractor.hpp"
#include "manifest/MultiDiskDetector.hpp"
#include "manifest/QueryCLI.hpp"
#include "manifest/HatariLauncher.hpp"
#include "manifest/Scanner.hpp"
#include "manifest/DiskRecord.hpp"
#include "manifest/gui/MainWindow.hpp"

#include <QApplication>
#include <QMetaType>
#include <QObject>
#include <QSettings>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path defaultDbPath() {
    if (const char* env = std::getenv("MANIFEST_DB"); env && *env) return env;
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / "manifest.db";
    }
    return "manifest.db";
}

// Pulls `--db <path>` out of argv starting at `start`. Returns the resolved
// path and removes both tokens from argv (simple in-place compaction).
fs::path parseDbFlag(int& argc, char** argv, int start) {
    fs::path result = defaultDbPath();
    int w = start;
    for (int r = start; r < argc; ++r) {
        if (std::strcmp(argv[r], "--db") == 0 && r + 1 < argc) {
            result = argv[r + 1];
            ++r;
            continue;
        }
        argv[w++] = argv[r];
    }
    argc = w;
    return result;
}

bool takeFlag(int& argc, char** argv, int start, const char* flag) {
    for (int r = start; r < argc; ++r) {
        if (std::strcmp(argv[r], flag) == 0) {
            for (int j = r; j < argc - 1; ++j) argv[j] = argv[j + 1];
            --argc;
            return true;
        }
    }
    return false;
}

int runInspect(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: manifest inspect <image-path>\n");
        return 2;
    }
    try {
        manifest::DiskReader reader(argv[2]);
        manifest::DiskRecord rec = reader.takeRecord();
        manifest::MetadataExtractor::enrich(reader, rec);
        manifest::Identifier().identify(rec);

        std::printf("path            %s\n", rec.path.c_str());
        std::printf("title           %s\n",
                    rec.identified_title.value_or("(unidentified)").c_str());
        std::printf("publisher       %s\n", rec.publisher.value_or("").c_str());
        std::printf("year            %s\n",
                    rec.year ? std::to_string(*rec.year).c_str() : "");
        std::printf("filename        %s\n", rec.filename.c_str());
        std::printf("format          %s\n", rec.format.c_str());
        std::printf("image_hash      %s\n", rec.image_hash.c_str());
        std::printf("volume_label    %s\n", rec.volume_label.c_str());
        std::printf("oem_name        %s\n", rec.oem_name.c_str());
        std::printf("geometry        %u sides / %u tracks / %u spt / %u bps\n",
                    rec.sides, rec.tracks, rec.sectors_per_track, rec.bytes_per_sector);
        std::printf("files (%zu):\n", rec.files.size());
        for (const auto& f : rec.files) {
            std::printf("  %c %-14s  %8u bytes  %s\n",
                        f.is_launcher ? '*' : ' ',
                        f.filename.c_str(), f.size_bytes, f.file_hash.c_str());
        }
        if (!rec.tags.empty()) {
            std::printf("tags           ");
            for (const auto& t : rec.tags) std::printf(" %s", t.c_str());
            std::printf("\n");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "inspect failed: %s\n", e.what());
        return 1;
    }
}

int runQuery(int argc, char** argv) {
    const fs::path db_path = parseDbFlag(argc, argv, 2);

    // Optional: --find <term> for one-shot mode.
    std::string find_term;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--find") == 0) { find_term = argv[i + 1]; break; }
    }

    try {
        manifest::Database  db(db_path);
        manifest::cli::QueryCLI cli(db);
        if (!find_term.empty()) return cli.findOnce(find_term);
        return cli.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "query failed: %s\n", e.what());
        return 1;
    }
}

int runLaunch(int argc, char** argv) {
    const fs::path db_path = parseDbFlag(argc, argv, 2);
    if (argc < 3) {
        std::fprintf(stderr, "usage: manifest launch <id> [--db <path>]\n");
        return 2;
    }
    try {
        const int64_t id = std::stoll(argv[2]);
        manifest::Database db(db_path);
        auto rec = db.queryById(id);
        if (!rec) {
            std::fprintf(stderr, "no such id: %lld\n", static_cast<long long>(id));
            return 1;
        }
        const auto r = manifest::HatariLauncher::launch(rec->path);
        if (!r.launched) { std::fprintf(stderr, "ERR: %s\n", r.error.c_str()); return 1; }
        std::printf("launched: %s\n", rec->path.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "launch failed: %s\n", e.what());
        return 1;
    }
}

int runScan(int argc, char** argv) {
    const bool incremental = takeFlag(argc, argv, 2, "--incremental");
    const fs::path db_path = parseDbFlag(argc, argv, 2);

    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: manifest scan <folder> [--incremental] [--db <path>]\n");
        return 2;
    }
    const fs::path root = argv[2];

    try {
        manifest::Database   db(db_path);
        manifest::Identifier identifier;
        manifest::Scanner    scanner(db, identifier);

        QObject::connect(&scanner, &manifest::Scanner::progress,
            [](int i, int n, const QString& path) {
                std::printf("[%d/%d] %s\n", i, n, path.toStdString().c_str());
            });

        const auto s = scanner.scan(root, incremental);
        manifest::MultiDiskDetector::detectAndPersist(db);
        std::printf("\n%zu scanned, %zu added, %zu updated, %zu skipped, %zu failed\n",
                    s.scanned, s.added, s.updated, s.skipped, s.failed);
        std::printf("disk_sets: %zu groups\n", db.listDiskSets().size());
        std::printf("db: %s\n", db_path.string().c_str());
        return s.failed == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scan failed: %s\n", e.what());
        return 1;
    }
}

} // namespace

// Routes between the GUI and the CLI.
//   manifest                     → GUI
//   manifest --gui               → GUI (explicit)
//   manifest inspect <path>      → dump DiskReader output (diagnostic)
//   manifest scan <folder> [--incremental] [--db <path>]
//   manifest query ...           → readline query shell (phase 5)
//   manifest launch <id> ...     → one-shot Hatari launch (phase 5)
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "inspect") == 0) return runInspect(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "scan")    == 0) return runScan(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "query")   == 0) return runQuery(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "launch")  == 0) return runLaunch(argc, argv);

    // --- GUI ----------------------------------------------------------------
    // Pull --db out of argv before handing it to QApplication.
    const bool     db_on_cli = std::any_of(argv + 1, argv + argc,
        [](const char* a){ return std::strcmp(a, "--db") == 0; });
    const fs::path cli_db    = parseDbFlag(argc, argv, 1);

    // Scanner signals cross threads → register the value types.
    qRegisterMetaType<manifest::DiskRecord>("manifest::DiskRecord");
    qRegisterMetaType<manifest::Scanner::Summary>("manifest::Scanner::Summary");

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ManifeST");
    QCoreApplication::setApplicationName("ManifeST");

    // Resolve DB path: explicit --db wins; otherwise use last-used from
    // QSettings; otherwise the default ($HOME/manifest.db).
    QString db_path;
    if (db_on_cli) {
        db_path = QString::fromStdString(cli_db.string());
    } else {
        QSettings s("ManifeST", "ManifeST");
        db_path = s.value("db/last_path").toString();
        if (db_path.isEmpty()) db_path = QString::fromStdString(cli_db.string());
    }

    try {
        manifest::gui::MainWindow w(db_path);
        w.show();
        return app.exec();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "GUI startup failed: %s\n", e.what());
        return 1;
    }
}
