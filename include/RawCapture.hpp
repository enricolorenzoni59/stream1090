/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

inline std::string jsonString(const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}

// Writes an exclusive, contiguous API-IQ recording before host DSP. The DSP
// consumer (or capture-only consumer) owns all disk I/O, never the SDK callback.
// Slow storage can backpressure acquisition; a subsequent API gap invalidates
// the run instead of silently concatenating discontinuous samples.
class RawCapture {
  public:
    RawCapture(const std::string& path, uint32_t rate, uint64_t limit,
               std::map<std::string, std::string> properties)
        : rate_(rate), limit_(limit), properties_(std::move(properties)) {
        properties_["started_unix_ms"] = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        data_ = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (data_ < 0) throw std::runtime_error("Cannot create capture (will not overwrite): " + path);
        metadata_ = ::open((path + ".json").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (metadata_ < 0) { ::close(data_); data_ = -1; ::unlink(path.c_str());
            throw std::runtime_error("Cannot create capture metadata (will not overwrite): " + path + ".json"); }
        try { writeMetadata(false, "recording"); }
        catch (...) { ::close(data_); ::close(metadata_); throw; }
    }
    ~RawCapture() {
        if (!finished_) { try { finish(false, "aborted"); } catch (...) {} }
        if (data_ >= 0) ::close(data_);
        if (metadata_ >= 0) ::close(metadata_);
    }
    RawCapture(const RawCapture&) = delete;
    RawCapture& operator=(const RawCapture&) = delete;
    bool done() const { return limit_ && samples_ >= limit_; }
    uint64_t samples() const { return samples_; }
    void append(const int16_t* iq, size_t elements) {
        size_t pairs = elements / 2;
        if (limit_) pairs = std::min<uint64_t>(pairs, limit_ - samples_);
        if (!pairs) return;
        if constexpr (std::endian::native == std::endian::little) {
            writeAll(data_, reinterpret_cast<const char*>(iq), pairs * 4);
        } else {
            std::vector<char> bytes(pairs * 4);
            for (size_t n = 0; n < pairs * 2; ++n) {
                const uint16_t v = uint16_t(iq[n]); bytes[2*n] = char(v); bytes[2*n+1] = char(v >> 8);
            }
            writeAll(data_, bytes.data(), bytes.size());
        }
        samples_ += pairs;
    }
    void update(const std::map<std::string, std::string>& values) {
        for (const auto& [k,v] : values) properties_[k] = v;
    }
    // The producer stops forwarding at the first discontinuity. A fully written
    // bounded prefix therefore remains contiguous even if acquisition fails
    // later, while the consumer/driver is stopping. Keep that failure in metadata.
    bool finishSession(bool healthy) {
        const bool valid = done() || (!limit_ && healthy);
        finish(valid, valid ? (healthy ? "complete" : "complete_before_device_failure")
                            : (healthy ? "interrupted" : "device_failure_or_discontinuity"));
        return valid;
    }
    void finish(bool valid, const std::string& reason) {
        if (finished_) return;
        if (::fsync(data_) != 0) { valid = false; writeMetadata(false, "disk_sync_failed");
            throw std::runtime_error("Capture fsync failed"); }
        writeMetadata(valid, reason);
        if (::fsync(metadata_) != 0) throw std::runtime_error("Capture metadata fsync failed");
        finished_ = true;
    }
  private:
    static void writeAll(int fd, const char* bytes, size_t n) {
        while (n) {
            const ssize_t written = ::write(fd, bytes, n);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("Capture write failed (disk full or I/O error)");
            bytes += written; n -= size_t(written);
        }
    }
    void writeMetadata(bool valid, const std::string& reason) {
        std::ostringstream out;
        out << "{\n  \"schema\": 1,\n  \"format\": \"cs16\",\n  \"endianness\": \"little\",\n"
               "  \"layout\": \"interleaved_iq\",\n  \"scale\": 32768,\n"
               "  \"stage\": \"API output before host FIR and Q14 conversion\",\n"
            << "  \"sample_rate\": " << rate_ << ",\n  \"samples\": " << samples_
            << ",\n  \"requested_samples\": " << limit_ << ",\n  \"valid\": " << (valid ? "true" : "false")
            << ",\n  \"reason\": " << jsonString(reason) << ",\n  \"properties\": {";
        bool first = true;
        for (const auto& [k,v] : properties_) {
            if (!first) out << ',';
            first = false; out << "\n    " << jsonString(k) << ": " << jsonString(v);
        }
        out << "\n  }\n}\n";
        if (::lseek(metadata_, 0, SEEK_SET) < 0 || ::ftruncate(metadata_, 0) != 0)
            throw std::runtime_error("Cannot update capture metadata");
        const auto text = out.str(); writeAll(metadata_, text.data(), text.size());
    }
    int data_ = -1, metadata_ = -1;
    uint32_t rate_;
    uint64_t limit_, samples_ = 0;
    std::map<std::string, std::string> properties_;
    bool finished_ = false;
};
