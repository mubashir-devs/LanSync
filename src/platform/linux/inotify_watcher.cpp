// Linux inotify implementation of IWatcher (recursive).
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "core/common/logging.hpp"
#include "core/common/path_safety.hpp"
#include "filesystem/watcher/watcher.hpp"

namespace lansync::fsys {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM |
                                IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR |
                                IN_DONT_FOLLOW;

class InotifyWatcher final : public IWatcher {
public:
    ~InotifyWatcher() override { stop(); }

    Result<void> start(const fs::path& root, std::function<void(const WatchEvent&)> cb) override {
        if (thread_.joinable()) return Error{ErrorCode::InvalidArgument, "already started"};
        root_ = root;
        callback_ = std::move(cb);
        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return Error{ErrorCode::IoError, std::string("inotify_init: ") + std::strerror(errno)};
        stopFd_ = ::eventfd(0, EFD_CLOEXEC);
        if (stopFd_ < 0) {
            ::close(fd_);
            fd_ = -1;
            return Error{ErrorCode::IoError, "eventfd failed"};
        }
        if (auto r = addRecursive(root_, ""); !r) {
            cleanup();
            return r;
        }
        thread_ = std::jthread([this] { loop(); });
        return {};
    }

    void stop() override {
        if (thread_.joinable()) {
            const std::uint64_t one = 1;
            [[maybe_unused]] auto n = ::write(stopFd_, &one, sizeof(one));
            thread_.join();
        }
        cleanup();
    }

private:
    void cleanup() {
        if (fd_ >= 0) ::close(fd_);
        if (stopFd_ >= 0) ::close(stopFd_);
        fd_ = stopFd_ = -1;
        wdToPath_.clear();
    }

    Result<void> addWatch(const fs::path& abs, const std::string& rel) {
        const int wd = ::inotify_add_watch(fd_, abs.c_str(), kMask);
        if (wd < 0) {
            if (errno == ENOSPC) {
                return Error{ErrorCode::Busy, "inotify watch limit reached (fs.inotify.max_user_watches)"};
            }
            return Error{ErrorCode::IoError, std::string("inotify_add_watch '") + abs.string() + "': " + std::strerror(errno)};
        }
        wdToPath_[wd] = rel;
        return {};
    }

    Result<void> addRecursive(const fs::path& abs, const std::string& rel) {
        if (auto r = addWatch(abs, rel); !r) {
            if (rel.empty()) return r;   // root must be watchable
            LS_WARN("not watching '{}': {}", abs.string(), r.error().message);
            if (r.error().code == ErrorCode::Busy) limitHit_ = true;
            return {};
        }
        std::error_code ec;
        for (fs::directory_iterator it(abs, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code sec;
            const auto st = it->symlink_status(sec);
            if (sec || !fs::is_directory(st) || fs::is_symlink(st)) continue;
            const std::string childRel = rel.empty() ? it->path().filename().string()
                                                     : rel + "/" + it->path().filename().string();
            if (pathsafety::isInternalPath(childRel)) continue;
            (void)addRecursive(it->path(), childRel);
        }
        return {};
    }

    void emit(WatchEventType t, std::string path, std::string old = {}) {
        if (callback_) callback_(WatchEvent{t, std::move(path), std::move(old)});
    }

    void loop() {
        alignas(inotify_event) char buf[16384];
        struct Pending { std::string path; bool isDir; };
        std::map<std::uint32_t, Pending> movedFrom;   // cookie -> path

        for (;;) {
            pollfd fds[2] = {{fd_, POLLIN, 0}, {stopFd_, POLLIN, 0}};
            const int pr = ::poll(fds, 2, -1);
            if (pr < 0 && errno == EINTR) continue;
            if (pr < 0 || (fds[1].revents & POLLIN)) return;

            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) continue;

            for (char* p = buf; p < buf + n;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(p);
                p += sizeof(inotify_event) + ev->len;

                if (ev->mask & IN_Q_OVERFLOW) {
                    emit(WatchEventType::Overflow, "");
                    continue;
                }
                const auto dir = wdToPath_.find(ev->wd);
                if (ev->mask & IN_IGNORED) {
                    if (dir != wdToPath_.end()) wdToPath_.erase(dir);
                    continue;
                }
                if (dir == wdToPath_.end()) continue;
                if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                    if (dir->second.empty()) emit(WatchEventType::Overflow, "");   // root gone
                    continue;
                }
                if (ev->len == 0) continue;
                const std::string name = ev->name;
                const std::string rel = dir->second.empty() ? name : dir->second + "/" + name;
                if (pathsafety::isInternalPath(rel)) continue;
                const bool isDir = (ev->mask & IN_ISDIR) != 0;

                if (ev->mask & IN_CREATE) {
                    if (isDir) {
                        (void)addRecursive(root_ / rel, rel);
                        emit(WatchEventType::DirCreated, rel);
                    } else {
                        emit(WatchEventType::FileCreated, rel);
                    }
                } else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                    if (!isDir) emit(WatchEventType::FileModified, rel);
                } else if (ev->mask & IN_DELETE) {
                    emit(isDir ? WatchEventType::DirDeleted : WatchEventType::FileDeleted, rel);
                } else if (ev->mask & IN_MOVED_FROM) {
                    movedFrom[ev->cookie] = {rel, isDir};
                } else if (ev->mask & IN_MOVED_TO) {
                    const auto from = movedFrom.find(ev->cookie);
                    if (from != movedFrom.end()) {
                        if (isDir) rewriteWatches(from->second.path, rel);
                        emit(WatchEventType::Renamed, rel, from->second.path);
                        movedFrom.erase(from);
                    } else if (isDir) {   // moved in from outside the tree
                        (void)addRecursive(root_ / rel, rel);
                        emit(WatchEventType::DirCreated, rel);
                    } else {
                        emit(WatchEventType::FileCreated, rel);
                    }
                }
            }
            // A MOVED_FROM with no MOVED_TO in the same batch left the tree.
            for (const auto& [cookie, pend] : movedFrom) {
                emit(pend.isDir ? WatchEventType::DirDeleted : WatchEventType::FileDeleted, pend.path);
            }
            movedFrom.clear();
            if (limitHit_) {
                limitHit_ = false;
                emit(WatchEventType::Overflow, "");   // some dirs are unwatched: force a rescan
            }
        }
    }

    // A directory was renamed: keep watch descriptors pointing at the new names.
    void rewriteWatches(const std::string& from, const std::string& to) {
        for (auto& [wd, path] : wdToPath_) {
            if (path == from) path = to;
            else if (path.size() > from.size() && path.compare(0, from.size(), from) == 0 &&
                     path[from.size()] == '/') path = to + path.substr(from.size());
        }
    }

    fs::path root_;
    std::function<void(const WatchEvent&)> callback_;
    int fd_{-1};
    int stopFd_{-1};
    std::map<int, std::string> wdToPath_;   // only touched by the watcher thread after start
    bool limitHit_{false};
    std::jthread thread_;
};

}  // namespace

std::unique_ptr<IWatcher> makeNativeWatcher() { return std::make_unique<InotifyWatcher>(); }

}  // namespace lansync::fsys
