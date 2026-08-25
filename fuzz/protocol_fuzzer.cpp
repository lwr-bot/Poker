#include "poker/net/length_field_codec.hpp"
#include "poker.pb.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }

    poker::net::LengthFieldCodec codec(64 * 1024);
    const auto chunk_size = std::max<std::size_t>(1, data[0] % 32);
    std::size_t offset = 1;
    while (offset < size) {
        const auto length = std::min(chunk_size, size - offset);
        const auto result = codec.feed(data + offset, length);
        if (!result) {
            break;
        }
        for (const auto& frame : result.frames) {
            poker::protocol::v1::Envelope envelope;
            envelope.ParseFromArray(frame.data(), static_cast<int>(frame.size()));
        }
        offset += length;
    }

    if (size - 1 <= 64 * 1024) {
        poker::protocol::v1::Envelope envelope;
        envelope.ParseFromArray(data + 1, static_cast<int>(size - 1));
    }
    return 0;
}
