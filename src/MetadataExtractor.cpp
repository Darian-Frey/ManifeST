#include "manifest/MetadataExtractor.hpp"

#include "manifest/DiskReader.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace manifest {

namespace {

constexpr std::size_t kSha1Len = 20;

bool isLauncherExtension(const std::string& ext) {
    return ext == "PRG" || ext == "APP" || ext == "TOS";
}

} // namespace

std::string MetadataExtractor::sha1Hex(const uint8_t* data, std::size_t len) {
    std::array<unsigned char, kSha1Len> digest{};
    unsigned int out_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data, len)           != 1 ||
        EVP_DigestFinal_ex(ctx, digest.data(), &out_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA1 digest failed");
    }
    EVP_MD_CTX_free(ctx);

    char hex[kSha1Len * 2 + 1];
    for (std::size_t i = 0; i < kSha1Len; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, kSha1Len * 2);
}

std::string MetadataExtractor::sha1Hex(const std::vector<uint8_t>& bytes) {
    return sha1Hex(bytes.data(), bytes.size());
}

namespace {

void flagLauncher(DiskRecord& record) {
    std::size_t launcher_idx   = 0;
    std::size_t launcher_count = 0;
    for (std::size_t i = 0; i < record.files.size(); ++i) {
        const auto& f = record.files[i];
        if (f.in_root && isLauncherExtension(f.extension)) {
            launcher_idx = i;
            ++launcher_count;
        }
    }
    if (launcher_count == 1) {
        record.files[launcher_idx].is_launcher = true;
    }
}

} // namespace

void MetadataExtractor::enrich(const DiskReader& reader, DiskRecord& record) {
    record.image_hash = sha1Hex(reader.rawImage());
    for (std::size_t i = 0; i < record.files.size(); ++i) {
        auto bytes = reader.readFileBytes(i);
        record.files[i].file_hash = sha1Hex(bytes);
    }
    flagLauncher(record);
}

void MetadataExtractor::enrichImageOnly(const DiskReader& reader, DiskRecord& record) {
    record.image_hash = sha1Hex(reader.rawImage());
    flagLauncher(record);
}

} // namespace manifest
