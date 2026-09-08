#include "transport.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <deque>
#include <type_traits>

namespace wukong::detail {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Plain = websocket::stream<tcp::socket>;
using Secure = websocket::stream<asio::ssl::stream<tcp::socket>>;

template<bool TLS>
class Session final : public Transport, public std::enable_shared_from_this<Session<TLS>> {
    using Stream = std::conditional_t<TLS, Secure, Plain>;
public:
    Session(asio::io_context& io, Endpoint endpoint, const Options& options, Callbacks callbacks)
        : resolver_(io), context_(asio::ssl::context::tls_client), endpoint_(std::move(endpoint)),
          options_(options), callbacks_(std::move(callbacks)), buffer_(options.maxMessageBytes) {
        if constexpr (TLS) {
            context_.set_default_verify_paths();
            if (!options.caFile.empty()) context_.load_verify_file(options.caFile);
            if (SSL_CTX_set_min_proto_version(context_.native_handle(), TLS1_2_VERSION) != 1)
                throw Error(ErrorCode::Transport, "Unable to configure TLS");
            stream_ = std::make_unique<Stream>(io, context_);
            stream_->next_layer().set_verify_mode(asio::ssl::verify_peer);
            stream_->next_layer().set_verify_callback(asio::ssl::host_name_verification(endpoint_.host));
            if (!SSL_set_tlsext_host_name(stream_->next_layer().native_handle(), endpoint_.host.c_str()))
                throw Error(ErrorCode::Transport, "Unable to configure TLS host");
        } else stream_ = std::make_unique<Stream>(io);
        stream_->read_message_max(options.maxMessageBytes);
        stream_->text(true);
    }
    void start() override {
        auto self = this->shared_from_this();
        resolver_.async_resolve(endpoint_.host, endpoint_.port,
            [self](boost::system::error_code ec, tcp::resolver::results_type results) {
                if (self->stopped_) return;
                if (ec) return self->fail();
                asio::async_connect(beast::get_lowest_layer(*self->stream_), results,
                    [self](boost::system::error_code error, const tcp::endpoint&) {
                        if (self->stopped_) return;
                        if (error) return self->fail();
                        boost::system::error_code ignored;
                        beast::get_lowest_layer(*self->stream_).set_option(tcp::no_delay(true), ignored);
                        self->handshake();
                    });
            });
    }
    bool send(std::string text) override {
        if (stopped_ || !opened_ || text.size() > options_.maxMessageBytes ||
            text.size() > options_.maxQueuedBytes - queuedBytes_) return false;
        queuedBytes_ += text.size();
        queue_.push_back(std::make_shared<std::string>(std::move(text)));
        if (queue_.size() == 1) write();
        return true;
    }
    void cancel() override {
        if (stopped_) return;
        stopped_ = true;
        resolver_.cancel();
        boost::system::error_code ignored;
        auto& socket = beast::get_lowest_layer(*stream_);
        socket.cancel(ignored);
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
        // The active async write captures its buffer independently of this queue.
        queue_.clear();
        queuedBytes_ = 0;
    }
private:
    void fail() {
        if (stopped_) return;
        cancel();
        callbacks_.failed(); // Transport errors deliberately contain no endpoint/token/frame text.
    }
    void handshake() {
        if constexpr (TLS) {
            auto self = this->shared_from_this();
            stream_->next_layer().async_handshake(asio::ssl::stream_base::client,
                [self](boost::system::error_code ec) {
                    if (self->stopped_) return;
                    if (ec) return self->fail();
                    self->upgrade();
                });
        } else upgrade();
    }
    void upgrade() {
        auto self = this->shared_from_this();
        stream_->async_handshake(endpoint_.authority, endpoint_.target,
            [self](boost::system::error_code ec) {
                if (self->stopped_) return;
                if (ec) return self->fail();
                self->opened_ = true;
                self->callbacks_.open();
                if (!self->stopped_) self->read();
            });
    }
    void read() {
        auto self = this->shared_from_this();
        stream_->async_read(buffer_, [self](boost::system::error_code ec, std::size_t) {
            if (self->stopped_) return;
            if (ec) return self->fail();
            auto text = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());
            self->callbacks_.message(std::move(text));
            if (!self->stopped_) self->read();
        });
    }
    void write() {
        auto self = this->shared_from_this();
        auto data = queue_.front();
        stream_->async_write(asio::buffer(*data),
            [self, data](boost::system::error_code ec, std::size_t) {
                if (self->stopped_) return;
                if (ec) return self->fail();
                self->queuedBytes_ -= data->size();
                self->queue_.pop_front();
                if (!self->queue_.empty()) self->write();
            });
    }
    tcp::resolver resolver_;
    asio::ssl::context context_; // Must outlive the TLS stream.
    std::unique_ptr<Stream> stream_;
    Endpoint endpoint_;
    Options options_;
    Callbacks callbacks_;
    beast::flat_buffer buffer_;
    std::deque<std::shared_ptr<std::string>> queue_;
    std::size_t queuedBytes_ = 0;
    bool stopped_ = false, opened_ = false;
};
} // namespace
std::shared_ptr<Transport> makeTransport(boost::asio::io_context& io, Endpoint endpoint,
                                        const Options& options, Transport::Callbacks callbacks) {
    if (endpoint.tls) return std::make_shared<Session<true>>(io, std::move(endpoint), options, std::move(callbacks));
    return std::make_shared<Session<false>>(io, std::move(endpoint), options, std::move(callbacks));
}
} // namespace wukong::detail
