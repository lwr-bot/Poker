#ifndef POKER_NET_LENGTH_FIELD_CODEC_HPP
#define POKER_NET_LENGTH_FIELD_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace poker::net {

enum class CodecError : std::uint8_t {
    none = 0,
    empty_frame,
    frame_too_large,
    decoder_failed,
};

struct DecodeResult {
    CodecError error{CodecError::none};
    std::vector<std::vector<std::uint8_t>> frames;

    explicit operator bool() const noexcept { return error == CodecError::none; }
};

class LengthFieldCodec {
public:
    static constexpr std::size_t header_size = 4;
    static constexpr std::size_t default_max_frame_size = 1024 * 1024;

    explicit LengthFieldCodec(std::size_t max_frame_size = default_max_frame_size);

    static std::vector<std::uint8_t> encode(const std::vector<std::uint8_t>& payload,
                                            std::size_t max_frame_size = default_max_frame_size);
    static std::vector<std::uint8_t> encode(const std::string& payload,
                                            std::size_t max_frame_size = default_max_frame_size);

    DecodeResult feed(const std::uint8_t* data, std::size_t size);
    DecodeResult feed(const std::vector<std::uint8_t>& data);
    DecodeResult feed(const std::string& data);
    void reset() noexcept;
    std::size_t bufferedBytes() const noexcept;

private:
    std::uint32_t peekLength() const noexcept;
    void compact();
    DecodeResult fail(CodecError error);

    std::size_t max_frame_size_;
    std::vector<std::uint8_t> buffer_;
    std::size_t read_offset_{0};
    bool failed_{false};
};

std::string toString(CodecError error);

}  // namespace poker::net

#endif

