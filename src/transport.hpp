#pragma once
#include "protocol.hpp"
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>

namespace wukong::detail {
// Transport operations and callbacks are confined to the client's I/O thread.
class Transport {
public:
    struct Callbacks {
        std::function<void()> open;
        std::function<void(std::string)> message;
        // Protocol/size violations are terminal; network loss may be retried.
        std::function<void(bool protocolViolation)> failed;
    };
    virtual ~Transport() = default;
    virtual void start() = 0;
    virtual bool send(std::string text) = 0; // False means the byte budget was exhausted.
    virtual void cancel() = 0; // Cancels DNS and socket operations without waiting on the peer.
};
std::shared_ptr<Transport> makeTransport(boost::asio::io_context& io, Endpoint endpoint,
                                        const Options& options, Transport::Callbacks callbacks);
} // namespace wukong::detail
