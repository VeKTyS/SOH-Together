#pragma once

// Tiny helpers to write/read numbers byte by byte (lowest byte first).
// Every read checks there's enough data left: packets from the network can be
// garbage, we never want to read past the end.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Coop {

class ByteWriter {
  public:
    void U8(uint8_t v) {
        mBytes.push_back(v);
    }
    void U16(uint16_t v) {
        Le(v, 2);
    }
    void U32(uint32_t v) {
        Le(v, 4);
    }
    void U64(uint64_t v) {
        Le(v, 8);
    }
    void Bytes(std::span<const uint8_t> b) {
        mBytes.insert(mBytes.end(), b.begin(), b.end());
    }

    std::vector<uint8_t> Take() {
        return std::move(mBytes);
    }

  private:
    void Le(uint64_t v, int n) {
        for (int i = 0; i < n; i++) {
            mBytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
    std::vector<uint8_t> mBytes;
};

class ByteReader {
  public:
    explicit ByteReader(std::span<const uint8_t> bytes) : mBytes(bytes) {
    }

    bool U8(uint8_t& out) {
        return Le(out, 1);
    }
    bool U16(uint16_t& out) {
        return Le(out, 2);
    }
    bool U32(uint32_t& out) {
        return Le(out, 4);
    }
    bool U64(uint64_t& out) {
        return Le(out, 8);
    }

    bool Bytes(size_t n, std::string& out) {
        if (Remaining() < n) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(mBytes.data() + mPos), n);
        mPos += n;
        return true;
    }

    size_t Remaining() const {
        return mBytes.size() - mPos;
    }

  private:
    template <typename T> bool Le(T& out, size_t n) {
        if (Remaining() < n) {
            return false;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < n; i++) {
            v |= static_cast<uint64_t>(mBytes[mPos + i]) << (8 * i);
        }
        mPos += n;
        out = static_cast<T>(v);
        return true;
    }

    std::span<const uint8_t> mBytes;
    size_t mPos = 0;
};

} // namespace Coop
