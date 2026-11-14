#pragma once

#include <fstream>
#include <stdexcept>
#include <string>

#include "replay/event_codec.hpp"

namespace lob {

// Appends every event it is given to a binary event log (the same format replay
// reads), so a live session can be replayed deterministically afterwards. Events
// are recorded exactly as received -- including duplicates and out-of-order ones --
// because that is what the pipeline saw. Single-threaded (the consumer thread).
class BinaryEventRecorder {
public:
    explicit BinaryEventRecorder(const std::string& path)
        : out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("cannot open event log for writing: " + path);
        buffer_.resize(1 << 20);
        out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        write_magic(out_);
    }

    void record(const MarketEvent& ev) {
        write_event(out_, ev);
        ++count_;
    }

    void flush() { out_.flush(); }
    [[nodiscard]] size_t count() const noexcept { return count_; }

private:
    std::vector<char> buffer_;
    std::ofstream out_;
    size_t count_ = 0;
};

}  // namespace lob
