#include <wukong/wkim.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

using namespace wukong;
using namespace std::chrono_literals;

// Stdout is a serialized test-control channel, separate from the real SDK transport.
std::mutex outputMutex;
void emit(Json value) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << value.dump() << std::endl;
}
std::string required(const char* name) {
    const auto* value = std::getenv(name);
    if (!value) throw std::runtime_error("Missing harness environment");
    return value;
}

int main() {
    try {
        Options options;
        options.caFile = required("WKIM_CA_FILE");
        options.connectionTimeout = 3000ms;
        options.requestTimeout = 1200ms;
        options.pingInterval = 500ms;
        options.pongTimeout = 2000ms;
        options.initialReconnectDelay = 500ms;
        options.maxReconnectDelay = 1500ms;
        options.maxReconnectAttempts = 40;
        options.reconnectJitter = false;
        WKIM im(required("WKIM_URL"), {required("WKIM_UID"), required("WKIM_TOKEN")}, options);
        im.on(Event::Connect, [](const Json& value) { emit({{"kind", "connect"}, {"result", value}}); });
        im.on(Event::Disconnect, [](const Json&) { emit({{"kind", "disconnect"}}); });
        im.on(Event::Reconnecting, [](const Json&) { emit({{"kind", "reconnecting"}}); });
        im.on(Event::Error, [](const Json&) { emit({{"kind", "error"}}); });
        im.on(Event::Message, [](const Json& value) { emit({{"kind", "message"}, {"message", value}}); });
        const auto result = im.connect().get();
        emit({{"kind", "ready"}, {"nodeId", result.nodeId}});
        std::string line;
        // Only the application thread waits on futures; SDK callbacks never do so.
        while (std::getline(std::cin, line)) {
            const auto command = Json::parse(line);
            const auto id = command.at("id");
            try {
                const auto kind = command.at("kind").get<std::string>();
                if (kind == "send") {
                    SendOptions sendOptions;
                    sendOptions.clientMsgNo = command.at("clientMsgNo").get<std::string>();
                    const auto ack = im.send(command.at("uid"), ChannelType::Person,
                                             command.at("payload"), sendOptions).get();
                    emit({{"kind", "reply"}, {"id", id}, {"ok", true}, {"ack", {
                        {"messageId", ack.messageId}, {"messageSeq", ack.messageSeq}, {"reasonCode", ack.reasonCode}}}});
                } else if (kind == "stop") {
                    im.destroy().get();
                    emit({{"kind", "reply"}, {"id", id}, {"ok", true},
                          {"destroyed", im.state() == ConnectionState::Destroyed}});
                    return 0;
                } else {
                    throw std::runtime_error("Unknown harness command");
                }
            } catch (const Error& error) {
                emit({{"kind", "reply"}, {"id", id}, {"ok", false}, {"code", error.code()}});
            }
        }
        im.destroy().get();
    } catch (...) {
        // Never print auth data, protocol frames, or arbitrary exception text.
        emit({{"kind", "fatal"}});
        return 1;
    }
}
