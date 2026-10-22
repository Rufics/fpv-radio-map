#include "staged_files.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace rfmap {

namespace fs = std::filesystem;

StagedFiles::~StagedFiles() {
    std::error_code ignored;
    for (const auto& file : staged_) fs::remove(file, ignored);
}

fs::path StagedFiles::stagedPath(const fs::path& target) { return target.string() + std::string(kSuffix); }

void StagedFiles::write(const fs::path& target, std::string_view bytes, fs::perms mode) {
    const fs::path file = stagedPath(target);
    fs::create_directories(file.parent_path());
    // One entry per target: from here on a failure removes it, and install() moves it once.
    if (std::ranges::find(staged_, file) == staged_.end()) staged_.push_back(file);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
        if (!out.flush()) {
            throw fs::filesystem_error("cannot write", file, std::make_error_code(std::errc::io_error));
        }
    }
    fs::permissions(file, mode & fs::perms::mask);
}

std::optional<std::string> StagedFiles::read(const fs::path& target) const {
    const fs::path staged = stagedPath(target);
    const fs::path& file = std::ranges::find(staged_, staged) != staged_.end() ? staged : target;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

void StagedFiles::install() {
    for (const auto& file : staged_) {
        const std::string name = file.string();
        std::error_code failed;
        fs::rename(file, name.substr(0, name.size() - kSuffix.size()), failed);
        if (failed) {
            const std::string message =
                std::format("the database has committed, but {} cannot move into place ({}): move the files "
                            "ending in {} into place by hand",
                            name, failed.message(), kSuffix);
            keep();
            throw InstallError(message);
        }
    }
    keep();
}

}  // namespace rfmap
