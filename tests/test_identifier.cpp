#include "manifest/Identifier.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using manifest::DiskRecord;
using manifest::FileRecord;
using manifest::Identifier;

namespace {

int failures = 0;

#define CHECK(expr) do {                                                       \
    if (!(expr)) {                                                             \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);   \
        ++failures;                                                            \
    }                                                                          \
} while (0)

bool hasTag(const DiskRecord& r, const std::string& t) {
    return std::find(r.tags.begin(), r.tags.end(), t) != r.tags.end();
}

DiskRecord makeRecord(const std::string& filename) {
    DiskRecord r;
    r.filename   = filename;
    r.path       = "/tmp/" + filename;
    r.format     = "ST";
    r.image_hash = "hash-placeholder";
    return r;
}

void testTosecBasic() {
    Identifier id;
    auto r = makeRecord("Dungeon Master (1987)(FTL).st");
    id.identify(r);
    CHECK(r.identified_title.value_or("") == "Dungeon Master");
    CHECK(r.publisher.value_or("")        == "FTL");
    CHECK(r.year.value_or(0)              == 1987);
    CHECK(hasTag(r, "game"));
}

void testTosecWithFlagsAndMultidisk() {
    Identifier id;
    auto r = makeRecord("Another World (1991)(Delphine)(Disk 1 of 2)[cr].ST");
    id.identify(r);
    CHECK(r.identified_title.value_or("") == "Another World");
    CHECK(r.publisher.value_or("")        == "Delphine");
    CHECK(r.year.value_or(0)              == 1991);
    CHECK(hasTag(r, "multidisk-1of2"));
    CHECK(hasTag(r, "cracked"));
    CHECK(hasTag(r, "game"));
}

void testTosecUnknownYear() {
    // "19xx" is a TOSEC placeholder; we must still parse title + publisher
    // but leave year unset.
    Identifier id;
    auto r = makeRecord("Artura (19xx)(Gremlin).st");
    id.identify(r);
    CHECK(r.identified_title.value_or("") == "Artura");
    CHECK(r.publisher.value_or("")        == "Gremlin");
    CHECK(!r.year.has_value());
}

void testVolumeLabelFallback() {
    Identifier id;
    auto r = makeRecord("scene_release_whatever.st");   // no TOSEC pattern
    r.volume_label = "DM1_DISK";
    id.identify(r);
    CHECK(r.identified_title.value_or("") == "DM1 DISK");  // _ → space
    CHECK(!r.publisher.has_value());
}

void testOemJunkRejected() {
    // OEM "NNNNNNÁë" has non-printable bytes — must NOT be accepted as title.
    Identifier id;
    auto r = makeRecord("unknown-dump.st");
    r.volume_label = "";
    r.oem_name     = "NNNNNN\xC1\xEB";
    id.identify(r);
    CHECK(!r.identified_title.has_value());
}

void testLoneLauncherFallback() {
    Identifier id;
    auto r = makeRecord("unlabeled.st");
    FileRecord f;
    f.filename    = "DMBOOT.PRG";
    f.extension   = "PRG";
    f.is_launcher = true;
    r.files.push_back(f);
    id.identify(r);
    CHECK(r.identified_title.value_or("") == "DMBOOT");
}

void testJsonHashLookup() {
    const auto json_path = fs::temp_directory_path() / "manifest-test-tosec.json";
    {
        std::ofstream out(json_path);
        out << R"({
            "abc123": {
                "title": "Dungeon Master",
                "publisher": "FTL",
                "year": 1987,
                "tags": ["game", "rpg"]
            }
        })";
    }

    Identifier id(json_path);
    auto r = makeRecord("rando-hash.st");   // no TOSEC, no label, no launcher
    r.image_hash = "abc123";
    id.identify(r);
    CHECK(r.identified_title.value_or("") == "Dungeon Master");
    CHECK(r.publisher.value_or("")        == "FTL");
    CHECK(r.year.value_or(0)              == 1987);
    CHECK(hasTag(r, "rpg"));

    fs::remove(json_path);
}

void testJsonMissingSilentlySkipped() {
    Identifier id(fs::path("/nonexistent/tosec.json"));
    auto r = makeRecord("unnameable.st");
    id.identify(r);
    CHECK(!r.identified_title.has_value());
}

} // namespace

int main() {
    testTosecBasic();
    testTosecWithFlagsAndMultidisk();
    testTosecUnknownYear();
    testVolumeLabelFallback();
    testOemJunkRejected();
    testLoneLauncherFallback();
    testJsonHashLookup();
    testJsonMissingSilentlySkipped();
    if (failures) {
        std::fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    std::puts("test_identifier: OK");
    return 0;
}
