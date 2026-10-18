// Corpus replay driver for compilers without libFuzzer: runs LLVMFuzzerTestOneInput once on every
// file of the target's checked-in corpus, fixed at build time (RFMAP_FUZZ_CORPUS). It reads no
// paths from the command line. A crash still stops the program.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
extern "C" __attribute__((weak)) int LLVMFuzzerInitialize(int* argc, char*** argv);

namespace {

void runFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (LLVMFuzzerInitialize) LLVMFuzzerInitialize(&argc, &argv);
    std::size_t runs = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(RFMAP_FUZZ_CORPUS)) {
        if (entry.is_regular_file()) {
            runFile(entry.path());
            ++runs;
        }
    }
    std::printf("replayed %zu inputs from %s\n", runs, RFMAP_FUZZ_CORPUS);
    return runs == 0 ? 1 : 0;
}
