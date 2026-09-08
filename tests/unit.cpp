#include "protocol.hpp"
#include <iostream>
#include <set>
#include <stdexcept>
using namespace wukong;
using namespace wukong::detail;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Check failed: " #x); } while (false)
template<class F> void throws(F action) { bool caught = false; try { action(); } catch (...) { caught = true; } CHECK(caught); }
int main() {
    try {
        CHECK(parseUrl("wss://example.com/ws?route=1").target == "/ws?route=1");
        CHECK(parseUrl("ws://localhost?route=1").target == "/?route=1");
        CHECK(parseUrl("wss://[::1]:5200/ws").host == "::1");
        CHECK(parseUrl("wss://[::1]:5200/ws").authority == "[::1]:5200");
        for (auto url : {"http://example.com", "ws://", "ws://user:pass@host", "ws://host:0", "ws://host:65536",
                         "ws://host:", "ws://host/x#y", "ws://host/\r\n", "ws://::1/ws", "ws://[::1"})
            throws([&] { parseUrl(url); });
        CHECK(base64Encode("") == ""); CHECK(base64Encode("f") == "Zg==");
        CHECK(base64Encode("fo") == "Zm8="); CHECK(base64Encode("foo") == "Zm9v");
        throws([] { parseJson(std::string(100, '[') + "0" + std::string(100, ']')); });
        auto recv = receiveMessage({{"message_id", "123"}, {"message_seq", 42}, {"channel_id", "alice"},
            {"channel_type", 1}, {"from_uid", "bob"}, {"timestamp", 0}, {"payload", {{"type", 1}}}});
        CHECK(recv["messageId"] == "123" && recv["header"].is_object() && recv["payload"]["type"] == 1);
        Json payload = {{"type", 1}, {"content", "你好 🌏"}};
        CHECK(decodePayload(base64Encode(payload.dump())) == payload);
        CHECK(decodePayload(payload.dump()) == payload);
        CHECK(decodePayload(payload) == payload);
        CHECK(decodePayload("opaque%%%") == "opaque%%%");
        CHECK(decodePayload("e30=") == Json::object());
        CHECK(decodePayload("e31=") == "e31="); // Reject non-canonical padding bits.
        auto params = sendParams("bob", ChannelType::Person, payload, {});
        CHECK(params["header"]["redDot"] == true);
        CHECK(decodePayload(params["payload"]) == payload);
        SendOptions noRedDot; noRedDot.header.redDot = false; noRedDot.clientMsgNo = "stable";
        CHECK(sendParams("bob", ChannelType::Person, payload, noRedDot)["header"]["redDot"] == false);
        CHECK(sendParams("bob", ChannelType::Person, payload, noRedDot)["clientMsgNo"] == "stable");
        throws([&] { sendParams("", ChannelType::Person, payload, {}); });
        throws([&] { sendParams("bob", ChannelType::Person, nullptr, {}); });
        CHECK(connectParams({"alice", "secret", "device", DeviceFlag::Desktop})["deviceFlag"] == 2);
        auto ack = sendResult(Json::parse(R"({"messageId":18446744073709551615,"messageSeq":42,"reasonCode":1})"));
        CHECK(ack.messageId == "18446744073709551615"); CHECK(ack.messageSeq == 42);
        CHECK(sendResult({{"message_id", "9007199254740993"}, {"message_seq", 1}, {"reason_code", 1}}).messageId == "9007199254740993");
        throws([] { sendResult({{"messageId", "1"}, {"messageSeq", -1}, {"reasonCode", 1}}); });
        throws([] { connectResult({{"reasonCode", 2}}); });
        throws([] { connectResult(Json::object()); });
        auto event = customEvent({{"id", "e1"}, {"type", "changed"}, {"timestamp", 0}, {"data", "{\"ok\":true}"}});
        CHECK(event["data"]["ok"] == true);
        throws([] { customEvent({{"id", ""}, {"type", "changed"}, {"timestamp", 1}, {"data", 1}}); });
        std::set<std::string> ids;
        for (int i = 0; i < 100; ++i) { auto id = uuid(); CHECK(id.size() == 36 && id[14] == '4'); ids.insert(id); }
        CHECK(ids.size() == 100);
        // Captured objects can release other subscriptions when their listener is removed.
        for (bool shutdown : {false, true}) {
            WKIM owner("ws://127.0.0.1:1/ws", {"alice", "token"});
            const auto first = owner.on(Event::Message, [](const Json&) {});
            bool released = false;
            auto capture = std::shared_ptr<int>(new int(0), [&](int* value) {
                delete value;
                owner.off(first);
                released = true;
            });
            const auto second = owner.on(Event::Message, [capture](const Json&) {});
            capture.reset();
            if (shutdown) owner.destroy().get();
            else owner.off(second);
            CHECK(released);
        }
        WKIM client("ws://127.0.0.1:1/ws", {"alice", "token"});
        throws([&] { client.send("bob", ChannelType::Person, payload).get(); });
        client.disconnect().get(); client.destroy().get();
        CHECK(client.state() == ConnectionState::Destroyed);
        throws([&] { client.connect().get(); });
        client.destroy().get();
        std::cout << "Protocol and lifecycle unit tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
