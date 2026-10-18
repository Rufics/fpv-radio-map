#include "rfmod/confined_root.hpp"

namespace rfmod {
namespace fs = std::filesystem;

ConfinedRoot::ConfinedRoot(const fs::path& root) {
    fs::create_directories(root);
    root_ = fs::canonical(root);
    prefix_ = (root_ / "").string();
}

fs::path ConfinedRoot::confine(const fs::path& candidate) const {
    fs::path resolved = fs::weakly_canonical(candidate);
    if (!resolved.string().starts_with(prefix_)) {
        throw PathEscapeError(candidate.string() + " is outside " + root_.string());
    }
    return resolved;
}

fs::path ConfinedRoot::resolve(const fs::path& relative) const {
    if (relative.is_absolute()) throw PathEscapeError(relative.string() + " is absolute");
    return confine(root_ / relative);
}

}  // namespace rfmod
