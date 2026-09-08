#include <wukong/wkim.hpp>
int main() {
    wukong::WKIM im("ws://127.0.0.1:5200", {"alice", "token"});
    im.destroy().get();
}
