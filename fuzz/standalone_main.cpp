// Corpus replay driver for compilers without libFuzzer: runs LLVMFuzzerTestOneInput once on every
// file named on the command line, or in a directory named there. A crash still stops the program.

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
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path arg = argv[i];
        if (arg.string().rfind('-', 0) == 0) continue;  // libFuzzer flags such as -runs=0
        if (std::filesystem::is_directory(arg)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(arg)) {
                if (entry.is_regular_file()) {
                    runFile(entry.path());
                    ++runs;
                }
            }
        } else {
            runFile(arg);
            ++runs;
        }
    }
    std::printf("replayed %zu inputs\n", runs);
    return 0;
}
