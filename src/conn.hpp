#pragma once
#include <coop/thread-event.hpp>
#include <juice/juice.h>

#include "net/packet-parser.hpp"
#include "protocol.hpp"

namespace p2p {
struct JuiceAgentDeleter {
    static auto operator()(juice_agent_t* ptr) -> void {
        juice_destroy(ptr);
    }
};
using AutoJuiceAgent = std::unique_ptr<juice_agent_t, JuiceAgentDeleter>;

enum class SendResult {
    Success,
    WouldBlock,
    MessageTooLarge,
    UnknownError,
};

struct RemoteEvents;

struct Connection {
    // private
    net::PacketParser parser;
    AutoJuiceAgent    agent;
    RemoteEvents*     remote_events = nullptr;

    // callbacks
    // be careful that these callbacks are called from another thread
    std::function<void(net::BytesRef)> on_received     = [](net::BytesRef) {};
    std::function<void()>              on_disconnected = [] {};

    auto push_signaling_data(net::BytesRef data) -> coop::Async<bool>;
    auto send_data(net::BytesRef data) -> SendResult;

    struct Params {
        std::span<juice_turn_server_t>     turns = {};
        std::function<coop::Async<bool>()> start_backend;
        const char*                        stun_addr;
        uint16_t                           stun_port;
        bool                               controlling;
    };
    auto connect(Params params) -> coop::Async<bool>;
};
} // namespace p2p
