#include <wukong/wkim.hpp>
#include <cstdlib>
#include <iostream>
#include <mutex>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: wukong_chat <ws[s]://host:port/ws> <uid> <peer-uid>\n"
                     "Supply WKIM_TOKEN through the environment; /quit exits.\n";
        return 2;
    }
    const char* token = std::getenv("WKIM_TOKEN");
    if (!token) { std::cerr << "WKIM_TOKEN is required\n"; return 2; }
    try {
        std::mutex output;
        wukong::Options options;
        if (const char* ca = std::getenv("WKIM_CA_FILE")) options.caFile = ca;
        wukong::WKIM im(argv[1], {argv[2], token}, options);
        const auto messageListener = im.on(wukong::Event::Message, [&](const wukong::Json& message) {
            std::lock_guard<std::mutex> lock(output);
            // This interactive example renders message content intentionally. The SDK itself never logs it.
            std::cout << "Message: " << message.at("payload").dump() << '\n';
        });
        im.on(wukong::Event::Error, [&](const wukong::Json&) {
            std::lock_guard<std::mutex> lock(output);
            std::cerr << "EasySDK operation failed\n";
        });
        im.on(wukong::Event::Reconnecting, [&](const wukong::Json&) {
            std::lock_guard<std::mutex> lock(output);
            std::cout << "Reconnecting\n";
        });
        im.connect().get();
        { std::lock_guard<std::mutex> lock(output); std::cout << "Connected; enter text or /quit\n"; }
        std::string line;
        while (std::getline(std::cin, line) && line != "/quit") {
            try {
                const auto result = im.send(argv[3], wukong::ChannelType::Person,
                    {{"type", 1}, {"content", line}}).get();
                std::lock_guard<std::mutex> lock(output);
                std::cout << (result.reasonCode == 1 ? "SEND completed\n" : "SEND rejected\n");
            } catch (const wukong::Error&) {
                std::lock_guard<std::mutex> lock(output);
                std::cerr << "SEND failed; reconcile before retrying\n";
            }
        }
        im.off(messageListener);
        im.disconnect().get();
        im.destroy().get();
    } catch (...) { std::cerr << "EasySDK operation failed\n"; return 1; }
}
