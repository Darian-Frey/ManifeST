#include "manifest/Scanner.hpp"

#include "manifest/Database.hpp"
#include "manifest/DiskReader.hpp"
#include "manifest/Identifier.hpp"
#include "manifest/MetadataExtractor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <string>
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

Scanner::Summary Scanner::scan(const std::filesystem::path& root, bool incremental) {
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
        if (incremental && already_in_db) {
            ++summary.skipped;
            continue;
        }

        try {
            DiskReader reader(path);
            DiskRecord rec = reader.takeRecord();
            MetadataExtractor::enrich(reader, rec);
            identifier_.identify(rec);

            {
                Database::Transaction tx(db_);
                db_.upsertDisk(rec);
                db_.upsertFiles(rec);
                db_.upsertTags(rec);
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
