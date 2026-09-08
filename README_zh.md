# WuKongEasySDK-CPP

[English](README.md)

WuKongIM 的 C++17 轻量通信 SDK，参考
[WuKongEasySDK-JS](https://github.com/WuKongIM/WuKongEasySDK-JS) `v2.0.4`
（`9c03c98c725982fac224cd1d3b52456eae983975`）的接口与 JSON-RPC 协议。
提供 WS/WSS 连接、CONNECT 鉴权、在线消息、自动 RECVACK、心跳、有界重连和自定义事件。

工程版本为 **0.1.0**，支持本仓库维护的 vcpkg Git registry，也可使用源码和 CMake 安装导出；提供 Windows x64、macOS arm64 和 Linux x64 预编译压缩包。

## 下载预编译包，解压接入

从 [Release v0.1.0](https://github.com/WuKongIM/WuKongEasySDK-CPP/releases/tag/v0.1.0)
下载匹配架构和编译器的 ZIP 及 `SHA256SUMS`，验证 SHA-256 后解压。
包内包含 Debug/Release SDK、第三方依赖、许可文件和独立示例，无需另装 vcpkg。
在解压目录执行：

```sh
cmake -S example -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/wukong-sdk.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Windows 请使用 Visual Studio 2022 和 `-A x64`；PowerShell 同样使用 `"$PWD/wukong-sdk.cmake"`。
Linux 包要求 Ubuntu 24.04 x64 / GCC 13，macOS 包面向 macOS 14+ arm64 / Apple Clang；
Windows 包使用 v143 工具集，Release 为 `/MD`，Debug 为 `/MDd`。
分发 Windows 应用时携带复制到可执行文件旁的 OpenSSL DLL，并安装对应 VC++ 运行库。
WSS 请通过 `Options::caFile`（交互示例为 `WKIM_CA_FILE`）提供受维护的 CA 证书包；
不要依赖 OpenSSL 构建机的默认证书路径。

完整内容、验证范围与升级步骤见 [预编译包说明](docs/PREBUILT.md)。
其他架构、编译器、CRT 或依赖组合使用下方的 vcpkg/源码方式。

## 推荐：vcpkg + CMake

先安装 [vcpkg](https://learn.microsoft.com/zh-cn/vcpkg/get_started/get-started)，
将 `VCPKG_ROOT` 指向安装目录。准备 Git、CMake 3.20+ 和 C++17 编译器：
Windows 使用 Visual Studio 2022，macOS 使用 Xcode 命令行工具，Linux 使用 GCC/Clang。
验证使用的 vcpkg 工具版本为 `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`。

在你的应用目录创建 `vcpkg.json`：

```json
{"dependencies": ["wukong-easy-sdk"]}
```

`vcpkg-configuration.json`:

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/WuKongIM/WuKongEasySDK-CPP.git",
      "baseline": "63ec99d34c7605b64e2173d201639042e0e49de9",
      "packages": [
        "wukong-easy-sdk"
      ]
    }
  ]
}
```

在自己的 `main.cpp` 旁添加 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
find_package(WuKongEasySDK 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE WuKongEasySDK::WuKongEasySDK)
```
```sh
# Linux / macOS
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

```powershell
# Windows / Visual Studio 2022
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel 2
```

vcpkg 会自动安装 SDK、Boost、OpenSSL 和 JSON，无需手动逐个安装。
首次构建可能需要编译依赖并耗时数分钟，这不是预编译压缩包。
SDK port 为静态库；Windows 使用 `x64-windows` 时，分发应用需要携带构建时复制到程序旁的依赖 DLL。

这是 WuKongIM 在本仓库维护的公开 Git registry，不是微软默认目录中的包，
因此必须同时提供 registry 配置和依赖声明。SDK 源码固定为
`3e367a908f42385ab9306f9708b7456399cace7d`，与 registry baseline 分别固定。
将两个 JSON 文件提交到应用仓库；已有 manifest 的项目应合并字段，不要覆盖原有依赖。

[独立消费端示例](examples/vcpkg-consumer) · [Registry 维护说明](docs/VCPKG.md)

## 备选：源码编译与接入

需要 C++17、CMake 3.20+、Boost 1.74+、OpenSSL 1.1.1+、nlohmann/json 3.11+。
产品应使用仍受维护并已修补的依赖版本。JSON 优先使用系统安装包，缺失时获取固定 3.11.3 commit；
离线构建设置 `WUKONG_FETCH_JSON=OFF` 并预先安装全部依赖。

```sh
# macOS
brew install cmake boost openssl@3 nlohmann-json
# Ubuntu / Debian
sudo apt-get install g++ cmake libboost-dev libssl-dev nlohmann-json3-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Windows 使用 Visual Studio 2022 和 vcpkg，`VCPKG_ROOT` 指向 vcpkg 安装目录：

```powershell
git -C "$env:VCPKG_ROOT" fetch origin 04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

仓库 manifest 固定 vcpkg baseline。Windows 分发时需要同时携带 vcpkg 动态依赖。

```cmake
add_subdirectory(external/WuKongEasySDK-CPP)
target_link_libraries(my_app PRIVATE WuKongEasySDK::WuKongEasySDK)
```

也可运行 `cmake --install build --config Release --prefix /path/to/sdk-prefix`，
然后在下游 CMake 中使用 `find_package(WuKongEasySDK 0.1 CONFIG REQUIRED)`；
通过 `CMAKE_PREFIX_PATH` 指向安装目录。静态库导出保留其 Boost、OpenSSL、Threads 和 JSON 依赖。

## 连接与发送

业务后端在产品登录后提供 `uid`、短期 `token` 和 Gateway URL；客户端不能调用 Product HTTP 管理接口。
生产环境使用 `wss://`。服务端默认示例的路径是 `/`，只有 listener 或反向代理配置 `/ws` 时才使用该路径。

```cpp
#include <wukong/wkim.hpp>
#include <cstdlib>
#include <iostream>

int main() {
    const char* token = std::getenv("WKIM_TOKEN");
    if (!token) return 2;
    try {
        wukong::WKIM im("ws://127.0.0.1:5200", {"alice", token});
        const auto listener = im.on(wukong::WKIMEvent::Message,
            [](const wukong::Json& message) {
                // 将 message.at("payload") 投递到应用自己的 UI/事件队列。
                (void)message;
            });
        im.connect().get();
        auto ack = im.send("bob", wukong::WKIMChannelType::Person,
                          {{"type", 1}, {"content", "Hello from C++!"}}).get();
        if (ack.reasonCode == 1) std::cout << "SEND completed\n";
        im.off(listener);
        im.disconnect().get();
        im.destroy().get();
    } catch (const wukong::Error&) {
        std::cerr << "EasySDK operation failed\n";
        return 1;
    }
}
```

`connect()` 在 CONNECT 鉴权成功后完成；`send()` 的 future 在 SENDACK 后完成。
发送成功不等于对方收到、展示或完成业务处理。EasySDK 不包含消息数据库、会话、未读、离线恢复、推送、上传或订阅 API。

运行可交互的 Alice/Bob 示例：两个终端分别设置各自后端颁发的 `WKIM_TOKEN`，然后执行：

```sh
./build/wukong_chat ws://127.0.0.1:5200 alice bob
./build/wukong_chat ws://127.0.0.1:5200 bob alice
```

两边都显示 `Connected` 后输入文本，输入 `/quit` 退出。Windows 可执行文件位于 `build/Release/wukong_chat.exe`。
示例主动显示收到的业务内容；SDK 本身不输出日志。

## API 与资源归属

| API | 行为 |
| --- | --- |
| 构造函数 / `WKIM::init(...)` | 每个身份一个独立实例和 I/O 线程，无全局单例 |
| `connect()` | 返回 `future<ConnectResult>`；并发调用复用同一次鉴权尝试 |
| `send(...)` | 返回 `future<SendResult>`；JSON 对象/数组以 UTF-8 Base64 发送，不离线排队或自动重发 |
| `on(Event, callback)` / `off(ListenerId)` | 使用监听器 ID 管理订阅 |
| `disconnect()` | 取消连接、心跳、重连及全部待处理请求；随后允许重新连接 |
| `destroy()` | 终结实例；后续操作拒绝，可重复调用；析构函数等待工作线程退出 |
| `state()` / `isConnected()` | 线程安全状态快照 |

公共操作支持从应用线程调用。事件在单个 I/O 线程串行执行，回调可以发起异步 SDK 操作，
**不能在回调中对 SDK future 调用 `.get()` 或 `.wait()`**。耗时工作和 UI 更新应投递到应用自己的执行器。
阻塞回调也会延迟心跳、超时和资源清理。`off()` 后已被复制准备分发的回调仍可能结束执行；
回调捕获的数据必须活到客户端关闭之后。异常不会逃逸出回调。捕获本实例时使用 `weak_ptr`，避免引用环。

`disconnect()`/`destroy()` 直接取消 socket，不等待对端完成 WebSocket close 握手。
回调内销毁不会等待自己的线程；当前回调和已排队清理结束后线程退出。切换账号、Token 或 URL 时新建实例。

| 事件 | 数据 |
| --- | --- |
| `Connect` | CONNECT result，包括 `reasonCode` |
| `Message` | `header`、字符串 `messageId`、整数 `messageSeq`、秒级 `timestamp`、`channelId`、`channelType`、`fromUid`、已解码 `payload` |
| `SendAck` | `messageId`、`messageSeq`、`reasonCode` |
| `CustomEvent` | `id`、`type`、毫秒级 `timestamp`、`data`；JSON 字符串会解析为 JSON |
| `Reconnecting` | 从 1 开始的 `attempt`，毫秒级 `delay` |
| `Disconnect` / `Error` | 脱敏的断开信息 / 数字 `code` 与脱敏 `message` |

接收时兼容对象、JSON 文本和 Base64 JSON；不认识的字符串保持原样。消息 ID 不经过浮点转换。
自动 RECVACK 携带两个消息标识及原 Header，并在应用回调前排入发送队列，它只代表传输层接收。
未知通知被忽略；畸形协议输入会关闭连接并报告脱敏错误。

## 超时、重连与容量

完整配置见 [`Options`、`Header` 和 `SendOptions`](include/wukong/wkim.hpp)。设备类别为 APP `0`、WEB `1`、PC/Desktop `2`（默认）。
空 `deviceId` 自动生成 UUID 并在重连时保留。`clientMsgNo` 默认生成 UUID；业务对账可主动指定稳定值。
`redDot` 默认 true，显式 false 会被尊重。

- DNS/TCP/TLS/Upgrade/CONNECT 总超时 10 秒，请求超时 15 秒；心跳间隔 25 秒，Pong 超时 10 秒。
- 同 ID 的 `result: null` 可完成心跳；错误 ID 和无关 `pong` 通知不能完成心跳。
- 曾经鉴权成功的连接断开后，最多重试 5 次；指数退避上限 30 秒，默认加入半值到全值之间的抖动。
- 首次连接失败交还调用方；鉴权失败、服务端主动断开、畸形协议、手动断开和销毁停止自动重试。
- 默认最多 1,024 个请求（包含等待 I/O 线程的命令），命令队列与网络写队列各 4 MiB，单帧 1 MiB，接收 JSON 嵌套最多 64 层。
- 请求已满时仍保留一个断开控制槽；容量不足返回 `QueueFull`。
- 超时或断线可能导致发送结果未知，应按 `clientMsgNo` 对账；SDK 不自动重发消息。

WSS 检查证书链和主机名，使用 OpenSSL 系统信任根，最低 TLS 1.2。`Options::caFile` 可补充 PEM CA；没有跳过校验开关。
控制台示例也可读取 `WKIM_CA_FILE`。SDK 不输出 Token、Payload、原始帧、URL、服务端响应正文或底层异常对象。
服务端原因码保留在 `Error::code()`；本地错误使用负数 `ErrorCode`。

## 测试

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
cmake -S . -B build -DWUKONG_INTEGRATION_TESTS=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

可通过 `WUKONG_SANITIZE=ON` 启用 AddressSanitizer/UndefinedBehaviorSanitizer。
WS/WSS 测试使用真实回环 socket 和临时证书。真实产品与 JS 互通测试：

```sh
python3 tests/product.py --server /path/to/wukongim --client build/tests/wukong_product_smoke \
  --js-entry /path/to/WuKongEasySDK-JS/dist/cjs/index.js
```

该测试拥有独立临时单节点集群（256 hash slots），开启 Token 鉴权，由测试后端通过回环 Product HTTP 配置身份，
验证 C++/C++ 双向消息、重连、错误 Token 拒绝、在线清理，以及可选的真实 JS SDK 互通，最后关闭自己的进程。
测试后端的管理操作不属于 SDK。精确版本和证据见 [VALIDATION.md](docs/VALIDATION.md)。

## 许可

[MIT](LICENSE)，第三方依赖保留各自许可。
