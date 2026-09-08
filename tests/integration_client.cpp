#include <wukong/wkim.hpp>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace wukong;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Check failed: " #x); } while (false)
struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<Event, Json>> values;
    void add(Event type, const Json& value) {
        { std::lock_guard<std::mutex> lock(mutex); values.emplace_back(type, value); }
        changed.notify_all();
    }
    Json wait(Event type, std::size_t occurrence = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        Json result;
        CHECK(changed.wait_for(lock, 5s, [&] {
            std::size_t count = 0;
            for (const auto& entry : values) if (entry.first == type && ++count == occurrence) {
                result = entry.second; return true;
            }
            return false;
        }));
        return result;
    }
    std::size_t count(Event type) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t total = 0;
        for (const auto& entry : values) if (entry.first == type) ++total;
        return total;
    }
};
template<class T> T ready(std::future<T> future) {
    CHECK(future.wait_for(5s) == std::future_status::ready);
    return future.get();
}
template<class T> void fails(std::future<T> future, int code) {
    CHECK(future.wait_for(5s) == std::future_status::ready);
    bool failed = false;
    try { future.get(); } catch (const Error& error) { CHECK(error.code() == code); failed = true; }
    CHECK(failed);
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    try {
        const std::string mode = argv[2];
        Options options;
        options.connectionTimeout = 400ms;
        options.requestTimeout = 250ms;
        options.pingInterval = 60ms;
        options.pongTimeout = 100ms;
        options.initialReconnectDelay = 50ms;
        options.maxReconnectDelay = 100ms;
        options.maxReconnectAttempts = 2;
        options.reconnectJitter = false;
        options.maxPendingRequests = 8;
        if (mode == "oversize") options.autoReconnect = false;
        if (mode == "cancel_reconnect") options.pingInterval = 0ms;
        if (mode == "tls_ok" && argc > 3) options.caFile = argv[3];
        if (mode == "tls_host" && argc > 3) options.caFile = argv[3];
        Events events; // Outlives the client and its callback captures.
        auto im = WKIM::init(argv[1], {"alice", "CANARY_TOKEN", "device-alice"}, options);
        for (auto event : {Event::Connect, Event::Disconnect, Event::Message, Event::CustomEvent,
                           Event::Error, Event::SendAck, Event::Reconnecting})
            im->on(event, [&, event](const Json& data) { events.add(event, data); });
        if (mode == "bad_auth" || mode == "bad_connack") {
            fails(im->connect(), 2);
            CHECK(events.count(Event::Connect) == 0);
            CHECK(events.count(Event::Reconnecting) == 0);
        } else if (mode == "connect_timeout" || mode == "upgrade_timeout") {
            fails(im->connect(), static_cast<int>(ErrorCode::Timeout));
        } else if (mode == "tls_untrusted" || mode == "tls_host") {
            fails(im->connect(), static_cast<int>(ErrorCode::Transport));
        } else if (mode == "destroy_connect") {
            auto attempt = im->connect();
            auto stop1 = im->destroy(); auto stop2 = im->destroy();
            ready(std::move(stop1)); ready(std::move(stop2));
            fails(std::move(attempt), static_cast<int>(ErrorCode::Destroyed));
        } else {
            auto first = im->connect(); auto second = im->connect();
            CHECK(ready(std::move(first)).reasonCode == 1);
            CHECK(ready(std::move(second)).reasonCode == 1);
            events.wait(Event::Connect);
            if (mode == "exchange" || mode == "tls_ok") {
                auto result = ready(im->send("bob", ChannelType::Person, {{"type", 1}, {"content", "你好 🌏"}}));
                CHECK(result.messageId == "9007199254740993");
                CHECK(result.messageSeq == 42);
                auto message = events.wait(Event::Message);
                CHECK(message["payload"]["content"] == "你好 🌏");
                CHECK(message["messageId"] == "18446744073709551615");
                CHECK(events.wait(Event::CustomEvent)["data"]["ok"] == true);
                // Server sends this event only after receiving RECVACK and three same-ID pings.
                CHECK(events.wait(Event::CustomEvent, 2)["type"] == "verified");
                CHECK(im->isConnected());
                CHECK(events.count(Event::SendAck) == 1);
            } else if (mode == "send_error") {
                fails(im->send("bob", ChannelType::Person, {{"type", 1}}), 11);
                CHECK(im->isConnected());
                CHECK(events.count(Event::SendAck) == 0);
            } else if (mode == "send_timeout") {
                fails(im->send("bob", ChannelType::Person, {{"type", 1}}), static_cast<int>(ErrorCode::Timeout));
                CHECK(im->isConnected());
            } else if (mode == "pending_disconnect") {
                auto send = im->send("bob", ChannelType::Person, {{"type", 1}});
                ready(im->disconnect());
                fails(std::move(send), static_cast<int>(ErrorCode::Cancelled));
                CHECK(ready(im->connect()).reasonCode == 1);
                CHECK(events.wait(Event::Connect, 2)["reasonCode"] == 1);
            } else if (mode == "heartbeat_timeout" || mode == "uncorrelated_pong" || mode == "reconnect") {
                events.wait(Event::Reconnecting);
                CHECK(events.wait(Event::Connect, 2)["reasonCode"] == 1);
                CHECK(im->isConnected());
            } else if (mode == "reconnect_exhausted") {
                events.wait(Event::Reconnecting, 2);
                for (std::size_t i = 1; i <= 5; ++i) {
                    if (events.wait(Event::Error, i)["code"] == static_cast<int>(ErrorCode::ReconnectExhausted)) break;
                }
                CHECK(im->state() == ConnectionState::Disconnected);
                CHECK(events.count(Event::Reconnecting) == 2);
            } else if (mode == "cancel_reconnect") {
                events.wait(Event::Reconnecting);
                ready(im->disconnect());
                CHECK(im->state() == ConnectionState::Disconnected);
                // Use the server's next fresh connection as a barrier after the cancelled retry timer.
                CHECK(ready(im->connect()).reasonCode == 1);
                CHECK(events.wait(Event::CustomEvent)["type"] == "stable");
                CHECK(events.count(Event::Connect) == 2);
            } else if (mode == "server_disconnect" || mode == "malformed" || mode == "oversize") {
                events.wait(Event::Disconnect);
                CHECK(im->state() == ConnectionState::Disconnected);
                CHECK(events.count(Event::Reconnecting) == 0);
            } else if (mode == "queue_limit") {
                std::promise<void> entered, resume;
                auto release = resume.get_future().share();
                const auto listener = im->on(Event::CustomEvent, [&](const Json&) { entered.set_value(); release.wait(); });
                ready(entered.get_future());
                std::vector<std::future<SendResult>> sends;
                for (int i = 0; i < 8; ++i) sends.push_back(im->send("bob", ChannelType::Person, {{"type", 1}}));
                fails(im->send("bob", ChannelType::Person, {{"type", 1}}), static_cast<int>(ErrorCode::QueueFull));
                auto disconnect = im->disconnect(); // The reserved control slot remains usable under load.
                resume.set_value();
                ready(std::move(disconnect));
                for (auto& send : sends) fails(std::move(send), static_cast<int>(ErrorCode::Cancelled));
                im->off(listener);
            } else if (mode == "listener_cleanup") {
                int removedCalls = 0;
                auto id = im->on(Event::Message, [&](const Json&) { ++removedCalls; });
                im->off(id);
                im->on(Event::Message, [](const Json&) { throw std::runtime_error("CANARY_PAYLOAD"); });
                ready(im->send("bob", ChannelType::Person, {{"type", 1}, {"content", "你好 🌏"}}));
                events.wait(Event::CustomEvent, 2);
                CHECK(removedCalls == 0);
                CHECK(im->isConnected());
            } else if (mode == "destroy_callback") {
                std::promise<void> destroyed;
                im->on(Event::CustomEvent, [&](const Json&) { im.reset(); destroyed.set_value(); });
                ready(destroyed.get_future());
                CHECK(!im);
            }
        }
        if (im) {
            if (im->state() != ConnectionState::Destroyed) ready(im->disconnect());
            ready(im->destroy());
            CHECK(im->state() == ConnectionState::Destroyed);
        }
        std::cout << mode << " passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
