#include "manifest/MetadataExtractor.hpp"

#include <cstdio>
#include <string>
#include <vector>

using manifest::MetadataExtractor;

namespace {

int failures = 0;

#define CHECK_EQ(a, b) do {                                                    \
    auto _a = (a); auto _b = (b);                                              \
    if (!(_a == _b)) {                                                         \
        std::fprintf(stderr, "FAIL %s:%d  %s == %s  (got \"%s\" vs \"%s\")\n", \
                     __FILE__, __LINE__, #a, #b,                               \
                     std::string(_a).c_str(), std::string(_b).c_str());        \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// Known SHA1 vectors (RFC 3174 / common test strings).
void testKnownSha1Vectors() {
    // empty input
    CHECK_EQ(MetadataExtractor::sha1Hex(nullptr, 0),
             std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));

    const std::string abc = "abc";
    CHECK_EQ(MetadataExtractor::sha1Hex(
                reinterpret_cast<const uint8_t*>(abc.data()), abc.size()),
             std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));

    // "The quick brown fox jumps over the lazy dog"
    const std::string fox = "The quick brown fox jumps over the lazy dog";
    CHECK_EQ(MetadataExtractor::sha1Hex(
                reinterpret_cast<const uint8_t*>(fox.data()), fox.size()),
             std::string("2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"));
}

void testVectorOverload() {
    std::vector<uint8_t> bytes{'a', 'b', 'c'};
    CHECK_EQ(MetadataExtractor::sha1Hex(bytes),
             std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
}

} // namespace

int main() {
    testKnownSha1Vectors();
    testVectorOverload();
    if (failures) {
        std::fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    std::puts("test_metadata: OK");
    return 0;
}
