#include <wukong/wkim.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace wukong;
using namespace std::chrono_literals;

namespace {
std::mutex outputMutex;
void emit(const Json& value) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << value.dump() << std::endl;
}
std::string required(const char* name) {
    const auto* value = std::getenv(name);
    if (!value) throw std::runtime_error("Missing harness environment");
    return value;
}

// A deliberately slow callback holds the I/O thread while application threads
// exercise admission. A deadline prevents a broken test driver from hanging it.
struct CallbackGate {
    std::atomic<bool> armed{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    void release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
    }
    void wait() {
        if (!armed.exchange(false)) return;
        emit({{"kind", "blocked"}});
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return released; }))
            emit({{"kind", "fatal"}});
    }
};

template<class T> T settled(std::future<T>& future) {
    if (future.wait_for(6s) != std::future_status::ready)
        throw std::runtime_error("Future did not settle");
    return future.get();
}
Json outcome(std::future<SendResult>& future) {
    try {
        const auto ack = settled(future);
        return {{"ok", true}, {"ack", {{"messageId", ack.messageId},
            {"messageSeq", ack.messageSeq}, {"reasonCode", ack.reasonCode}}}};
    } catch (const Error& error) {
        return {{"ok", false}, {"code", error.code()}};
    }
}
} // namespace

int main() {
    try {
        Options options;
        options.caFile = required("WKIM_CA_FILE");
        options.connectionTimeout = 3000ms;
        options.requestTimeout = 3000ms;
        options.pingInterval = 1000ms;
        options.pongTimeout = 4000ms;
        options.initialReconnectDelay = 500ms;
        options.maxReconnectDelay = 1500ms;
        options.maxReconnectAttempts = 40;
        options.reconnectJitter = false;
        options.maxPendingRequests = 16;
        CallbackGate gate;
        std::unique_ptr<WKIM> im;
        auto start = [&] {
            im = std::make_unique<WKIM>(required("WKIM_URL"),
                AuthOptions{required("WKIM_UID"), required("WKIM_TOKEN")}, options);
            im->on(Event::Connect, [](const Json& v) { emit({{"kind", "connect"}, {"result", v}}); });
            im->on(Event::Disconnect, [](const Json&) { emit({{"kind", "disconnect"}}); });
            im->on(Event::Reconnecting, [](const Json&) { emit({{"kind", "reconnecting"}}); });
            im->on(Event::Error, [](const Json&) { emit({{"kind", "error"}}); });
            im->on(Event::Message, [&](const Json& v) {
                emit({{"kind", "message"}, {"message", v}});
                gate.wait();
            });
            auto future = im->connect();
            settled(future);
        };
        auto park = [&] {
            if (!im) return;
            auto future = im->destroy();
            settled(future);
            if (im->state() != ConnectionState::Destroyed) throw std::runtime_error("Not destroyed");
            im.reset(); // Join the I/O worker before measuring idle resources.
        };
        start();
        emit({{"kind", "ready"}});
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto cmd = Json::parse(line);
            const auto kind = cmd.at("kind").get<std::string>();
            Json reply = {{"kind", "reply"}, {"id", cmd.at("id")}, {"ok", true}};
            if (kind == "arm") {
                { std::lock_guard<std::mutex> lock(gate.mutex); gate.released = false; }
                gate.armed = true;
            } else if (kind == "send") {
                SendOptions send;
                send.clientMsgNo = cmd.at("clientMsgNo");
                auto future = im->send(cmd.at("uid"), ChannelType::Person, cmd.at("payload"), send);
                reply.update(outcome(future));
            } else if (kind == "burst") {
                const auto& messages = cmd.at("messages");
                if (messages.empty() || messages.size() > 64) throw std::runtime_error("Burst bound");
                std::vector<std::future<SendResult>> futures(messages.size());
                std::vector<std::thread> workers;
                const std::string uid = cmd.at("uid");
                // Each application thread owns disjoint future slots. Joining
                // admissions before opening the gate makes QueueFull deterministic.
                for (std::size_t lane = 0; lane < 8; ++lane) {
                    workers.emplace_back([&, lane] {
                        for (std::size_t i = lane; i < messages.size(); i += 8) {
                            SendOptions send;
                            send.clientMsgNo = messages[i].at("clientMsgNo");
                            futures[i] = im->send(uid, ChannelType::Person, messages[i].at("payload"), send);
                        }
                    });
                }
                for (auto& worker : workers) worker.join();
                gate.release();
                reply["results"] = Json::array();
                for (auto& future : futures) reply["results"].push_back(outcome(future));
            } else if (kind == "recycle") {
                const int count = cmd.at("count");
                if (count < 1 || count > 5) throw std::runtime_error("Lifecycle bound");
                for (int i = 0; i < count; ++i) { park(); start(); }
                reply["cycles"] = count;
            } else if (kind == "park" || kind == "stop") {
                park();
                reply["destroyed"] = true;
            } else {
                throw std::runtime_error("Unknown command");
            }
            emit(reply);
            if (kind == "stop") return 0;
        }
        park();
    } catch (...) {
        emit({{"kind", "fatal"}}); // Never emit credentials or protocol exception text.
        return 1;
    }
}
