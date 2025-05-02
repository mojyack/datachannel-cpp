#include <coop/task-handle.hpp>
#include <coop/timer.hpp>

#include "macros/coop-assert.hpp"
#include "p2p/conn.hpp"
#include "p2p/net/mock-c2c/client.hpp"
#include "util/span.hpp"

namespace {
auto runner = coop::Runner();

struct Peer {
    p2p::Connection       conn;
    net::mock::MockClient backend;
};

auto pass = false;

auto test() -> coop::Async<void> {
    auto p1 = Peer();
    auto p2 = Peer();

    p1.conn.parser.send_data = [&p1](net::BytesRef data) {
        return p1.backend.send(data);
    };
    p1.backend.on_received = [&p1](net::BytesRef data) -> coop::Async<void> {
        co_await p1.conn.push_signaling_data(data);
    };
    p2.conn.parser.send_data = [&p2](net::BytesRef data) {
        return p2.backend.send(data);
    };
    p2.backend.on_received = [&p2](net::BytesRef data) -> coop::Async<void> {
        co_await p2.conn.push_signaling_data(data);
    };

    auto p1_task    = coop::TaskHandle();
    auto p1_started = coop::SingleEvent();
    runner.push_task(
        p1.conn.connect({
            .start_backend = [&p1, &p1_started] -> coop::Async<bool> {
                p1_started.notify();
                return p1.backend.connect();
            },
            .stun_addr   = "stun.l.google.com",
            .stun_port   = 19302,
            .controlling = false,
        }),
        &p1_task);
    co_await p1_started;
    coop_ensure(co_await p2.conn.connect({
        .start_backend = [&p1, &p2] -> coop::Async<bool> { return p2.backend.connect(p1.backend); },
        .stun_addr     = "stun.l.google.com",
        .stun_port     = 19302,
        .controlling   = true,
    }));
    co_await p1_task.join();

    auto event          = coop::ThreadEvent();
    p1.conn.on_received = [&event](net::BytesRef data) -> void {
        PRINT("p1 received data: {}", from_span(data));
        event.notify();
    };
    p2.conn.on_received = [&event](net::BytesRef data) -> void {
        PRINT("p2 received data: {}", from_span(data));
        event.notify();
    };
    coop_ensure(p1.conn.send_data(to_span("hello p2")) == p2p::SendResult::Success);
    coop_ensure(co_await event == 1);
    coop_ensure(p2.conn.send_data(to_span("hello p1")) == p2p::SendResult::Success);
    coop_ensure(co_await event == 1);
    p1.conn.on_disconnected = [&event]() -> void {
        PRINT("p1 disconnected");
        event.notify();
    };
    // simulate disconnection
    coop_ensure(co_await p2.backend.finish());
    p2.conn.agent.reset();
    PRINT("==== note: this step takes a while ====");
    coop_ensure(co_await event == 1);

    pass = true;
}
} // namespace

auto main() -> int {
    runner.push_task(test());
    runner.run();
    if(pass) {
        std::println("pass");
        return 0;
    } else {
        return -1;
    }
}
