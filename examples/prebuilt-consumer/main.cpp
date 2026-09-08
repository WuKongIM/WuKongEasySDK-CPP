#include <wukong/wkim.hpp>
#include <iostream>

int main() {
    // Exercise linked SDK code without starting network operations or provisioning credentials.
    wukong::WKIM client("ws://127.0.0.1:5200", {"example", ""});
    client.destroy().get();
    if (client.state() != wukong::ConnectionState::Destroyed) return 1;
    std::cout << "Installed WuKongEasySDK lifecycle passed\n";
}
