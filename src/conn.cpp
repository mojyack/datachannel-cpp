#include <coop/thread.hpp>

#include "conn.hpp"
#include "macros/coop-unwrap.hpp"
#include "macros/logger.hpp"
#include "macros/unwrap.hpp"
#include "util/critical.hpp"
#include "util/variant.hpp"

namespace p2p {
struct CandidateEvent {
    std::string desc;
};

struct ConnectedEvent {
};

struct GatheringDoneEvent {
};

using RemoteEvent = Variant<CandidateEvent, ConnectedEvent, GatheringDoneEvent>;

struct RemoteEvents {
    coop::ThreadEvent                  updated;
    Critical<std::vector<RemoteEvent>> queue;
};

namespace {
auto logger = Logger("p2p");

template <class T>
auto push_event(Connection& self, T data) -> void {
    unwrap_mut(events, self.remote_events);
    {
        auto [lock, queue] = events.queue.access();
        queue.emplace_back(RemoteEvent::create<T>(std::move(data)));
    }
    events.updated.notify();
}

auto on_state_changed(juice_agent_t* const /*agent*/, const juice_state_t state, void* const user_ptr) -> void {
    LOG_DEBUG(logger, "state changed to {}", juice_state_to_string(state));

    auto& self = *std::bit_cast<Connection*>(user_ptr);
    switch(state) {
    case JUICE_STATE_COMPLETED:
        push_event(self, ConnectedEvent());
        break;
    case JUICE_STATE_FAILED:
        self.on_disconnected();
        break;
    default:
        break;
    }
}

auto on_candidate(juice_agent_t* const /*agent*/, const char* const desc, void* const user_ptr) -> void {
    auto& self = *std::bit_cast<Connection*>(user_ptr);
    push_event(self, CandidateEvent(desc));
}

auto on_gathering_done(juice_agent_t* const /*agent*/, void* const user_ptr) -> void {
    auto& self = *std::bit_cast<Connection*>(user_ptr);
    push_event(self, GatheringDoneEvent());
}

auto on_recv(juice_agent_t* const /*agent*/, const char* const data, const size_t size, void* const user_ptr) -> void {
    auto& self = *std::bit_cast<Connection*>(user_ptr);
    self.on_received({(std::byte*)data, size});
}
} // namespace

auto Connection::push_signaling_data(PrependableBuffer buffer) -> coop::Async<bool> {
    coop_unwrap(parsed, net::split_header(buffer.body()));
    const auto [header, _] = parsed;
    if(!co_await parser.callbacks.invoke(header, std::move(buffer)) && header.id != Error::pt) {
        coop_ensure(co_await parser.send_packet(Error{}, header.id));
    }
    co_return true;
}

auto Connection::send_data(const net::BytesRef data) -> SendResult {
    switch(juice_send(agent.get(), (const char*)data.data(), data.size())) {
    case JUICE_ERR_SUCCESS:
        return SendResult::Success;
    case JUICE_ERR_AGAIN:
        return SendResult::WouldBlock;
    case JUICE_ERR_TOO_LARGE:
        return SendResult::MessageTooLarge;
    default:
        return SendResult::UnknownError;
    }
}

auto Connection::connect(Params params) -> coop::Async<bool> {
    constexpr auto ignore_gathering_done = true;

    auto config = juice_config_t{
        .stun_server_host  = params.stun_addr,
        .stun_server_port  = params.stun_port,
        .bind_address      = NULL,
        .cb_state_changed  = on_state_changed,
        .cb_candidate      = on_candidate,
        .cb_gathering_done = !ignore_gathering_done ? on_gathering_done : NULL,
        .cb_recv           = on_recv,
        .user_ptr          = this,
    };
    if(!params.turns.empty()) {
        config.turn_servers       = params.turns.data();
        config.turn_servers_count = params.turns.size();
    }
    agent.reset(juice_create(&config));

    auto remote_desc                                 = std::string();
    auto session_desc_set                            = coop::SingleEvent();
    parser.callbacks.by_type[SessionDescription::pt] = [this, &remote_desc, &session_desc_set](net::Header header, PrependableBuffer buffer) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_unwrap_v_mut(request, (serde::load<net::BinaryFormat, SessionDescription>(buffer.body())));
        remote_desc = std::move(request.desc);
        session_desc_set.notify();
        co_ensure_v(co_await parser.send_packet(Success(), header.id));
        parser.callbacks.by_type.erase(SessionDescription::pt); // oneshot
        co_return true;
    };
    parser.callbacks.by_type[Candidate::pt] = [this](net::Header /*header*/, PrependableBuffer buffer) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_unwrap_v_mut(request, (serde::load<net::BinaryFormat, Candidate>(buffer.body())));
        co_ensure_v(juice_add_remote_candidate(agent.get(), request.desc.data()) == JUICE_ERR_SUCCESS);
        co_return true;
    };
    auto remote_gathering_done                  = coop::SingleEvent();
    parser.callbacks.by_type[GatheringDone::pt] = [this, &remote_gathering_done](net::Header header, PrependableBuffer /*buffer*/) -> coop::Async<bool> {
        constexpr auto error_value = false;
        remote_gathering_done.notify();
        co_ensure_v(co_await parser.send_packet(Success(), header.id));
        parser.callbacks.by_type.erase(SessionDescription::pt); // oneshot
        co_return true;
    };

    coop_ensure(co_await params.start_backend());

    if(!params.controlling) {
        LOG_DEBUG(logger, "waiting for remote session description");
        co_await session_desc_set;
        LOG_DEBUG(logger, "received remote session description: {}", remote_desc);
        coop_ensure(juice_set_remote_description(agent.get(), remote_desc.data()) == JUICE_ERR_SUCCESS);
    }

    auto local_desc = std::array<char, JUICE_MAX_SDP_STRING_LEN>();
    coop_ensure(juice_get_local_description(agent.get(), local_desc.data(), local_desc.size()) == JUICE_ERR_SUCCESS);
    auto local_desc_str = std::string(local_desc.data());
    LOG_DEBUG(logger, "local session description {}", local_desc_str);
    coop_ensure(co_await parser.receive_response<Success>(SessionDescription{std::move(local_desc_str)}));

    if(params.controlling) {
        LOG_DEBUG(logger, "waiting for remote session description");
        co_await session_desc_set;
        LOG_DEBUG(logger, "received remote session description: {}", remote_desc);
        coop_ensure(juice_set_remote_description(agent.get(), remote_desc.data()) == JUICE_ERR_SUCCESS);
    }

    auto events   = std::unique_ptr<RemoteEvents>(new RemoteEvents());
    remote_events = events.get();
    coop_ensure(juice_gather_candidates(agent.get()) == JUICE_ERR_SUCCESS);
    auto connected = false;
    while(!connected) {
        co_await events->updated;
        auto queue = std::exchange(events->queue.access().second, {});
        for(auto& event : queue) {
            switch(event.get_index()) {
            case RemoteEvent::index_of<CandidateEvent>:
                coop_ensure(co_await parser.send_packet(Candidate{std::move(event.as<CandidateEvent>().desc)}));
                break;
            case RemoteEvent::index_of<ConnectedEvent>:
                connected = true;
                break;
            case RemoteEvent::index_of<GatheringDoneEvent>:
                coop_ensure(co_await parser.receive_response<Success>(GatheringDone()));
                break;
            }
        }
    }
    remote_events = nullptr;

    if(!ignore_gathering_done) {
        co_await remote_gathering_done;
    } else {
        parser.callbacks.by_type.erase(GatheringDone::pt);
    }

    co_return true;
}
} // namespace p2p
