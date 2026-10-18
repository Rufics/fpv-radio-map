#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace rfmod {

/// A path resolved outside the directory it had to stay in.
class PathEscapeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A directory that file operations must not leave. Every path the registry renames, removes,
/// writes or loads code from goes through confine() first, so a crafted module id, package path
/// or symlink cannot reach a file outside the registry root.
class ConfinedRoot {
public:
    /// Creates `root` if needed and remembers its canonical form.
    explicit ConfinedRoot(const std::filesystem::path& root);

    [[nodiscard]] const std::filesystem::path& path() const { return root_; }

    /// `candidate` in canonical form (symlinks and ".." resolved; the last parts may not exist
    /// yet). Throws PathEscapeError unless the result lies strictly inside the root.
    [[nodiscard]] std::filesystem::path confine(const std::filesystem::path& candidate) const;

    /// confine(root / relative); `relative` must not be absolute.
    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& relative) const;

private:
    std::filesystem::path root_;
    std::string prefix_;  ///< root_ with a trailing separator: what every confined path starts with
};

}  // namespace rfmod
