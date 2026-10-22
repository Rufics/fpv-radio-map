#pragma once

// Files that change together with the database: written next to their place first, and moved
// into place only once the database has committed, so a failed transaction leaves no file changed.

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// Where the files a server keeps beside its database live: the module root and the trust store.
struct ServerFiles {
    std::filesystem::path modulesRoot;
    std::filesystem::path trustStore;
};

/// The files of one transaction that wait for its commit: installed together after it, removed
/// when it fails.
class StagedFiles {
public:
    /// A staged file sits next to its place under this suffix until it is installed.
    static constexpr std::string_view kSuffix = ".rfmap-restoring";

    /// A file that cannot move into place after the commit. The database has committed by then,
    /// so the files not yet moved stay staged and the message says how to finish by hand.
    class InstallError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    StagedFiles() = default;
    StagedFiles(const StagedFiles&) = delete;
    StagedFiles& operator=(const StagedFiles&) = delete;
    ~StagedFiles();

    /// Writes `bytes` with `mode` next to `target`; it replaces `target` at install(). Writing a
    /// target again replaces what was staged for it. A file that fails half way is removed with the others.
    /// Refuses a target whose staged name another file already has.
    void write(const std::filesystem::path& target, std::string_view bytes, std::filesystem::perms mode);
    /// What `target` will hold after install(): the staged bytes when it is staged, else the file on
    /// disk; nothing when neither exists. A file that exists but cannot be read throws.
    [[nodiscard]] std::optional<std::string> read(const std::filesystem::path& target) const;
    /// Leaves the files on disk: they are no longer this object's to remove.
    void keep() { staged_.clear(); }
    /// Moves every file to its place; each move replaces the old file in one step.
    void install();

private:
    static std::filesystem::path stagedPath(const std::filesystem::path& target);

    std::vector<std::filesystem::path> staged_;
};

}  // namespace rfmap
