#include <cstdio>
#include <cstring>

// Routes between the GUI and the CLI query shell.
//   manifest                     → launch GUI
//   manifest --gui               → launch GUI (explicit)
//   manifest scan <folder> ...   → headless scan
//   manifest query ...           → readline query shell
//   manifest launch <id> ...     → one-shot Hatari launch
int main(int argc, char** argv) {
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
