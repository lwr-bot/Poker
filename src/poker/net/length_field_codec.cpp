#include "poker/net/length_field_codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace poker::net {

LengthFieldCodec::LengthFieldCodec(std::size_t max_frame_size)
    : max_frame_size_(max_frame_size) {
    if (max_frame_size_ == 0 || max_frame_size_ > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("maximum frame size must fit a positive uint32");
    }
}

std::vector<std::uint8_t> LengthFieldCodec::encode(const std::vector<std::uint8_t>& payload,
                                                    std::size_t max_frame_size) {
    if (payload.empty()) {
        throw std::invalid_argument("empty protocol frames are not allowed");
    }
    if (payload.size() > max_frame_size || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("protocol frame exceeds the configured maximum");
    }

    const auto size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame;
    frame.reserve(header_size + payload.size());
    frame.push_back(static_cast<std::uint8_t>((size >> 24U) & 0xffU));
    frame.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xffU));
    frame.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xffU));
    frame.push_back(static_cast<std::uint8_t>(size & 0xffU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::uint8_t> LengthFieldCodec::encode(const std::string& payload,
                                                    std::size_t max_frame_size) {
    return encode(std::vector<std::uint8_t>(payload.begin(), payload.end()), max_frame_size);
}

DecodeResult LengthFieldCodec::feed(const std::uint8_t* data, std::size_t size) {
    if (failed_) {
        return fail(CodecError::decoder_failed);
    }
    if (size > 0 && data == nullptr) {
        throw std::invalid_argument("non-empty codec input requires a data pointer");
    }
    if (size == 0) {
        return {};
    }
    const auto buffered = buffer_.size() - read_offset_;
    const auto maximum_buffer = max_frame_size_ + header_size;
    if (size > maximum_buffer || buffered > maximum_buffer - size) {
        return fail(CodecError::frame_too_large);
    }
    buffer_.insert(buffer_.end(), data, data + size);

    DecodeResult result;
    while (buffer_.size() - read_offset_ >= header_size) {
        const auto length = peekLength();
        if (length == 0) {
            return fail(CodecError::empty_frame);
        }
        if (length > max_frame_size_) {
            return fail(CodecError::frame_too_large);
        }
        const auto complete_size = header_size + static_cast<std::size_t>(length);
        if (buffer_.size() - read_offset_ < complete_size) {
            break;
        }
        const auto begin = buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_ + header_size);
        const auto end = begin + static_cast<std::ptrdiff_t>(length);
        result.frames.emplace_back(begin, end);
        read_offset_ += complete_size;
    }
    compact();
    return result;
}

DecodeResult LengthFieldCodec::feed(const std::vector<std::uint8_t>& data) {
    return feed(data.data(), data.size());
}

DecodeResult LengthFieldCodec::feed(const std::string& data) {
    return feed(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

void LengthFieldCodec::reset() noexcept {
    buffer_.clear();
    read_offset_ = 0;
    failed_ = false;
}

std::size_t LengthFieldCodec::bufferedBytes() const noexcept {
    return buffer_.size() - read_offset_;
}

std::uint32_t LengthFieldCodec::peekLength() const noexcept {
    const auto* header = buffer_.data() + read_offset_;
    return (static_cast<std::uint32_t>(header[0]) << 24U)
           | (static_cast<std::uint32_t>(header[1]) << 16U)
           | (static_cast<std::uint32_t>(header[2]) << 8U)
           | static_cast<std::uint32_t>(header[3]);
}

void LengthFieldCodec::compact() {
    if (read_offset_ == 0) {
        return;
    }
    if (read_offset_ == buffer_.size()) {
        buffer_.clear();
        read_offset_ = 0;
        return;
    }
    if (read_offset_ >= 4096 || read_offset_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
        read_offset_ = 0;
    }
}

DecodeResult LengthFieldCodec::fail(CodecError error) {
    if (error != CodecError::decoder_failed) {
        failed_ = true;
    }
    return {error, {}};
}

std::string toString(CodecError error) {
    switch (error) {
    case CodecError::none: return "none";
    case CodecError::empty_frame: return "empty frame";
    case CodecError::frame_too_large: return "frame too large";
    case CodecError::decoder_failed: return "decoder failed";
    }
    return "unknown codec error";
}

}  // namespace poker::net
