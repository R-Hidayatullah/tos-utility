// A four-route HTTP server, because patching client.xml is only half the job.
//
// The client does not read a server list off disk -- it fetches ServerListURL
// over HTTP and takes the addresses from there. Repointing that URL at
// ourselves is what actually moves the client onto the relay, so the relay has
// to be able to answer it. Serving the static config from the same place also
// makes every file the client asks for show up in the log, which is how you
// find out what else it wanted.
#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace relay {

class Stats;

class Httpd {
public:
    // Files under `www_dir` are served if present; otherwise the built-in
    // generated serverlist/static config answer.
    bool start(uint16_t port, const std::string& www_dir, Stats* stats);
    void stop();

    // Content served for /serverlist.xml.
    void set_serverlist(const std::string& xml);
    // Content served for /static__Config.txt. Getting this wrong is how a
    // Steam client ends up asking for a username and password: the file
    // carries UseSteamClient, and a 404 leaves the client on its defaults.
    void set_static_config(const std::string& text);
    uint16_t port() const { return port_; }

private:
    void accept_loop();
    void serve(uintptr_t sock);
    std::string route(const std::string& path, std::string& ctype);

    uint16_t port_ = 0;
    std::string www_;
    Stats* stats_ = nullptr;
    uintptr_t srv_ = ~uintptr_t(0);
    std::thread th_;
    std::atomic<bool> run_{false};
    mutable std::mutex m_;
    std::string serverlist_;
    std::string static_config_;
};

}  // namespace relay
