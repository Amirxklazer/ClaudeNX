#pragma once
#include <string>
#include <vector>
#include <functional>

struct ChatMsg { std::string role, text; };

namespace net {
bool init();
void shutdown();
std::string localIp();
bool startServer(const std::string& key,
                 std::function<void(const std::string& token)> onToken);
void stopServer();
bool serverRunning();
bool chat(const std::string& token, const std::string& model, const std::string& system,
          const std::vector<ChatMsg>& hist, std::string& out, std::string& err);
}
