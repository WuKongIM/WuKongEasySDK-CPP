#include <wukong/wkim.hpp>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>
using namespace wukong;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Check failed: " #x); } while (false)
struct Inbox {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Json> messages;
    void push(const Json& message) {
        { std::lock_guard<std::mutex> lock(mutex); messages.push_back(message); }
        changed.notify_all();
    }
    Json wait(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, 10s, [&] { return messages.size() >= count; }));
        return messages.at(count - 1);
    }
};
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const char* aliceToken = std::getenv("WKIM_ALICE_TOKEN");
    const char* bobToken = std::getenv("WKIM_BOB_TOKEN");
    if (!aliceToken || !bobToken) return 2;
    try {
        Options options;
        options.pingInterval = 100ms;
        Inbox aliceInbox, bobInbox;
        WKIM alice(argv[1], {"cpp-alice", aliceToken, "cpp-alice-device"}, options);
        alice.on(Event::Message, [&](const Json& value) { aliceInbox.push(value); });
        CHECK(alice.connect().get().reasonCode == 1);
        if (std::string(argv[2]) == "js") {
            Json payload = {{"type", 1}, {"content", "C++ 与 JS 双向消息 🌏"}};
            CHECK(alice.send("cpp-bob", ChannelType::Person, payload).get().reasonCode == 1);
            auto reply = aliceInbox.wait(1);
            CHECK(reply["fromUid"] == "cpp-bob");
            CHECK(reply["payload"] == payload);
            alice.disconnect().get();
            std::cout << "C++/JavaScript SDK bidirectional messaging passed\n";
            return 0;
        }
        WKIM bob(argv[1], {"cpp-bob", bobToken, "cpp-bob-device"}, options);
        bob.on(Event::Message, [&](const Json& value) { bobInbox.push(value); });
        CHECK(bob.connect().get().reasonCode == 1);
        Json payload = {{"type", 1}, {"content", "C++ Alice → Bob 你好 🌏"}};
        auto ack = alice.send("cpp-bob", ChannelType::Person, payload).get();
        auto received = bobInbox.wait(1);
        CHECK(received["payload"] == payload && received["fromUid"] == "cpp-alice");
        CHECK(received["messageId"] == ack.messageId && received["messageSeq"] == ack.messageSeq);
        auto reply = Json{{"type", 1}, {"content", "Bob → Alice"}};
        CHECK(bob.send("cpp-alice", ChannelType::Person, reply).get().reasonCode == 1);
        CHECK(aliceInbox.wait(1)["payload"] == reply);
        bob.disconnect().get();
        CHECK(!bob.isConnected());
        bob.connect().get();
        Json reconnected = {{"type", 1}, {"content", "reconnected"}};
        alice.send("cpp-bob", ChannelType::Person, reconnected).get();
        CHECK(bobInbox.wait(2)["payload"] == reconnected);
        WKIM rejected(argv[1], {"cpp-alice", "wrong-token", "wrong-token-device"}, options);
        bool authRejected = false;
        try { rejected.connect().get(); }
        catch (const Error& error) { authRejected = error.code() == 2; }
        CHECK(authRejected);
        alice.disconnect().get(); bob.disconnect().get();
        std::cout << "C++/C++ messaging, SENDACK, reconnect and invalid-token rejection passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
