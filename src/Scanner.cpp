#include "manifest/Scanner.hpp"

#include "manifest/CrackerGroupDetector.hpp"
#include "manifest/Database.hpp"
#include "manifest/DiskReader.hpp"
#include "manifest/GameStringScanner.hpp"
#include "manifest/Identifier.hpp"
#include "manifest/MenuDiskCatalog.hpp"
#include "manifest/MetadataExtractor.hpp"
#include "manifest/ScreenScraperCache.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

namespace manifest {

namespace {

bool isImageExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    if (ext.empty() || ext[0] != '.') return false;
    ext.erase(ext.begin());
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext == "ST" || ext == "MSA" || ext == "DIM" || ext == "STX";
}

std::vector<std::filesystem::path> collectImages(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root,
             std::filesystem::directory_options::skip_permission_denied, ec),
             end; it != end; it.increment(ec)) {
        if (ec) {
            std::fprintf(stderr, "WARN: %s — skipping\n", ec.message().c_str());
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (isImageExtension(it->path())) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

Scanner::Scanner(Database& db, const Identifier& identifier, QObject* parent)
    : QObject(parent), db_(db), identifier_(identifier) {}

void Scanner::requestCancel() {
    cancel_.store(true, std::memory_order_relaxed);
}

Scanner::Summary Scanner::scan(const std::filesystem::path& root,
                               bool incremental, bool quick) {
    cancel_.store(false, std::memory_order_relaxed);
    Summary summary;

    if (!std::filesystem::exists(root)) {
        std::fprintf(stderr, "ERR: scan root does not exist: %s\n", root.string().c_str());
        emit finished(summary);
        return summary;
    }

    const auto images = collectImages(root);
    const int total = static_cast<int>(images.size());

    for (int i = 0; i < total; ++i) {
        if (cancel_.load(std::memory_order_relaxed)) break;

        const auto& path = images[static_cast<std::size_t>(i)];
        const std::string path_str = path.string();

        emit progress(i + 1, total, QString::fromStdString(path_str));
        ++summary.scanned;

        const bool already_in_db = db_.pathExists(path_str);

        // Stat the file upfront so we can use mtime/size for the
        // incremental-skip check AND record them if we do scan.
        std::error_code ec;
        const auto fsize = std::filesystem::file_size(path, ec);
        int64_t file_size = 0;
        int64_t file_mtime = 0;
        if (!ec) {
            file_size = static_cast<int64_t>(fsize);
            const auto ftime = std::filesystem::last_write_time(path, ec);
            if (!ec) {
                // Cast to system_clock for epoch-seconds reading.
                const auto sys_tp = std::chrono::file_clock::to_sys(ftime);
                file_mtime = std::chrono::duration_cast<std::chrono::seconds>(
                    sys_tp.time_since_epoch()).count();
            }
        }

        if (incremental && already_in_db &&
            db_.fileStatMatches(path_str, file_mtime, file_size)) {
            ++summary.skipped;
            continue;
        }

        try {
            DiskReader reader(path);
            DiskRecord rec = reader.takeRecord();
            rec.file_mtime = file_mtime;
            rec.file_size  = file_size;

            // Hashing — full (per-file) in deep mode, image-only in quick.
            if (quick) MetadataExtractor::enrichImageOnly(reader, rec);
            else       MetadataExtractor::enrich        (reader, rec);

            identifier_.identify(rec);
            if (ss_cache_)     ss_cache_->enrich(rec);
            if (menu_catalog_) menu_catalog_->enrich(rec);

            // The three byte-scanning passes below dominate scan time.
            // Quick mode skips all of them — image hash + file listing +
            // catalog enrichment alone still produce a useful catalogue
            // for the common "point-at-folder, see what's there" case.
            if (!quick) {
                if (menu_catalog_) {
                    rec.detected_games = GameStringScanner::scan(
                        reader.rawImage(), menu_catalog_->allKnownGamesUpper());
                }
                for (const auto& group :
                     CrackerGroupDetector::detect(reader.rawImage())) {
                    if (std::find(rec.tags.begin(), rec.tags.end(), group)
                        == rec.tags.end()) {
                        rec.tags.push_back(group);
                    }
                }
                for (const auto& s : reader.bootSectorStrings()) {
                    if (s.size() < 4) continue;
                    rec.text_fragments.push_back({"boot", s});
                }
                GameStringScanner::Config cfg{30, 512, 6};
                auto runs = GameStringScanner::extractRuns(reader.rawImage(), cfg);
                std::sort(runs.begin(), runs.end(),
                          [](const std::string& a, const std::string& b){
                              return a.size() > b.size();
                          });
                const std::size_t kMaxDeep = 25;
                for (std::size_t i = 0; i < runs.size() && i < kMaxDeep; ++i) {
                    rec.text_fragments.push_back({"deep", std::move(runs[i])});
                }
            }

            {
                Database::Transaction tx(db_);
                db_.upsertDisk(rec);
                db_.upsertFiles(rec);
                db_.upsertTags(rec);
                db_.upsertMenuContents(rec);
                db_.upsertDetectedGames(rec);
                db_.upsertTextFragments(rec);
                tx.commit();
            }

            if (already_in_db) ++summary.updated;
            else               ++summary.added;

            emit imageDone(rec);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "WARN: %s — %s — skipping\n",
                         path_str.c_str(), e.what());
            ++summary.failed;
        }
    }

    emit finished(summary);
    return summary;
}

} // namespace manifest
