#include "core/transfer/chunk_writer.hpp"

namespace lansync::transfer {

namespace fs = std::filesystem;

namespace {
void restrictPermissions(const fs::path& p) {
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}
}  // namespace

Result<std::unique_ptr<ChunkWriter>> ChunkWriter::createFresh(const fs::path& part,
                                                              std::uint64_t expectedSize,
                                                              std::string expectedHash) {
    std::unique_ptr<ChunkWriter> w(new ChunkWriter(part, expectedSize, std::move(expectedHash)));
    w->out_.open(part, std::ios::binary | std::ios::trunc);
    if (!w->out_) return Error{ErrorCode::IoError, "cannot create part file '" + part.string() + "'"};
    restrictPermissions(part);
    return w;
}

Result<std::unique_ptr<ChunkWriter>> ChunkWriter::resume(const fs::path& part,
                                                         std::uint64_t expectedSize,
                                                         std::string expectedHash) {
    std::error_code ec;
    const auto existing = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (ec || existing == 0 || existing > expectedSize) {
        return createFresh(part, expectedSize, std::move(expectedHash));
    }
    std::unique_ptr<ChunkWriter> w(new ChunkWriter(part, expectedSize, std::move(expectedHash)));
    auto consumed = crypto::hashFilePrefix(part, existing, w->hasher_);
    if (!consumed || *consumed != existing) {
        return createFresh(part, expectedSize, w->expectedHash_);   // unreadable prefix: start over
    }
    w->received_ = existing;
    w->out_.open(part, std::ios::binary | std::ios::app);
    if (!w->out_) return Error{ErrorCode::IoError, "cannot reopen part file '" + part.string() + "'"};
    return w;
}

ChunkWriter::~ChunkWriter() = default;

Result<void> ChunkWriter::write(std::string_view data) {
    if (closed_) return Error{ErrorCode::InvalidArgument, "writer closed"};
    if (received_ + data.size() > expectedSize_) {
        return Error{ErrorCode::ProtocolError, "peer sent more data than announced"};
    }
    out_.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out_) return Error{ErrorCode::IoError, "write to part file failed (disk full?)"};
    hasher_.update(data);
    received_ += data.size();
    return {};
}

Result<void> ChunkWriter::finish() {
    if (closed_) return Error{ErrorCode::InvalidArgument, "writer closed"};
    closed_ = true;
    out_.flush();
    const bool writeOk = static_cast<bool>(out_);
    out_.close();
    if (!writeOk) {
        std::error_code ec;
        fs::remove(part_, ec);
        return Error{ErrorCode::IoError, "flush of part file failed"};
    }
    if (received_ != expectedSize_) {
        return Error{ErrorCode::ProtocolError, "transfer incomplete (" + std::to_string(received_) +
                                                   "/" + std::to_string(expectedSize_) + " bytes)"};
    }
    if (hasher_.finishHex() != expectedHash_) {
        std::error_code ec;
        fs::remove(part_, ec);
        return Error{ErrorCode::SecurityError, "integrity check failed: content hash mismatch"};
    }
    return {};
}

void ChunkWriter::abort(bool deletePart) {
    if (!closed_) {
        closed_ = true;
        out_.close();
    }
    if (deletePart) {
        std::error_code ec;
        fs::remove(part_, ec);
    }
}

}  // namespace lansync::transfer
