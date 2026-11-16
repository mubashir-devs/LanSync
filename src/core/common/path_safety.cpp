#include "core/common/path_safety.hpp"

namespace lansync::pathsafety {

namespace fs = std::filesystem;

Result<std::string> validateRelativePath(std::string_view rel) {
    auto bad = [](const char* why) { return Error{ErrorCode::SecurityError, std::string("invalid path: ") + why}; };
    if (rel.empty()) return bad("empty");
    if (rel.size() > kMaxPathBytes) return bad("too long");
    if (rel.front() == '/') return bad("absolute");
    if (rel.back() == '/') return bad("trailing slash");
    if (rel.size() >= 2 && rel[1] == ':') return bad("drive prefix");
    for (const char c : rel) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return bad("control character");
        if (c == '\\') return bad("backslash");
    }

    std::size_t start = 0;
    bool first = true;
    while (start <= rel.size()) {
        const auto end = rel.find('/', start);
        const auto comp = rel.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (comp.empty()) return bad("empty component");
        if (comp == "." || comp == "..") return bad("dot component");
        if (comp.size() > kMaxComponentBytes) return bad("component too long");
        if (first && comp == kInternalDir) return bad("reserved name");
        first = false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return std::string(rel);
}

bool isInternalPath(std::string_view rel) {
    return rel == kInternalDir || (rel.size() > kInternalDir.size() &&
                                   rel.substr(0, kInternalDir.size()) == kInternalDir &&
                                   rel[kInternalDir.size()] == '/');
}

Result<fs::path> resolveInside(const fs::path& root, std::string_view rel) {
    auto valid = validateRelativePath(rel);
    if (!valid) return valid.error();

    std::error_code ec;
    const fs::path canonRoot = fs::weakly_canonical(root, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot resolve root: " + ec.message()};

    // Walk component by component: refuse to traverse any existing symlink.
    fs::path current = canonRoot;
    for (const auto& comp : fs::path(*valid)) {
        current /= comp;
        std::error_code lec;
        if (fs::is_symlink(fs::symlink_status(current, lec))) {
            return Error{ErrorCode::SecurityError, "path traverses a symlink"};
        }
    }

    const fs::path resolved = fs::weakly_canonical(current, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot resolve path: " + ec.message()};

    // Component-wise prefix check (not a string prefix: "/a/bc" is not inside "/a/b").
    auto r = resolved.begin();
    for (auto b = canonRoot.begin(); b != canonRoot.end(); ++b, ++r) {
        if (r == resolved.end() || *r != *b) {
            return Error{ErrorCode::SecurityError, "path escapes the sync folder"};
        }
    }
    if (resolved == canonRoot) return Error{ErrorCode::SecurityError, "path is the sync root"};
    return resolved;
}

std::string toRelative(const fs::path& root, const fs::path& absolute) {
    return absolute.lexically_relative(root).generic_string();
}

}  // namespace lansync::pathsafety
