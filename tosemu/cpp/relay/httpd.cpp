#include "httpd.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <sstream>

#include "common.h"
#include "log.h"
#include "stats.h"

namespace relay {

bool Httpd::start(uint16_t port, const std::string& www_dir, Stats* stats) {
    port_ = port;
    www_ = www_dir;
    stats_ = stats;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
               sizeof(yes));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // local client only
    sa.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
        listen(s, 16) != 0) {
        LOGE("http: cannot bind 127.0.0.1:%u (WSA %d)", port, WSAGetLastError());
        closesocket(s);
        return false;
    }
    srv_ = uintptr_t(s);
    run_ = true;
    th_ = std::thread(&Httpd::accept_loop, this);
    LOGI("http: serving on http://127.0.0.1:%u/ (www=%s)", port,
         www_dir.c_str());
    return true;
}

void Httpd::stop() {
    if (!run_.exchange(false)) return;
    if (srv_ != ~uintptr_t(0)) {
        closesocket(SOCKET(srv_));
        srv_ = ~uintptr_t(0);
    }
    if (th_.joinable()) th_.join();
}

void Httpd::set_serverlist(const std::string& xml) {
    std::lock_guard<std::mutex> lk(m_);
    serverlist_ = xml;
}

void Httpd::set_static_config(const std::string& text) {
    std::lock_guard<std::mutex> lk(m_);
    static_config_ = text;
}

void Httpd::accept_loop() {
    while (run_.load()) {
        SOCKET c = accept(SOCKET(srv_), nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::thread(&Httpd::serve, this, uintptr_t(c)).detach();
    }
}

std::string Httpd::route(const std::string& path, std::string& ctype) {
    ctype = "text/plain";
    std::string name = path;
    if (!name.empty() && name[0] == '/') name.erase(0, 1);
    size_t q = name.find('?');
    if (q != std::string::npos) name.erase(q);
    if (name.empty()) name = "serverlist.xml";
    // No traversal: the client only ever asks for a bare filename.
    if (name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos)
        return std::string();

    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0)
        ctype = "text/xml";

    // The server list is always ours: its ports have to match the ports we
    // are actually listening on, so a stale file in www would break the client
    // in a way that looks like the relay is down.
    if (name == "serverlist.xml") {
        std::lock_guard<std::mutex> lk(m_);
        if (!serverlist_.empty()) return serverlist_;
    }
    std::string body;
    if (!www_.empty() && read_file(join_path(www_, name), body)) return body;
    if (name == "static__Config.txt") {
        std::lock_guard<std::mutex> lk(m_);
        if (!static_config_.empty()) return static_config_;
    }
    return std::string();
}

void Httpd::serve(uintptr_t sock) {
    SOCKET c = SOCKET(sock);
    char buf[4096];
    int n = recv(c, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { closesocket(c); return; }
    buf[n] = 0;

    std::istringstream rq(buf);
    std::string method, path, ver;
    rq >> method >> path >> ver;
    if (stats_) stats_->http_hit();

    std::string ctype;
    std::string body = route(path, ctype);
    std::ostringstream o;
    if (body.empty()) {
        LOGW("http: 404 %s %s", method.c_str(), path.c_str());
        o << "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
             "Connection: close\r\n\r\n";
    } else {
        LOGI("http: 200 %s %s (%u bytes)", method.c_str(), path.c_str(),
             unsigned(body.size()));
        o << "HTTP/1.1 200 OK\r\nContent-Type: " << ctype
          << "\r\nContent-Length: " << body.size()
          << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        if (method != "HEAD") o << body;
    }
    std::string out = o.str();
    size_t off = 0;
    while (off < out.size()) {
        int w = send(c, out.data() + off, int(out.size() - off), 0);
        if (w <= 0) break;
        off += size_t(w);
    }
    shutdown(c, SD_BOTH);
    closesocket(c);
}

}  // namespace relay
