// A stand-in for go-pmtiles in the region tests, so they run without Go: `version` prints a
// version, `extract <in> <out> [flags]` copies <in> to <out> and appends the flags to
// extract.log next to <in>. --maxzoom=0 fails, for the failure path.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class FakePmtiles {
public:
    explicit FakePmtiles(std::vector<std::string> args) : args_(std::move(args)) {}

    int run() const {
        if (args_.size() == 1 && args_[0] == "version") {
            std::cout << "pmtiles 0.0.0-fake, commit test\n";
            return 0;
        }
        if (args_.size() < 3 || args_[0] != "extract") {
            std::cerr << "usage: fake_pmtiles version | extract <in> <out> [flags]\n";
            return 2;
        }
        return extract();
    }

private:
    int extract() const {
        const std::filesystem::path in = args_[1];
        std::ofstream log(in.parent_path() / "extract.log", std::ios::app);
        for (std::size_t i = 3; i < args_.size(); ++i) {
            log << args_[i] << (i + 1 < args_.size() ? " " : "\n");
            if (args_[i] == "--maxzoom=0") {
                std::cerr << "fake failure for maxzoom 0\n";
                return 3;
            }
        }
        std::error_code ec;
        std::filesystem::copy_file(in, args_[2], std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "cannot copy: " << ec.message() << "\n";
            return 1;
        }
        return 0;
    }

    std::vector<std::string> args_;
};

}  // namespace

int main(int argc, char** argv) {
    return FakePmtiles(std::vector<std::string>(argv + 1, argv + argc)).run();
}
