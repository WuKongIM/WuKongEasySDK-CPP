#include <wukong/wkim.hpp>
#include "protocol.hpp"
#include "transport.hpp"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <map>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wukong {
namespace asio = boost::asio;
using Timer = asio::steady_timer;
using Completion = std::function<void(std::exception_ptr, Json)>;
namespace {
std::exception_ptr failure(ErrorCode code, const char* message) {
    return std::make_exception_ptr(Error(code, message));
}
template<class T> std::future<T> rejected(std::exception_ptr error) {
    std::promise<T> promise;
    promise.set_exception(error);
    return promise.get_future();
}
void validate(const AuthOptions& auth, const Options& options) {
    const int device = static_cast<int>(auth.deviceFlag);
    if (auth.uid.empty() || device < 0 || device > 2 ||
        options.connectionTimeout.count() <= 0 || options.requestTimeout.count() <= 0 ||
        options.pingInterval.count() < 0 || options.pongTimeout.count() <= 0 ||
        options.initialReconnectDelay.count() <= 0 || options.maxReconnectDelay < options.initialReconnectDelay ||
        options.maxPendingRequests == 0 || options.maxQueuedBytes == 0 || options.maxMessageBytes == 0)
        throw Error(ErrorCode::InvalidArgument, "Invalid authentication or SDK options");
}
} // namespace

struct WKIM::Impl : std::enable_shared_from_this<Impl> {
    struct Pending {
        std::shared_ptr<Timer> timer;
        Completion completion;
    };
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{asio::make_work_guard(io)};
    detail::Endpoint endpoint;
    AuthOptions auth;
    Options options;
    std::atomic<ConnectionState> state{ConnectionState::Disconnected};
    // This mutex fences admission against shutdown; all network state stays on io.
    std::mutex admissionMutex;
    bool shutdownRequested = false, shutdownComplete = false;
    std::vector<std::shared_ptr<std::promise<void>>> shutdownWaiters;
    std::size_t admitted = 0, commandBytes = 0;
    std::mutex listenerMutex;
    std::map<ListenerId, std::pair<Event, Listener>> listeners;
    ListenerId nextListener = 0;
    std::shared_ptr<detail::Transport> transport;
    Timer connectionTimer{io}, heartbeatTimer{io}, reconnectTimer{io};
    // Advancing the generation makes callbacks from replaced transports inert.
    std::uint64_t generation = 0, nextRequest = 0;
    std::unordered_map<std::string, Pending> pending;
    std::vector<std::function<void(std::exception_ptr, ConnectResult)>> connectWaiters;
    ConnectResult connectedResult;
    bool manual = true, hadConnection = false;
    unsigned reconnectAttempt = 0;
    std::mt19937 random{std::random_device{}()};

    Impl(std::string url, AuthOptions identity, Options config)
        : endpoint(detail::parseUrl(url)), auth(std::move(identity)), options(std::move(config)) {
        validate(auth, options);
        if (auth.deviceId.empty()) auth.deviceId = detail::uuid();
    }

    template<class T, class Action>
    std::future<T> submit(std::size_t bytes, Action action, bool control = false) {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        auto self = shared_from_this();
        std::lock_guard<std::mutex> lock(admissionMutex);
        if (shutdownRequested) {
            promise->set_exception(failure(ErrorCode::Destroyed, "SDK has been destroyed"));
            return future;
        }
        if (admitted >= options.maxPendingRequests + (control ? 1 : 0) || bytes > options.maxQueuedBytes - commandBytes) {
            promise->set_exception(failure(ErrorCode::QueueFull, "SDK request budget exhausted"));
            return future;
        }
        ++admitted;
        commandBytes += bytes;
        auto complete = [self, promise](std::exception_ptr error, auto... value) {
            { std::lock_guard<std::mutex> admission(self->admissionMutex); --self->admitted; }
            if (error) promise->set_exception(error);
            else promise->set_value(std::move(value)...);
        };
        asio::post(io, [self, bytes, action = std::move(action), complete]() mutable {
            { std::lock_guard<std::mutex> admission(self->admissionMutex); self->commandBytes -= bytes; }
            action(complete);
        });
        return future;
    }

    // Copy listeners before invoking user code. No SDK mutex is held across a callback.
    void emit(Event event, const Json& value) {
        std::vector<Listener> callbacks;
        {
            std::lock_guard<std::mutex> lock(listenerMutex);
            for (const auto& entry : listeners)
                if (entry.second.first == event) callbacks.push_back(entry.second.second);
        }
        for (const auto& callback : callbacks) {
            try { callback(value); } catch (...) { /* Application exceptions cannot stop transport cleanup. */ }
        }
    }
    void emitError(std::exception_ptr error) {
        try { std::rethrow_exception(error); }
        catch (const Error& e) { emit(Event::Error, {{"code", e.code()}, {"message", e.what()}}); }
        catch (...) { emit(Event::Error, {{"code", static_cast<int>(ErrorCode::Protocol)}, {"message", "SDK operation failed"}}); }
    }
    bool current(std::uint64_t epoch) const { return epoch == generation && transport != nullptr; }

    void connect(std::function<void(std::exception_ptr, ConnectResult)> completion) {
        if (state == ConnectionState::Connected) { completion(nullptr, connectedResult); return; }
        connectWaiters.push_back(std::move(completion));
        if (state == ConnectionState::Connecting || state == ConnectionState::Reconnecting) return;
        manual = false;
        hadConnection = false;
        reconnectAttempt = 0;
        start();
    }
    void finishConnect(std::exception_ptr error) {
        auto waiters = std::move(connectWaiters);
        connectWaiters.clear();
        for (auto& waiter : waiters) waiter(error, connectedResult);
    }
    void start() {
        state = ConnectionState::Connecting;
        const auto epoch = ++generation;
        auto weak = weak_from_this();
        connectionTimer.expires_after(options.connectionTimeout);
        connectionTimer.async_wait([weak, epoch](boost::system::error_code ec) {
            if (auto self = weak.lock(); self && !ec && self->current(epoch))
                self->fail(failure(ErrorCode::Timeout, "Connection timed out"), true);
        });
        try {
            transport = detail::makeTransport(io, endpoint, options, {
                [weak, epoch] {
                    if (auto self = weak.lock(); self && self->current(epoch)) self->authenticate(epoch);
                },
                [weak, epoch](std::string text) {
                    if (auto self = weak.lock(); self && self->current(epoch)) self->receive(text);
                },
                [weak, epoch] {
                    if (auto self = weak.lock(); self && self->current(epoch))
                        self->fail(failure(ErrorCode::Transport, "WebSocket transport closed or failed"), true);
                }
            });
            transport->start();
        } catch (...) {
            fail(failure(ErrorCode::Transport, "Unable to start WebSocket transport"), true);
        }
    }
    void authenticate(std::uint64_t epoch) {
        auto self = shared_from_this();
        request("connect", detail::connectParams(auth), options.connectionTimeout,
            [self, epoch](std::exception_ptr error, Json result) {
                if (!self->current(epoch)) return;
                if (error) { self->fail(error, false); return; }
                try { self->connectedResult = detail::connectResult(result); }
                catch (const Error&) { self->fail(std::current_exception(), false); return; }
                catch (...) { self->fail(failure(ErrorCode::Protocol, "Invalid CONNECT result"), false); return; }
                self->connectionTimer.cancel();
                self->state = ConnectionState::Connected;
                self->hadConnection = true;
                self->reconnectAttempt = 0;
                self->finishConnect(nullptr);
                self->heartbeat(epoch);
                self->emit(Event::Connect, result);
            });
    }
    void request(const std::string& method, Json params, std::chrono::milliseconds timeout, Completion completion) {
        const auto id = std::to_string(++nextRequest);
        std::string text;
        try { text = Json{{"id", id}, {"method", method}, {"params", std::move(params)}}.dump(); }
        catch (...) { completion(failure(ErrorCode::InvalidArgument, "Unable to encode JSON request"), nullptr); return; }
        if (!transport) { completion(failure(ErrorCode::NotConnected, "Not connected"), nullptr); return; }
        if (text.size() > options.maxMessageBytes) {
            completion(failure(ErrorCode::InvalidArgument, "Request exceeds message size limit"), nullptr); return;
        }
        auto timer = std::make_shared<Timer>(io);
        timer->expires_after(timeout);
        pending.emplace(id, Pending{timer, std::move(completion)});
        auto weak = weak_from_this();
        timer->async_wait([weak, id](boost::system::error_code ec) {
            if (auto self = weak.lock(); self && !ec)
                self->completeRequest(id, failure(ErrorCode::Timeout, "Request timed out; delivery may be unknown"), nullptr);
        });
        if (!transport->send(std::move(text)))
            completeRequest(id, failure(ErrorCode::QueueFull, "WebSocket write budget exhausted"), nullptr);
    }
    void completeRequest(const std::string& id, std::exception_ptr error, Json result) {
        const auto found = pending.find(id);
        if (found == pending.end()) return; // Late, duplicate and unknown responses cannot complete another request.
        auto completion = std::move(found->second.completion);
        found->second.timer->cancel();
        pending.erase(found);
        completion(error, std::move(result));
    }
    void receive(const std::string& text) {
        try {
            auto message = detail::parseJson(text);
            if (!message.is_object()) throw Error(ErrorCode::Protocol, "Expected a JSON object");
            if (message.contains("id")) {
                if (!message["id"].is_string() || message.contains("method") ||
                    message.contains("result") == message.contains("error"))
                    throw Error(ErrorCode::Protocol, "Invalid JSON-RPC response");
                const auto id = message["id"].get<std::string>();
                if (!pending.count(id)) return;
                if (message.contains("error")) {
                    const auto& error = message["error"];
                    if (!error.is_object() || !error.contains("code") || !error["code"].is_number_integer())
                        throw Error(ErrorCode::Protocol, "Invalid JSON-RPC error");
                    const auto code = error["code"].get<std::int64_t>();
                    if (code < std::numeric_limits<int>::min() || code > std::numeric_limits<int>::max())
                        throw Error(ErrorCode::Protocol, "Invalid JSON-RPC error code");
                    // The server's free-form text/data may contain secrets. Preserve only its numeric reason.
                    completeRequest(id, std::make_exception_ptr(Error(static_cast<int>(code), "Server rejected operation")), nullptr);
                } else completeRequest(id, nullptr, std::move(message["result"]));
                return;
            }
            if (!message.contains("method") || !message["method"].is_string())
                throw Error(ErrorCode::Protocol, "Invalid JSON-RPC notification");
            const auto method = message["method"].get<std::string>();
            if (method == "disconnect") {
                auto params = message.value("params", Json::object());
                int code = 0;
                if (params.is_object() && params.contains("reasonCode") && params["reasonCode"].is_number_integer())
                    code = params["reasonCode"].get<int>();
                fail(std::make_exception_ptr(Error(code, "Server disconnected this client")), false);
                return;
            }
            if (state != ConnectionState::Connected)
                throw Error(ErrorCode::Protocol, "Notification received before authentication");
            if (method == "recv") {
                auto params = detail::receiveMessage(message.at("params"));
                Json ack = {{"method", "recvack"}, {"params", {
                    {"header", params["header"]}, {"messageId", params["messageId"]}, {"messageSeq", params["messageSeq"]}}}};
                // Queue transport receipt before invoking application callbacks, including callbacks that disconnect.
                if (!transport->send(ack.dump())) {
                    fail(failure(ErrorCode::QueueFull, "Unable to queue receive acknowledgment"), true);
                    return;
                }
                emit(Event::Message, params);
            } else if (method == "event") emit(Event::CustomEvent, detail::customEvent(message.at("params")));
            // Unknown extensions and uncorrelated pong notifications do not satisfy a pending heartbeat.
        } catch (const Error&) { fail(std::current_exception(), false); }
        catch (...) { fail(failure(ErrorCode::Protocol, "Malformed JSON-RPC message"), false); }
    }
    void heartbeat(std::uint64_t epoch) {
        if (options.pingInterval.count() == 0) return;
        auto weak = weak_from_this();
        heartbeatTimer.expires_after(options.pingInterval);
        heartbeatTimer.async_wait([weak, epoch](boost::system::error_code ec) {
            auto self = weak.lock();
            if (!self || ec || !self->current(epoch) || self->state != ConnectionState::Connected) return;
            self->request("ping", Json::object(), self->options.pongTimeout,
                [weak, epoch](std::exception_ptr error, Json) {
                    auto owner = weak.lock();
                    if (!owner || !owner->current(epoch)) return;
                    if (error) owner->fail(error, true);
                    else owner->heartbeat(epoch); // A same-ID result:null is a valid pong.
                });
        });
    }
    // Invalidate first, then cancel resources and complete promises exactly once.
    void clear(std::exception_ptr error) {
        ++generation;
        connectionTimer.cancel(); heartbeatTimer.cancel(); reconnectTimer.cancel();
        auto oldTransport = std::move(transport);
        if (oldTransport) oldTransport->cancel();
        auto requests = std::move(pending);
        pending.clear();
        for (auto& entry : requests) {
            entry.second.timer->cancel();
            entry.second.completion(error, nullptr);
        }
        finishConnect(error);
    }
    void fail(std::exception_ptr error, bool retryable) {
        const bool wasActive = state != ConnectionState::Disconnected && state != ConnectionState::Destroyed;
        state = ConnectionState::Disconnected;
        clear(error);
        emitError(error);
        if (wasActive) emit(Event::Disconnect, {{"reason", "Connection ended"}});
        if (retryable && hadConnection && !manual && options.autoReconnect) scheduleReconnect();
    }
    void scheduleReconnect() {
        if (reconnectAttempt >= options.maxReconnectAttempts) {
            emitError(failure(ErrorCode::ReconnectExhausted, "Reconnect attempts exhausted"));
            return;
        }
        auto delay = options.initialReconnectDelay;
        for (unsigned i = 0; i < reconnectAttempt && delay < options.maxReconnectDelay; ++i)
            delay = delay > options.maxReconnectDelay / 2 ? options.maxReconnectDelay : delay * 2;
        if (options.reconnectJitter) {
            std::uniform_int_distribution<std::int64_t> jitter(std::max<std::int64_t>(1, delay.count() / 2), delay.count());
            delay = std::chrono::milliseconds(jitter(random));
        }
        ++reconnectAttempt;
        state = ConnectionState::Reconnecting;
        emit(Event::Reconnecting, {{"attempt", reconnectAttempt}, {"delay", delay.count()}});
        auto weak = weak_from_this();
        const auto epoch = generation;
        reconnectTimer.expires_after(delay);
        reconnectTimer.async_wait([weak, epoch](boost::system::error_code ec) {
            if (auto self = weak.lock(); self && !ec && !self->manual && self->generation == epoch) self->start();
        });
    }
    void disconnect(bool terminal) {
        const bool wasActive = state != ConnectionState::Disconnected && state != ConnectionState::Destroyed;
        manual = true;
        hadConnection = false;
        state = terminal ? ConnectionState::Destroyed : ConnectionState::Disconnected;
        clear(failure(terminal ? ErrorCode::Destroyed : ErrorCode::Cancelled,
                      terminal ? "SDK has been destroyed" : "Client disconnected"));
        if (wasActive) emit(Event::Disconnect, {{"code", 1000}, {"reason", "Client disconnected"}});
        if (terminal) {
            { std::lock_guard<std::mutex> lock(listenerMutex); listeners.clear(); }
            guard.reset();
        }
    }
    std::future<void> shutdown() {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        std::lock_guard<std::mutex> lock(admissionMutex);
        if (shutdownComplete) { promise->set_value(); return future; }
        shutdownWaiters.push_back(promise);
        if (shutdownRequested) return future;
        shutdownRequested = true;
        auto self = shared_from_this();
        asio::post(io, [self] {
            self->disconnect(true);
            std::vector<std::shared_ptr<std::promise<void>>> waiters;
            {
                std::lock_guard<std::mutex> admission(self->admissionMutex);
                self->shutdownComplete = true;
                waiters = std::move(self->shutdownWaiters);
            }
            for (auto& waiter : waiters) waiter->set_value();
        });
        return future;
    }
};

struct WKIM::Worker { std::thread thread; };
WKIM::WKIM(std::string url, AuthOptions auth, Options options)
    : impl_(std::make_shared<Impl>(std::move(url), std::move(auth), std::move(options))),
      worker_(std::make_unique<Worker>()) {
    worker_->thread = std::thread([impl = impl_] { impl->io.run(); });
}
WKIM::~WKIM() {
    impl_->shutdown();
    if (worker_->thread.joinable()) {
        if (worker_->thread.get_id() == std::this_thread::get_id()) worker_->thread.detach();
        else worker_->thread.join();
    }
}
std::shared_ptr<WKIM> WKIM::init(std::string url, AuthOptions auth, Options options) {
    return std::make_shared<WKIM>(std::move(url), std::move(auth), std::move(options));
}
std::future<ConnectResult> WKIM::connect() {
    auto self = impl_;
    return self->submit<ConnectResult>(0, [self](auto complete) { self->connect(complete); });
}
std::future<SendResult> WKIM::send(std::string channelId, ChannelType channelType, Json payload, SendOptions options) {
    auto self = impl_;
    std::string encoded;
    try {
        encoded = detail::sendParams(channelId, channelType, payload, options).dump();
        if (encoded.size() > self->options.maxMessageBytes)
            throw Error(ErrorCode::InvalidArgument, "Payload exceeds message size limit");
    } catch (const Error&) { return rejected<SendResult>(std::current_exception()); }
    catch (...) { return rejected<SendResult>(failure(ErrorCode::InvalidArgument, "Unable to encode payload")); }
    auto bytes = encoded.size();
    return self->submit<SendResult>(bytes, [self, encoded = std::move(encoded)](auto complete) {
        if (self->state != ConnectionState::Connected) {
            complete(failure(ErrorCode::NotConnected, "Connect before sending"), SendResult{}); return;
        }
        self->request("send", Json::parse(encoded), self->options.requestTimeout,
            [self, complete](std::exception_ptr error, Json result) {
                if (error) { complete(error, SendResult{}); return; }
                try {
                    auto ack = detail::sendResult(result);
                    complete(nullptr, ack);
                    self->emit(Event::SendAck, {{"messageId", ack.messageId}, {"messageSeq", ack.messageSeq}, {"reasonCode", ack.reasonCode}});
                } catch (const Error&) { complete(std::current_exception(), SendResult{}); }
                catch (...) { complete(failure(ErrorCode::Protocol, "Invalid SENDACK result"), SendResult{}); }
            });
    });
}
std::future<void> WKIM::disconnect() {
    auto self = impl_;
    return self->submit<void>(0, [self](auto complete) { self->disconnect(false); complete(nullptr); }, true);
}
std::future<void> WKIM::destroy() { return impl_->shutdown(); }
WKIM::ListenerId WKIM::on(Event event, Listener listener) {
    if (!listener) throw Error(ErrorCode::InvalidArgument, "Listener must not be empty");
    std::lock_guard<std::mutex> admission(impl_->admissionMutex);
    if (impl_->shutdownRequested) throw Error(ErrorCode::Destroyed, "SDK has been destroyed");
    std::lock_guard<std::mutex> lock(impl_->listenerMutex);
    const auto id = ++impl_->nextListener;
    impl_->listeners.emplace(id, std::make_pair(event, std::move(listener)));
    return id;
}
void WKIM::off(ListenerId listener) {
    std::lock_guard<std::mutex> lock(impl_->listenerMutex);
    impl_->listeners.erase(listener);
}
ConnectionState WKIM::state() const noexcept { return impl_->state.load(); }
} // namespace wukong
