#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace wukong {
using Json = nlohmann::json;

enum class ChannelType : int {
    Person = 1, Group = 2, CustomerService = 3, Community = 4,
    CommunityTopic = 5, Info = 6, Data = 7, Temp = 8, Live = 9, Visitors = 10
};
// Wire reason codes shared with the JS SDK; unrecognized future codes remain integers in Error.
enum class ReasonCode : int {
    Unknown = 0, Success = 1, AuthFail = 2, SubscriberNotExist = 3,
    InBlacklist = 4, ChannelNotExist = 5, UserNotOnNode = 6, SenderOffline = 7,
    MsgKeyError = 8, PayloadDecodeError = 9, ForwardSendPacketError = 10,
    NotAllowSend = 11, ConnectKick = 12, NotInWhitelist = 13, QueryTokenError = 14,
    SystemError = 15, ChannelIDError = 16, NodeMatchError = 17, NodeNotMatch = 18,
    Ban = 19, NotSupportHeader = 20, ClientKeyIsEmpty = 21, RateLimit = 22,
    NotSupportChannelType = 23, Disband = 24, SendBan = 25
};
enum class DeviceFlag : int { App = 0, Web = 1, Desktop = 2 };
enum class Event { Connect, Disconnect, Message, Error, SendAck, Reconnecting, CustomEvent };
enum class ConnectionState { Disconnected, Connecting, Connected, Reconnecting, Destroyed };
enum class ErrorCode : int {
    InvalidArgument = -1, NotConnected = -2, Timeout = -3, Transport = -4,
    Protocol = -5, Cancelled = -6, Destroyed = -7, QueueFull = -8,
    ReconnectExhausted = -9
};
using WKIMChannelType = ChannelType;
using WKIMDeviceFlag = DeviceFlag;
using WKIMEvent = Event;

// Local errors use negative codes. Positive server reason codes are preserved.
class Error : public std::runtime_error {
public:
    Error(int code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}
    Error(ErrorCode code, std::string message) : Error(static_cast<int>(code), std::move(message)) {}
    int code() const noexcept { return code_; }
private:
    int code_;
};

struct AuthOptions {
    std::string uid;
    std::string token;
    // Generated once per client if empty; reconnects retain the same device ID.
    std::string deviceId;
    DeviceFlag deviceFlag = DeviceFlag::Desktop;
};

struct Options {
    std::chrono::milliseconds connectionTimeout{10000}; // DNS through successful CONNECT.
    std::chrono::milliseconds requestTimeout{15000};
    std::chrono::milliseconds pingInterval{25000}; // Zero disables application heartbeat.
    std::chrono::milliseconds pongTimeout{10000};
    bool autoReconnect = true; // Only after at least one successful CONNECT.
    unsigned maxReconnectAttempts = 5;
    std::chrono::milliseconds initialReconnectDelay{1000};
    std::chrono::milliseconds maxReconnectDelay{30000};
    bool reconnectJitter = true; // Randomize between half and the capped backoff.
    std::size_t maxPendingRequests = 1024; // Includes commands waiting for the I/O thread.
    std::size_t maxQueuedBytes = 4 * 1024 * 1024; // Includes the active WebSocket write.
    std::size_t maxMessageBytes = 1024 * 1024; // Incoming/outgoing complete wire message.
    // Optional PEM CA bundle, in addition to system roots. Certificate and host checks stay on.
    std::string caFile;
};

struct Header {
    bool noPersist = false;
    bool redDot = true;
    bool syncOnce = false;
    bool dup = false;
};
struct MessageSetting {
    bool receipt = false;
    bool signal = false;
    bool stream = false;
    bool topic = false;
};
struct SendOptions {
    std::string clientMsgNo; // Generated when empty; preserve it for application reconciliation.
    Header header;
    MessageSetting setting;
    std::string topic;
};
struct ConnectResult {
    int reasonCode = 1;
    std::string serverKey;
    std::string salt;
    std::int64_t timeDiff = 0;
    int serverVersion = 0;
    std::uint64_t nodeId = 0;
};
struct SendResult {
    std::string messageId; // Never converted through floating point.
    std::uint64_t messageSeq = 0;
    int reasonCode = 1;
};

// One identity and one I/O thread per client. Public operations are thread-safe;
// event callbacks run serially on that I/O thread. Never wait on SDK futures
// inside a callback; queue work to your application's executor instead.
class WKIM final {
public:
    using Listener = std::function<void(const Json&)>;
    using ListenerId = std::uint64_t;

    WKIM(std::string url, AuthOptions auth, Options options = {});
    ~WKIM();
    WKIM(const WKIM&) = delete;
    WKIM& operator=(const WKIM&) = delete;
    WKIM(WKIM&&) = delete;
    WKIM& operator=(WKIM&&) = delete;

    static std::shared_ptr<WKIM> init(std::string url, AuthOptions auth, Options options = {});
    // Concurrent connect calls join the same bounded authentication attempt.
    std::future<ConnectResult> connect();
    // Completes on SENDACK; failures/timeouts are not retried or queued offline.
    std::future<SendResult> send(std::string channelId, ChannelType channelType,
                                 Json payload, SendOptions options = {});
    // Cancels reconnect, transport, timers and every pending request. Can connect again.
    std::future<void> disconnect();
    // Terminal asynchronous shutdown. Destructor waits for the I/O thread to exit.
    std::future<void> destroy();
    ListenerId on(Event event, Listener listener);
    void off(ListenerId listener);
    ConnectionState state() const noexcept;
    bool isConnected() const noexcept { return state() == ConnectionState::Connected; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    struct Worker;
    std::unique_ptr<Worker> worker_;
};
} // namespace wukong
