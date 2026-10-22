#include "staged_files.hpp"

#include <algorithm>
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

fs::path StagedFiles::stagedPath(const fs::path& target) { return target.string() + std::string(kSuffix); }

void StagedFiles::write(const fs::path& target, std::string_view bytes, fs::perms mode) {
    const fs::path file = stagedPath(target);
    fs::create_directories(file.parent_path());
    // One entry per target: from here on a failure removes it, and install() moves it once.
    if (std::ranges::find(staged_, file) == staged_.end()) {
        // A file already under the staged name is not ours: a module's own file, or one a failed
        // install left behind. Writing would truncate it, and a failure would remove it.
        if (fs::symlink_status(file).type() != fs::file_type::not_found) {
            throw fs::filesystem_error("a file already has the staged name", file,
                                       std::make_error_code(std::errc::file_exists));
        }
        staged_.push_back(file);
    }
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
    if (!fs::exists(file)) return std::nullopt;
    // file_size throws for what is not a regular file; a short read is an error, not an empty file.
    std::string bytes(fs::file_size(file), '\0');
    std::ifstream in(file, std::ios::binary);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in || in.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw fs::filesystem_error("cannot read", file, std::make_error_code(std::errc::io_error));
    }
    return bytes;
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
