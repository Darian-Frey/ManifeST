#include "manifest/DiskReader.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace {

int runInspect(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: manifest inspect <image-path>\n");
        return 2;
    }
    try {
        auto rec = manifest::DiskReader::read(argv[2]);
        std::printf("path            %s\n", rec.path.c_str());
        std::printf("filename        %s\n", rec.filename.c_str());
        std::printf("format          %s\n", rec.format.c_str());
        std::printf("volume_label    %s\n", rec.volume_label.c_str());
        std::printf("oem_name        %s\n", rec.oem_name.c_str());
        std::printf("geometry        %u sides / %u tracks / %u spt / %u bps\n",
                    rec.sides, rec.tracks, rec.sectors_per_track, rec.bytes_per_sector);
        std::printf("files (%zu):\n", rec.files.size());
        for (const auto& f : rec.files) {
            std::printf("  %-14s  %8u bytes  (cluster %u)\n",
                        f.filename.c_str(), f.size_bytes, f.start_cluster);
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

} // namespace

// Routes between the GUI and the CLI.
//   manifest                     → GUI
//   manifest --gui               → GUI (explicit)
//   manifest inspect <path>      → dump DiskReader output (diagnostic)
//   manifest scan <folder> ...   → headless scan
//   manifest query ...           → readline query shell
//   manifest launch <id> ...     → one-shot Hatari launch
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "inspect") == 0) {
        return runInspect(argc, argv);
    }

    const bool wants_cli = argc > 1 &&
        (std::strcmp(argv[1], "scan") == 0 ||
         std::strcmp(argv[1], "query") == 0 ||
         std::strcmp(argv[1], "launch") == 0);

    if (wants_cli) {
        std::puts("manifest (cli): not yet implemented");
        return 0;
    }

    std::puts("manifest (gui): not yet implemented");
    return 0;
}
