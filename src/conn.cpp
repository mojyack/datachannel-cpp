#include <coop/thread.hpp>

#include "conn.hpp"
#include "macros/coop-assert.hpp"
#include "macros/logger.hpp"
#include "macros/unwrap.hpp"

namespace p2p {
namespace {
auto logger = Logger("p2p");

auto on_state_changed(juice_agent_t* const /*agent*/, const juice_state_t state, void* const user_ptr) -> void {
    LOG_DEBUG(logger, "state changed to {}", juice_state_to_string(state));

    auto& self = *std::bit_cast<Connection*>(user_ptr);
    switch(state) {
    case JUICE_STATE_COMPLETED:
        self.connected.notify();
        break;
    case JUICE_STATE_FAILED:
        self.on_disconnected();
        break;
    default:
        break;
    }
}

auto on_candidate(juice_agent_t* const /*agent*/, const char* const desc, void* const user_ptr) -> void {
    auto& self   = *std::bit_cast<Connection*>(user_ptr);
    auto& runner = *self.injector->runner;
    auto  task   = [&self, desc] -> coop::Async<bool> {
        return self.parser.send_packet(Candidate{desc});
    };
    if(!(self.is_main_thread() ? runner.await(task()) : self.injector->inject_task(task()))) {
        self.on_disconnected();
    }
}

auto on_gathering_done(juice_agent_t* const /*agent*/, void* const user_ptr) -> void {
    auto& self   = *std::bit_cast<Connection*>(user_ptr);
    auto& runner = *self.injector->runner;
    auto  task   = [&self] -> coop::Async<bool> {
        return self.parser.receive_response<Success>(GatheringDone());
    };
    if(!(self.is_main_thread() ? runner.await(task()) : self.injector->inject_task(task()))) {
        self.on_disconnected();
    }
}

auto on_recv(juice_agent_t* const /*agent*/, const char* const data, const size_t size, void* const user_ptr) -> void {
    auto& self = *std::bit_cast<Connection*>(user_ptr);
    self.on_received({(std::byte*)data, size});
}
} // namespace

auto Connection::is_main_thread() const -> bool {
    return main_thread_id == std::this_thread::get_id();
}

auto Connection::push_signaling_data(net::BytesRef data) -> coop::Async<bool> {
    if(const auto p = parser.parse_received(data)) {
        if(!co_await parser.callbacks.invoke(p->header, p->payload) && p->header.id != Error::pt) {
            coop_ensure(co_await parser.send_packet(Error::pt, 0, p->header.id));
        }
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
    injector       = params.injector;
    main_thread_id = std::this_thread::get_id();

    auto remote_desc                                 = std::string();
    auto session_desc_set                            = coop::SingleEvent();
    parser.callbacks.by_type[SessionDescription::pt] = [this, &remote_desc, &session_desc_set](net::Header header, net::BytesRef payload) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_unwrap_v_mut(request, (serde::load<net::BinaryFormat, SessionDescription>(payload)));
        remote_desc = std::move(request.desc);
        session_desc_set.notify();
        co_ensure_v(co_await parser.send_packet(Success(), header.id));
        parser.callbacks.by_type.erase(SessionDescription::pt); // oneshot
        co_return true;
    };
    parser.callbacks.by_type[Candidate::pt] = [this](net::Header /*header*/, net::BytesRef payload) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_unwrap_v_mut(request, (serde::load<net::BinaryFormat, Candidate>(payload)));
        co_ensure_v(juice_add_remote_candidate(agent.get(), request.desc.data()) == JUICE_ERR_SUCCESS);
        co_return true;
    };
    auto remote_gathering_done                  = coop::SingleEvent();
    parser.callbacks.by_type[GatheringDone::pt] = [this, &remote_gathering_done](net::Header header, net::BytesRef /*payload*/) -> coop::Async<bool> {
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

    coop_ensure(juice_gather_candidates(agent.get()) == JUICE_ERR_SUCCESS);
    if(!ignore_gathering_done) {
        co_await remote_gathering_done;
    } else {
        parser.callbacks.by_type.erase(GatheringDone::pt);
    }
    co_await connected;
    co_return true;
}
} // namespace p2p
