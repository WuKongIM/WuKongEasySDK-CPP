#pragma once
#include <wukong/wkim.hpp>

namespace wukong::detail {
struct Endpoint {
    bool tls = false;
    std::string host, port, authority, target;
};
Endpoint parseUrl(const std::string& url);
std::string uuid();
std::string base64Encode(const std::string& bytes);
Json parseJson(const std::string& text, bool allowExceptions = true);
Json decodePayload(const Json& payload);
Json connectParams(const AuthOptions& auth);
Json sendParams(const std::string& channel, ChannelType type, const Json& payload, const SendOptions& options);
ConnectResult connectResult(const Json& result);
SendResult sendResult(const Json& result);
Json receiveMessage(Json params);
Json customEvent(Json params);
} // namespace wukong::detail
