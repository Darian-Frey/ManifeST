#include "manifest/DiskReader.hpp"

#include "AtariDiskEngine.h"

#include <QString>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>

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

// Sniff first 4 bytes of the file to classify the container format.
// Extension is a fallback tiebreaker (.DIM in particular has no single magic).
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

// Walk the engine's in-memory image to find the one FAT12 volume-label entry.
// The engine's readRootDirectory() strips volume labels before returning, so
// we parse the raw root-dir region ourselves using geometry from the BPB.
// Returns empty string if none found.
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
        if (e[0] == 0x00) break;      // end of directory
        if (e[0] == 0xE5) continue;   // deleted entry
        const uint8_t attr = e[11];
        // Volume label: bit 3 set, bit 4 (directory) clear. Skip LFN (attr == 0x0F)
        // even though it shouldn't occur on FAT12 Atari disks.
        if ((attr & 0x08) && !(attr & 0x10) && attr != 0x0F) {
            std::string label(reinterpret_cast<const char*>(e), 11);
            label = rtrim(std::move(label));
            // 8.3 label entries store name + ext contiguously with space padding
            // between them; collapse any interior run of spaces to a single one.
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

FileRecord makeFileRecord(const Atari::DirEntry& e) {
    FileRecord f;
    f.filename      = e.getFilename();   // "NAME.EXT"
    f.extension     = extensionUpper(f.filename);
    f.size_bytes    = e.getFileSize();
    f.start_cluster = e.getStartCluster();
    f.file_hash.clear();                 // filled later by MetadataExtractor
    f.is_launcher   = false;             // flagged later by MetadataExtractor
    return f;
}

void walkDirectory(const Atari::AtariDiskEngine& engine,
                   uint16_t cluster,                 // 0 = root
                   std::vector<FileRecord>& out,
                   std::set<uint16_t>& visited) {
    std::vector<Atari::DirEntry> entries =
        (cluster == 0) ? engine.readRootDirectory()
                       : engine.readSubDirectory(cluster);

    for (const auto& e : entries) {
        // Skip "." and ".." pseudo-entries in subdirectories.
        if (e.name[0] == '.' && cluster != 0) continue;
        // Skip deleted / volume-label entries (engine may or may not filter these).
        if (e.name[0] == 0xE5) continue;
        if (e.attr & 0x08)     continue;   // volume label

        if (e.isDirectory()) {
            const uint16_t child = e.getStartCluster();
            if (child >= 2 && visited.insert(child).second) {
                walkDirectory(engine, child, out, visited);
            }
        } else {
            out.push_back(makeFileRecord(e));
        }
    }
}

} // namespace

DiskRecord DiskReader::read(const std::filesystem::path& image_path) {
    DiskRecord rec;
    rec.path     = image_path.string();
    rec.filename = image_path.filename().string();
    rec.format   = detectFormat(image_path);

    Atari::AtariDiskEngine engine;
    const QString qpath = QString::fromStdString(rec.path);
    if (!engine.loadImage(qpath) || !engine.isLoaded()) {
        throw std::runtime_error("engine failed to load " + rec.path);
    }

    // Geometry — derive tracks from totalSectors / (sectorsPerTrack * sides).
    const auto bpb = engine.getBpb();
    rec.bytes_per_sector  = bpb.bytesPerSector;
    rec.sectors_per_track = bpb.sectorsPerTrack;
    rec.sides             = static_cast<uint8_t>(bpb.sides ? bpb.sides : 2);
    if (rec.sectors_per_track > 0 && rec.sides > 0) {
        rec.tracks = static_cast<uint16_t>(
            bpb.totalSectors / (rec.sectors_per_track * rec.sides));
    }
    rec.oem_name = rtrim(qToStd(bpb.oemName));

    // Volume label via our own root-dir parse (engine skips these entries).
    rec.volume_label = readVolumeLabel(engine);

    // File listing — skip if this is a raw-loader boot-sector game (no FAT).
    if (!engine.isRawLoaderDisk()) {
        std::set<uint16_t> visited;
        walkDirectory(engine, 0, rec.files, visited);
    }

    // Cracker-group tag carries over into identification / tagging later.
    const QString group = engine.getGroupName();
    if (!group.isEmpty()) {
        rec.tags.push_back(qToStd(group));
    }
    if (engine.isRawLoaderDisk()) {
        rec.tags.emplace_back("raw-loader");
    }

    return rec;
}

} // namespace manifest
