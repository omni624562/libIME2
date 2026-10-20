// Runs LLVMFuzzerTestOneInput on saved inputs without libFuzzer, so ctest checks
// them on every build: replay_main <file or folder>...
// A crash fails the test; with -DLIBIME2_FUZZ=ON the same inputs run under ASan.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void run(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

int wmain(int argc, wchar_t** argv) {
    size_t count = 0;
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path path(argv[i]);
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    run(entry.path());
                    ++count;
                }
            }
        }
        else if (std::filesystem::is_regular_file(path)) {
            run(path);
            ++count;
        }
    }
    std::printf("%zu saved inputs replayed\n", count);
    return count > 0 ? 0 : 1;
}
