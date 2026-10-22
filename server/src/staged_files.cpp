#include "staged_files.hpp"

#include <format>
#include <fstream>
#include <string>
#include <system_error>

namespace rfmap {

namespace fs = std::filesystem;

StagedFiles::~StagedFiles() {
    std::error_code ignored;
    for (const auto& file : staged_) fs::remove(file, ignored);
}

void StagedFiles::write(const fs::path& target, std::string_view bytes, fs::perms mode) {
    const fs::path file = target.string() + std::string(kSuffix);
    fs::create_directories(file.parent_path());
    staged_.push_back(file);  // from here on a failure removes it
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
        if (!out.flush()) {
            throw fs::filesystem_error("cannot write", file, std::make_error_code(std::errc::io_error));
        }
    }
    fs::permissions(file, mode & fs::perms::mask);
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
