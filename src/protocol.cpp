#include "protocol.hpp"
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <limits>

namespace wukong::detail {
namespace {
[[noreturn]] void invalid() { throw Error(ErrorCode::Protocol, "Invalid protocol message"); }
const Json& field(const Json& value, const char* camel, const char* snake) {
    if (value.contains(camel)) return value.at(camel);
    if (value.contains(snake)) return value.at(snake);
    invalid();
}
std::uint64_t unsignedNumber(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    invalid();
}
std::string messageId(const Json& value) {
    if (value.is_string() && !value.get_ref<const std::string&>().empty()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(unsignedNumber(value));
    invalid();
}
void success(const Json& result) {
    if (!result.is_object()) invalid();
    const auto& reason = field(result, "reasonCode", "reason_code");
    if (!reason.is_number_integer()) invalid();
    const auto code = reason.get<std::int64_t>();
    if (code < 0 || code > std::numeric_limits<int>::max()) invalid();
    if (code != 1) throw Error(static_cast<int>(code), "Server rejected operation");
}
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64Decode(const std::string& text) {
    if (text.size() % 4 != 0) invalid();
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alphabet[i])] = i;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const int a = table[static_cast<unsigned char>(text[i])];
        const int b = table[static_cast<unsigned char>(text[i + 1])];
        const bool pad2 = text[i + 2] == '=', pad3 = text[i + 3] == '=';
        const int c = pad2 ? 0 : table[static_cast<unsigned char>(text[i + 2])];
        const int d = pad3 ? 0 : table[static_cast<unsigned char>(text[i + 3])];
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
            ((pad2 || pad3) && i + 4 != text.size()) || (pad2 && (b & 15)) ||
            (!pad2 && pad3 && (c & 3))) invalid();
        out.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (!pad2) out.push_back(static_cast<char>((b << 4) | (c >> 2)));
        if (!pad3) out.push_back(static_cast<char>((c << 6) | d));
    }
    return out;
}
} // namespace

Endpoint parseUrl(const std::string& url) {
    Endpoint result;
    std::size_t start;
    if (url.rfind("wss://", 0) == 0) { result.tls = true; start = 6; }
    else if (url.rfind("ws://", 0) == 0) start = 5;
    else throw Error(ErrorCode::InvalidArgument, "URL must use ws:// or wss://");
    for (unsigned char c : url)
        if (c <= 32 || c == 127 || c == '#')
            throw Error(ErrorCode::InvalidArgument, "Invalid WebSocket URL");
    const auto end = url.find_first_of("/?", start);
    result.authority = url.substr(start, end == std::string::npos ? end : end - start);
    result.target = end == std::string::npos ? "/" : (url[end] == '?' ? "/" : "") + url.substr(end);
    const auto& authority = result.authority;
    if (authority.empty() || authority.find('@') != std::string::npos)
        throw Error(ErrorCode::InvalidArgument, "Invalid WebSocket authority");
    std::string port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos || close == 1 ||
            (close + 1 != authority.size() && authority[close + 1] != ':'))
            throw Error(ErrorCode::InvalidArgument, "Invalid IPv6 authority");
        result.host = authority.substr(1, close - 1);
        if (close + 1 != authority.size()) port = authority.substr(close + 2);
    } else {
        const auto colon = authority.find(':');
        result.host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
        if (result.host.empty() || result.host.find_first_of("[]") != std::string::npos)
            throw Error(ErrorCode::InvalidArgument, "Invalid host");
    }
    if (!authority.empty() && authority.back() == ':')
        throw Error(ErrorCode::InvalidArgument, "Empty port");
    if (!port.empty()) {
        unsigned numeric = 0;
        auto parsed = std::from_chars(port.data(), port.data() + port.size(), numeric);
        if (parsed.ec != std::errc() || parsed.ptr != port.data() + port.size() || numeric == 0 || numeric > 65535)
            throw Error(ErrorCode::InvalidArgument, "Invalid port");
    }
    result.port = port.empty() ? (result.tls ? "443" : "80") : port;
    return result;
}

std::string uuid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw Error(ErrorCode::Transport, "Unable to generate client identifier");
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[bytes[i] >> 4]; out += hex[bytes[i] & 15];
    }
    return out;
}
std::string base64Encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        auto a = static_cast<unsigned char>(bytes[i]);
        auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out += alphabet[a >> 2]; out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return out;
}
Json parseJson(const std::string& text, bool allowExceptions) {
    return Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) throw Error(ErrorCode::Protocol, "JSON nesting exceeds 64 levels");
        return true;
    }, allowExceptions);
}
Json decodePayload(const Json& payload) {
    if (!payload.is_string()) return payload;
    const auto& text = payload.get_ref<const std::string&>();
    auto direct = parseJson(text, false);
    if (!direct.is_discarded()) return direct;
    try {
        auto decoded = parseJson(base64Decode(text), false);
        if (!decoded.is_discarded()) return decoded;
    } catch (const Error&) {}
    return payload; // Preserve opaque payloads, as the JS SDK does.
}
Json connectParams(const AuthOptions& auth) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return {{"uid", auth.uid}, {"token", auth.token}, {"deviceId", auth.deviceId},
            {"deviceFlag", static_cast<int>(auth.deviceFlag)}, {"clientTimestamp", now}};
}
Json sendParams(const std::string& channel, ChannelType type, const Json& payload, const SendOptions& options) {
    if (channel.empty() || static_cast<int>(type) < 1 || static_cast<int>(type) > 10 ||
        (!payload.is_object() && !payload.is_array()))
        throw Error(ErrorCode::InvalidArgument, "Invalid channel or JSON object/array payload");
    const auto& h = options.header;
    const auto& s = options.setting;
    Json result = {{"channelId", channel}, {"channelType", static_cast<int>(type)},
        {"clientMsgNo", options.clientMsgNo.empty() ? uuid() : options.clientMsgNo},
        {"payload", base64Encode(payload.dump())},
        {"header", {{"noPersist", h.noPersist}, {"redDot", h.redDot}, {"syncOnce", h.syncOnce}, {"dup", h.dup}}},
        {"setting", {{"receipt", s.receipt}, {"signal", s.signal}, {"stream", s.stream}, {"topic", s.topic}}}};
    if (!options.topic.empty()) result["topic"] = options.topic;
    return result;
}
ConnectResult connectResult(const Json& result) {
    success(result);
    ConnectResult out;
    out.serverKey = result.value("serverKey", "");
    out.salt = result.value("salt", "");
    out.timeDiff = result.value("timeDiff", std::int64_t{0});
    out.serverVersion = result.value("serverVersion", 0);
    if (result.contains("nodeId")) out.nodeId = unsignedNumber(result.at("nodeId"));
    return out;
}
SendResult sendResult(const Json& result) {
    success(result);
    return {messageId(field(result, "messageId", "message_id")),
        unsignedNumber(field(result, "messageSeq", "message_seq")), 1};
}
Json receiveMessage(Json params) {
    if (!params.is_object()) invalid();
    params["messageId"] = messageId(field(params, "messageId", "message_id"));
    params["messageSeq"] = unsignedNumber(field(params, "messageSeq", "message_seq"));
    params["channelId"] = field(params, "channelId", "channel_id").get<std::string>();
    params["channelType"] = unsignedNumber(field(params, "channelType", "channel_type"));
    params["fromUid"] = field(params, "fromUid", "from_uid").get<std::string>();
    if (!params.contains("timestamp") || !params.at("timestamp").is_number_integer() || !params.contains("payload")) invalid();
    if (!params.contains("header")) params["header"] = Json::object();
    if (!params["header"].is_object()) invalid();
    params["payload"] = decodePayload(params["payload"]);
    return params;
}
Json customEvent(Json params) {
    if (!params.is_object() || !params.contains("id") || !params["id"].is_string() ||
        params["id"].get_ref<const std::string&>().empty() || !params.contains("type") || !params["type"].is_string() ||
        params["type"].get_ref<const std::string&>().empty() || !params.contains("timestamp") ||
        !params["timestamp"].is_number_integer() || !params.contains("data")) invalid();
    if (params["data"].is_string()) {
        auto parsed = parseJson(params["data"].get_ref<const std::string&>(), false);
        if (!parsed.is_discarded()) params["data"] = std::move(parsed);
    }
    return params;
}
} // namespace wukong::detail
