#include "net/common.hpp"

namespace p2p {
struct Success {
    constexpr static auto pt = net::PacketType(0x00);
};

struct Error {
    constexpr static auto pt = net::PacketType(0x01);
};

struct SessionDescription {
    constexpr static auto pt = net::PacketType(0x02);

    SerdeFieldsBegin;
    std::string SerdeField(desc);
    SerdeFieldsEnd;
};

struct Candidate {
    constexpr static auto pt = net::PacketType(0x03);

    SerdeFieldsBegin;
    std::string SerdeField(desc);
    SerdeFieldsEnd;
};

struct GatheringDone {
    constexpr static auto pt = net::PacketType(0x04);
};
} // namespace p2p
