// tos_relay -- patch, proxy, dump.
//
// Points the game client at this process by editing its config, relays every
// byte to the real server untouched, writes every decoded packet to a
// timestamped binary dump, and puts the client's files back the way it found
// them when it stops -- including after a kill, on the next run.
//
// Build:  cmake -S . -B build -G Ninja && cmake --build build
//    or:  g++ -std=c++17 -O2 -o tos_relay.exe *.cpp -lws2_32 -static
//
// Run (from the directory holding bf_inittable.bin and packet_opcodes.csv):
//
//   tos_relay.exe --region=asia
//   tos_relay.exe --restore              # put files back after a hard kill
//
// Patching files inside the game install needs administrator rights; without
// them the relay still runs, says so, and leaves the files alone.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "dump.h"
#include "guard.h"
#include "httpd.h"
#include "log.h"
#include "proto.h"
#include "proxy.h"
#include "stats.h"

using namespace relay;

namespace {

struct Options {
    std::string region = "asia";
    std::string game = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\TreeOfSavior";
    std::string data = ".";
    std::string out;
    std::string www;
    std::string note = "live-mitm";
    std::string key = "hsunffalqyrqewes";
    std::string steam = "auto";     // auto | yes | no
    std::string upstream0, upstream1;
    uint16_t listen0 = 7001, listen1 = 7002;
    uint16_t http = 8080;
    uint16_t relay_base = 17001;
    bool patch = true;
    bool redirect = true;
    bool restore_only = false;
};

// ------------------------------------------------------------ global state
//
// These outlive main's scope on purpose: the console control handler runs on
// its own thread and has to be able to finish the restore before Windows
// tears the process down.
Blowfish  g_bf;
Table     g_tbl;
Dump      g_dump;
Stats     g_stats;
Guard     g_guard;
Proxy     g_proxy;
Httpd     g_httpd;
Dashboard g_dash;

std::atomic<bool> g_stopping{false};
std::atomic<bool> g_done{false};
HANDLE g_quit = nullptr;

// The single exit path. Idempotent, and safe to call from a signal handler,
// the console control handler, or normal return. `clean` records in the dump
// whether we got here on purpose.
void shutdown_all(bool clean, const char* why) {
    if (g_stopping.exchange(true)) {
        // A second trigger (Ctrl+C twice) waits for the first to finish rather
        // than racing it -- half a restore is worse than a slow one.
        while (!g_done.load()) Sleep(50);
        return;
    }
    LOGI("shutting down (%s)", why);

    g_dash.stop();
    g_httpd.stop();
    g_proxy.stop();
    g_dump.close(clean);

    int restored = g_guard.restore_all();

    LOGI("dump: %s (%llu records, %s%s)", g_dump.path().c_str(),
         (unsigned long long)g_dump.records(), human_bytes(g_dump.bytes()).c_str(),
         g_dump.dropped() ? ", RECORDS DROPPED" : "");
    LOGI("packets: %llu c2s / %llu s2c, %llu distinct opcodes, %llu checksum "
         "failures", (unsigned long long)g_stats.packets(0),
         (unsigned long long)g_stats.packets(1),
         (unsigned long long)g_stats.distinct(),
         (unsigned long long)g_stats.bad_checksum());
    // Opcodes the table does not have are the reason to run a capture against
    // a client newer than the table, so they get named on the way out rather
    // than left for someone to find in the dump.
    auto unknown = g_stats.unknowns();
    if (!unknown.empty()) {
        LOGW("%u opcode(s) not in packet_opcodes.csv; %llu unframed record(s), "
             "%s captured that the table could not explain",
             unsigned(unknown.size()),
             (unsigned long long)g_stats.unframed_records(),
             human_bytes(g_stats.unframed_bytes()).c_str());
        LOGI("opcode_dec,opcode_hex,name,size   <- candidate rows, sizes are "
             "observed, not declared");
        for (const auto& kv : unknown) {
            LOGI("  %u,0x%04X,UNKNOWN_%u,%u%s   (%llu seen, %s, len %u..%u)",
                 kv.first, kv.first, kv.first,
                 kv.second.min_len == kv.second.max_len ? kv.second.max_len : 0,
                 kv.second.min_len == kv.second.max_len ? "" : "  variable?",
                 (unsigned long long)kv.second.count,
                 kv.second.from_client ? "client->server" : "server->client",
                 kv.second.min_len, kv.second.max_len);
        }
    }
    if (g_guard.armed())
        LOGE("STOPPED WITH %u FILE(S) STILL PATCHED -- rerun with --restore, or "
             "copy the backups over them by hand",
             unsigned(g_guard.entries().size()));
    else
        LOGI("game files: original (%d restored)", restored);
    LOGI("log: %s", g_log.path().c_str());

    g_done = true;
    if (g_quit) SetEvent(g_quit);
    g_log.close();
}

BOOL WINAPI ctrl_handler(DWORD type) {
    const char* why = "signal";
    switch (type) {
        case CTRL_C_EVENT:     why = "Ctrl+C"; break;
        case CTRL_BREAK_EVENT: why = "Ctrl+Break"; break;
        case CTRL_CLOSE_EVENT: why = "console closed"; break;
        case CTRL_LOGOFF_EVENT: why = "logoff"; break;
        case CTRL_SHUTDOWN_EVENT: why = "system shutdown"; break;
        default: break;
    }
    // For CTRL_CLOSE the process dies the moment this returns, so the restore
    // has to happen here and now, not on some other thread.
    shutdown_all(true, why);
    return TRUE;
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    LOGE("UNHANDLED EXCEPTION 0x%08lX at %p -- restoring files before dying",
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress
                                   : nullptr);
    shutdown_all(false, "crash");
    return EXCEPTION_EXECUTE_HANDLER;
}

void signal_handler(int sig) {
    shutdown_all(sig != SIGABRT, sig == SIGINT ? "SIGINT" : "signal");
    _exit(0);
}

// ------------------------------------------------------------------- setup

bool parse(int argc, char** argv, Options& o) {
    auto val = [](const std::string& a) {
        size_t e = a.find('=');
        return e == std::string::npos ? std::string() : a.substr(e + 1);
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--region=", 0) == 0) o.region = val(a);
        else if (a.rfind("--game=", 0) == 0) o.game = val(a);
        else if (a.rfind("--data=", 0) == 0) o.data = val(a);
        else if (a.rfind("--out=", 0) == 0) o.out = val(a);
        else if (a.rfind("--www=", 0) == 0) o.www = val(a);
        else if (a.rfind("--note=", 0) == 0) o.note = val(a);
        else if (a.rfind("--key=", 0) == 0) o.key = val(a);
        else if (a.rfind("--steam=", 0) == 0) o.steam = val(a);
        else if (a.rfind("--upstream0=", 0) == 0) o.upstream0 = val(a);
        else if (a.rfind("--upstream1=", 0) == 0) o.upstream1 = val(a);
        else if (a.rfind("--listen0=", 0) == 0) o.listen0 = uint16_t(atoi(val(a).c_str()));
        else if (a.rfind("--listen1=", 0) == 0) o.listen1 = uint16_t(atoi(val(a).c_str()));
        else if (a.rfind("--http=", 0) == 0) o.http = uint16_t(atoi(val(a).c_str()));
        else if (a.rfind("--relay-base=", 0) == 0) o.relay_base = uint16_t(atoi(val(a).c_str()));
        else if (a == "--no-patch") o.patch = false;
        else if (a == "--no-redirect") o.redirect = false;
        else if (a == "--restore") o.restore_only = true;
        else if (a == "-h" || a == "--help") return false;
        else { std::printf("unknown option: %s\n", a.c_str()); return false; }
    }
    if (o.out.empty()) o.out = join_path(o.data, "relay");
    if (o.www.empty()) o.www = join_path(o.data, "www");
    return true;
}

void usage() {
    std::printf(
        "tos_relay -- patch the client's config, proxy it, dump every packet\n\n"
        "  --region=asia|na       upstream preset                (asia)\n"
        "  --upstream0=ip:port    override the Server0 upstream\n"
        "  --upstream1=ip:port    override the Server1 upstream\n"
        "  --listen0=N            local port for Server0         (7001)\n"
        "  --listen1=N            local port for Server1         (7002)\n"
        "  --http=N               local http port, 0 to disable  (8080)\n"
        "  --relay-base=N         first port for zone relays     (17001)\n"
        "  --game=DIR             game install to patch\n"
        "  --data=DIR             bf_inittable.bin, packet_opcodes.csv   (.)\n"
        "  --out=DIR              dumps, logs, backups           (DATA\\relay)\n"
        "  --www=DIR              extra files to serve over http (DATA\\www)\n"
        "  --steam=auto|yes|no    Steam login in the config we serve  (auto)\n"
        "  --note=TEXT            stored in the dump header\n"
        "  --no-patch             leave the game's files alone\n"
        "  --no-redirect          do not rewrite zone addresses\n"
        "  --restore              restore patched files and exit\n");
}

bool split_hostport(const std::string& s, std::string& host, uint16_t& port) {
    size_t c = s.rfind(':');
    if (c == std::string::npos) return false;
    host = s.substr(0, c);
    port = uint16_t(atoi(s.c_str() + c + 1));
    return !host.empty() && port != 0;
}

std::string make_serverlist(uint16_t p0, uint16_t p1) {
    char b[512];
    std::snprintf(b, sizeof(b),
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                  "<serverlist>\r\n"
                  "\t<server GROUP_ID=\"1001\" TRAFFIC=\"0\" ENTER_LIMIT=\"100\" "
                  "NAME=\"[Relay] tosemu\" Server0_IP=\"127.0.0.1\" "
                  "Server0_Port=\"%u\" Server1_IP=\"127.0.0.1\" "
                  "Server1_Port=\"%u\"/>\r\n"
                  "</serverlist>\r\n",
                  p0, p1);
    return b;
}

// The static config the client fetches from StaticConfigURL, built from the
// install's own client.xml.
//
// This matters more than it looks. Repointing StaticConfigURL at ourselves and
// then serving nothing for it leaves the client on its built-in defaults --
// and one of the settings in this file is UseSteamClient. A Steam player whose
// client falls back to UseSteamClient=NO is asked for a username and password
// that they do not have, and it looks like the relay broke the login rather
// than the config it failed to serve. So the values are read out of the game's
// own client.xml and handed straight back.
std::string make_static_config(const Options& o, const std::string& client_xml,
                               bool* steam_on) {
    std::string nation = get_xml_attr(client_xml, "ServiceNation");
    std::string dict = get_xml_attr(client_xml, "Dictionary");
    std::string sso = get_xml_attr(client_xml, "UseNexonSSO");
    std::string glm = get_xml_attr(client_xml, "UseNexonGameLogManager");
    std::string hs = get_xml_attr(client_xml, "HackShield");
    std::string xign = get_xml_attr(client_xml, "Xigncode");
    std::string steam = get_xml_attr(client_xml, "UseSteamClient");

    bool on = steam == "YES";
    if (o.steam == "yes") on = true;
    else if (o.steam == "no") on = false;
    *steam_on = on;

    std::string s;
    s += "ServiceNation=" + (nation.empty() ? std::string("GLOBAL") : nation) + "\r\n";
    s += "Dictionary=" + (dict.empty() ? std::string("YES") : dict) + "\r\n";
    s += "UseNexonSSO=" + (sso.empty() ? std::string("NO") : sso) + "\r\n";
    s += "UseNexonGLM=" + (glm.empty() ? std::string("NO") : glm) + "\r\n";
    s += "UseHackshield=" + (hs.empty() ? std::string("NO") : hs) + "\r\n";
    s += std::string("UseSteamClient=") + (on ? "YES" : "NO") + "\r\n";
    s += "UseXigncode=" + (xign.empty() ? std::string("NO") : xign) + "\r\n";
    s += "UseNISMS_TESTURL=NO\r\n";
    s += "UseNISMS_ONLY_OFFER=YES\r\n";
    return s;
}

// Points the client's two config URLs at our own http server, leaving every
// other byte of its file exactly as it was.
bool patch_client_xml(const Options& o, const std::string& path) {
    std::string xml;
    if (!read_file(path, xml)) {
        LOGW("no client.xml at %s -- skipped", path.c_str());
        return false;
    }
    char list[128], base[128];
    std::snprintf(list, sizeof(list), "http://127.0.0.1:%u/serverlist.xml", o.http);
    std::snprintf(base, sizeof(base), "http://127.0.0.1:%u/", o.http);
    int n = set_xml_attr(xml, "ServerListURL", list);
    n += set_xml_attr(xml, "StaticConfigURL", base);
    if (n == 0) {
        LOGW("%s has no ServerListURL/StaticConfigURL -- skipped", path.c_str());
        return false;
    }
    return g_guard.patch(path, xml, "ServerListURL/StaticConfigURL -> relay");
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    if (!parse(argc, argv, o)) { usage(); return 1; }

    make_dirs(o.out);
    make_dirs(join_path(o.out, "logs"));
    make_dirs(join_path(o.out, "dumps"));

    const std::string ts = stamp_compact(now_us());
    if (!g_log.open(join_path(join_path(o.out, "logs"), "relay_" + ts + ".log"))) {
        std::printf("cannot open a log file under %s\n", o.out.c_str());
        return 1;
    }
    LOGI("tos_relay starting  pid=%lu  out=%s", GetCurrentProcessId(),
         o.out.c_str());

    if (!g_guard.init(o.out)) {
        LOGE("cannot create %s", join_path(o.out, "backup").c_str());
        return 1;
    }

    // Always first: if the last run was killed, its edits are still on the
    // user's disk. Put them back before doing anything else.
    int stale = g_guard.recover_stale();
    if (o.restore_only) {
        if (stale == 0) LOGI("nothing was patched; game files are original");
        g_log.close();
        return stale < 0 ? 1 : 0;
    }
    if (stale < 0) {
        LOGE("refusing to start on top of a journal that cannot be honoured");
        g_log.close();
        return 1;
    }

    // ---- protocol tables
    if (!g_bf.load(join_path(o.data, "bf_inittable.bin"), o.key)) {
        LOGE("bf_inittable.bin missing or short under %s", o.data.c_str());
        g_log.close();
        return 1;
    }
    if (!g_tbl.load(join_path(o.data, "packet_opcodes.csv"))) {
        LOGE("packet_opcodes.csv missing under %s", o.data.c_str());
        g_log.close();
        return 1;
    }
    LOGI("loaded %u opcodes", unsigned(g_tbl.count()));

    // ---- upstreams
    std::vector<Upstream> ups;
    std::string h0, h1;
    uint16_t p0 = 0, p1 = 0;
    if (!o.upstream0.empty() || !o.upstream1.empty()) {
        if (!o.upstream0.empty() && !split_hostport(o.upstream0, h0, p0)) {
            LOGE("bad --upstream0"); g_log.close(); return 1;
        }
        if (!o.upstream1.empty() && !split_hostport(o.upstream1, h1, p1)) {
            LOGE("bad --upstream1"); g_log.close(); return 1;
        }
    } else if (o.region == "na") {
        h0 = "52.5.58.238";     p0 = 7001;
        h1 = "52.4.126.228";    p1 = 7002;
    } else if (o.region == "asia") {
        h0 = "54.180.190.119";  p0 = 7001;
        h1 = "54.180.208.135";  p1 = 7002;
    } else {
        LOGE("unknown --region=%s (try asia, na, or --upstream0/1)",
             o.region.c_str());
        g_log.close();
        return 1;
    }
    if (!h0.empty()) ups.push_back({o.listen0, h0, p0});
    if (!h1.empty()) ups.push_back({o.listen1, h1, p1});

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("WSAStartup failed"); g_log.close(); return 1;
    }

    // ---- handlers before the first patch, so nothing can slip past them
    g_quit = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(crash_handler);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::atexit([] { shutdown_all(true, "exit"); });

    // ---- dump
    const std::string dump_path =
        join_path(join_path(o.out, "dumps"), "capture_" + ts + ".bin");
    if (!g_dump.open(dump_path, o.region, o.note)) {
        LOGE("cannot open dump %s", dump_path.c_str());
        WSACleanup();
        g_log.close();
        return 1;
    }
    LOGI("dump: %s", dump_path.c_str());

    g_stats.start();

    // ---- http, then the patch that points the client at it
    const std::string client_xml_path =
        join_path(join_path(o.game, "release"), "client.xml");
    std::string client_xml;
    read_file(client_xml_path, client_xml);

    if (o.http) {
        bool steam_on = false;
        std::string cfg = make_static_config(o, client_xml, &steam_on);
        g_httpd.set_serverlist(make_serverlist(o.listen0, o.listen1));
        g_httpd.set_static_config(cfg);
        if (!g_httpd.start(o.http, o.www, &g_stats))
            LOGW("http disabled; the client will keep using its own server list");
        LOGI("login: %s (static__Config.txt UseSteamClient=%s%s)",
             steam_on ? "Steam" : "username/password", steam_on ? "YES" : "NO",
             o.steam == "auto" ? ", from client.xml" : ", forced by --steam");
        if (client_xml.empty())
            LOGW("could not read %s -- serving a default static config; pass "
                 "--steam=yes if the client asks for a password",
                 client_xml_path.c_str());
        else if (!steam_on && get_xml_attr(client_xml, "UseSteamClient") == "YES")
            LOGW("client.xml says the client is a Steam client but the config "
                 "we serve disables Steam -- it will ask for a password");
    }

    if (o.patch) {
        std::string rel = join_path(o.game, "release");
        if (o.http) patch_client_xml(o, client_xml_path);
        // The client caches the last list it used and will happily dial those
        // addresses directly, so this one has to go too.
        std::string recent = join_path(rel, "serverlist_recent.xml");
        if (file_exists(recent))
            g_guard.patch(recent, make_serverlist(o.listen0, o.listen1),
                          "cached server list -> 127.0.0.1");
        if (!g_guard.armed())
            LOGW("nothing was patched -- run as administrator to edit files "
                 "under Program Files, or point the client at the relay by hand");
    } else {
        LOGI("--no-patch: the game's files will not be touched");
    }

    // ---- relay
    Proxy::Config pc;
    pc.redirect_zone = o.redirect;
    pc.relay_base = o.relay_base;
    g_proxy.start(ups, pc, &g_bf, &g_tbl, &g_dump, &g_stats);

    char title[160];
    std::snprintf(title, sizeof(title), "tos_relay  region=%s  http=%u",
                  o.region.c_str(), o.http);
    g_dash.start(&g_stats, &g_dump, &g_guard, title);
    LOGI("running -- Ctrl+C to stop and restore");

    WaitForSingleObject(g_quit, INFINITE);
    shutdown_all(true, "quit");
    WSACleanup();
    return 0;
}
