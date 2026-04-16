#include "manifest/DiskReader.hpp"

#include "AtariDiskEngine.h"

#include <QString>

#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace manifest {

namespace {

std::string qToStd(const QString& s) { return s.toStdString(); }

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string extensionUpper(const std::string& filename) {
    const auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return {};
    std::string ext = filename.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

std::string detectFormat(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::array<uint8_t, 4> magic{};
    f.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));

    const bool msa = (magic[0] == 0x0E && magic[1] == 0x0F);
    const bool stx = (magic[0] == 'R' && magic[1] == 'S' && magic[2] == 'Y' && magic[3] == 0x00);

    if (msa) return "MSA";
    if (stx) return "STX";

    const std::string ext = extensionUpper(path.filename().string());
    if (ext == "DIM") return "DIM";
    return "ST";
}

std::string readVolumeLabel(const Atari::AtariDiskEngine& engine) {
    const auto& raw = engine.getRawImageData();
    const auto bpb  = engine.getBpb();

    const uint32_t bps   = bpb.bytesPerSector ? bpb.bytesPerSector : 512;
    const uint32_t root_start_sector =
        static_cast<uint32_t>(bpb.reservedSectors) +
        static_cast<uint32_t>(bpb.fatCount) * static_cast<uint32_t>(bpb.sectorsPerFat);
    const uint32_t root_start_byte = root_start_sector * bps;
    const uint32_t root_bytes      = static_cast<uint32_t>(bpb.rootEntries) * 32u;

    if (root_bytes == 0 || root_start_byte + root_bytes > raw.size()) return {};

    for (uint32_t off = root_start_byte; off + 32 <= root_start_byte + root_bytes; off += 32) {
        const uint8_t* e = raw.data() + off;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        const uint8_t attr = e[11];
        if ((attr & 0x08) && !(attr & 0x10) && attr != 0x0F) {
            std::string label(reinterpret_cast<const char*>(e), 11);
            label = rtrim(std::move(label));
            std::string cleaned;
            cleaned.reserve(label.size());
            bool last_space = false;
            for (char c : label) {
                if (c == ' ') {
                    if (!last_space && !cleaned.empty()) cleaned += ' ';
                    last_space = true;
                } else {
                    cleaned += c;
                    last_space = false;
                }
            }
            return cleaned;
        }
    }
    return {};
}

FileRecord makeFileRecord(const Atari::DirEntry& e, bool in_root) {
    FileRecord f;
    f.filename      = e.getFilename();
    f.extension     = extensionUpper(f.filename);
    f.size_bytes    = e.getFileSize();
    f.start_cluster = e.getStartCluster();
    f.in_root       = in_root;
    return f;
}

} // namespace

struct DiskReader::Impl {
    Atari::AtariDiskEngine          engine;
    std::vector<Atari::DirEntry>    dir_entries;   // parallel to record.files
    DiskRecord                      record;

    void walk(uint16_t cluster, std::set<uint16_t>& visited) {
        std::vector<Atari::DirEntry> entries =
            (cluster == 0) ? engine.readRootDirectory()
                           : engine.readSubDirectory(cluster);

        for (const auto& e : entries) {
            if (e.name[0] == 0xE5)       continue;
            if (e.name[0] == '.' && cluster != 0) continue;
            if (e.attr & 0x08)           continue;   // volume label

            if (e.isDirectory()) {
                const uint16_t child = e.getStartCluster();
                if (child >= 2 && visited.insert(child).second) {
                    walk(child, visited);
                }
            } else {
                record.files.push_back(makeFileRecord(e, cluster == 0));
                dir_entries.push_back(e);
            }
        }
    }
};

DiskReader::DiskReader(const std::filesystem::path& image_path)
    : impl_(std::make_unique<Impl>()) {
    impl_->record.path     = image_path.string();
    impl_->record.filename = image_path.filename().string();
    impl_->record.format   = detectFormat(image_path);

    const QString qpath = QString::fromStdString(impl_->record.path);
    if (!impl_->engine.loadImage(qpath) || !impl_->engine.isLoaded()) {
        throw std::runtime_error("engine failed to load " + impl_->record.path);
    }

    const auto bpb = impl_->engine.getBpb();
    impl_->record.bytes_per_sector  = bpb.bytesPerSector;
    impl_->record.sectors_per_track = bpb.sectorsPerTrack;
    impl_->record.sides             = static_cast<uint8_t>(bpb.sides ? bpb.sides : 2);
    if (impl_->record.sectors_per_track > 0 && impl_->record.sides > 0) {
        impl_->record.tracks = static_cast<uint16_t>(
            bpb.totalSectors / (impl_->record.sectors_per_track * impl_->record.sides));
    }
    impl_->record.oem_name     = rtrim(qToStd(bpb.oemName));
    impl_->record.volume_label = readVolumeLabel(impl_->engine);

    if (!impl_->engine.isRawLoaderDisk()) {
        std::set<uint16_t> visited;
        impl_->walk(0, visited);
    }

    const QString group = impl_->engine.getGroupName();
    if (!group.isEmpty()) impl_->record.tags.push_back(qToStd(group));
    if (impl_->engine.isRawLoaderDisk()) impl_->record.tags.emplace_back("raw-loader");
}

DiskReader::~DiskReader() = default;

const DiskRecord& DiskReader::record() const {
    return impl_->record;
}

DiskRecord DiskReader::takeRecord() {
    return std::move(impl_->record);
}

const std::vector<uint8_t>& DiskReader::rawImage() const {
    return impl_->engine.getRawImageData();
}

std::vector<uint8_t> DiskReader::readFileBytes(size_t file_index) const {
    if (file_index >= impl_->dir_entries.size()) {
        throw std::out_of_range("DiskReader::readFileBytes index out of range");
    }
    return impl_->engine.readFile(impl_->dir_entries[file_index]);
}

std::vector<std::string> DiskReader::bootSectorStrings() const {
    std::vector<std::string> out;
    const auto q = impl_->engine.getBootSectorStrings();
    out.reserve(static_cast<std::size_t>(q.size()));
    for (const auto& s : q) out.push_back(qToStd(s));
    return out;
}

} // namespace manifest
