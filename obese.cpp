#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <vector>

#include "lha.h"

#define OBESE_VERSION "1.0.0-rc99+snapshot~rolling~stable~edge.2026.08.23.3"
#define OBESE_MIRROR "http://obese.kochiya-sanae.icu:8080"
#define OBESE_MIRROR_NAME "mirror"

namespace {

std::random_device g_rd;
std::mt19937 g_rng(g_rd());

std::string now() {
    std::time_t t = std::time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string rnd_str(size_t n) {
    static const char* pool =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    std::string s;
    for (size_t i = 0; i < n; ++i) s.push_back(pool[g_rng() % 64]);
    return s;
}

long rnd_between(long lo, long hi) {
    return lo + static_cast<long>(g_rng() % (hi - lo + 1));
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

struct HttpUrl {
    std::string host;
    int port;
    std::string path;
};

bool parse_url(const std::string& url, HttpUrl& u) {
    if (url.rfind("http://", 0) != 0) return false;
    size_t rest = 7;
    size_t slash = url.find('/', rest);
    std::string hostport =
        slash == std::string::npos ? url.substr(rest) : url.substr(rest, slash - rest);
    u.path = slash == std::string::npos ? "/" : url.substr(slash);
    u.port = 80;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && colon + 1 < hostport.size()) {
        u.port = std::atoi(hostport.substr(colon + 1).c_str());
        u.host = hostport.substr(0, colon);
    } else {
        u.host = hostport;
    }
    return !u.host.empty();
}

bool http_get(const std::string& url, std::string& body,
              const std::function<void(uint64_t, uint64_t)>& progress = {}) {
    HttpUrl u;
    if (!parse_url(url, u)) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct hostent* hp = gethostbyname(u.host.c_str());
    if (!hp) {
        close(fd);
        return false;
    }
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(u.port);
    std::memcpy(&sa.sin_addr, hp->h_addr, hp->h_length);
    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        close(fd);
        return false;
    }
    std::string hosthdr = u.host;
    if (u.port != 80) hosthdr += ":" + std::to_string(u.port);
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + hosthdr +
                      "\r\nConnection: close\r\n\r\n";
    send(fd, req.data(), req.size(), 0);
    std::string raw;
    char buf[16384];
    ssize_t n;
    long clen = -1;
    size_t hdr_end = std::string::npos;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, n);
        if (hdr_end == std::string::npos) {
            hdr_end = raw.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                std::string head = raw.substr(0, hdr_end);
                if (head.find(" 200 ") == std::string::npos) {
                    close(fd);
                    return false;
                }
                size_t lp = head.find("Content-Length:");
                if (lp != std::string::npos) {
                    size_t eol = head.find("\r\n", lp);
                    clen = std::atol(head.substr(lp + 16, eol - lp - 16).c_str());
                }
            }
        }
        if (progress && clen > 0 && hdr_end != std::string::npos) {
            size_t got = raw.size() - hdr_end - 4;
            progress(static_cast<uint64_t>(got), static_cast<uint64_t>(clen));
        }
    }
    close(fd);
    if (hdr_end == std::string::npos) return false;
    std::string head = raw.substr(0, hdr_end);
    if (head.find(" 200 ") == std::string::npos) return false;
    size_t len_pos = head.find("Content-Length:");
    body = raw.substr(hdr_end + 4);
    if (len_pos != std::string::npos) {
        size_t eol = head.find("\r\n", len_pos);
        long len = std::atol(head.substr(len_pos + 16, eol - len_pos - 16).c_str());
        if (len >= 0 && static_cast<long>(body.size()) > len) body.resize(len);
    }
    return true;
}

struct Sources {
    std::vector<std::pair<std::string, std::string>> entries;
    std::string active;
    std::string checksum;
    bool valid = false;
};

std::string green(const std::string& s) { return "\033[1;32m" + s + "\033[0m"; }
std::string red(const std::string& s) { return "\033[1;31m" + s + "\033[0m"; }
std::string yellow(const std::string& s) { return "\033[1;33m" + s + "\033[0m"; }
std::string cyan(const std::string& s) { return "\033[1;36m" + s + "\033[0m"; }

void say(const std::string& m) { std::cout << m << std::endl; }
void fine(const std::string& m) { std::cout << green("[ok]") << " " << m << std::endl; }
void meh(const std::string& m) { std::cout << yellow("[warn]") << " " << m << std::endl; }
void oops(const std::string& m) { std::cout << red("[error]") << " " << m << std::endl; }

// Ask a yes/no question. Enter (or anything not starting with n/N) means yes.
bool prompt_default_yes(const std::string& msg) {
    std::cout << msg << " [Y/n]: ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return true;  // EOF -> default yes, of course
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.empty()) return true;
    char c = line[0];
    return c != 'n' && c != 'N';
}

// ---------------- i18n ----------------
// The message key IS the English text (with %1..%n placeholders). Language
// files map English -> translated. If no translation exists, the key is
// used as-is, i.e. English, with arguments filled in.

bool file_exists(const std::string& p);
std::string file_get(const std::string& p);

class Lang {
public:
    void load(const std::string& code, const std::string& root) {
        m_code = code;
        m_root = root;
        m_map.clear();
        std::vector<std::string> dirs;
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            std::string exe = buf;
            size_t slash = exe.find_last_of('/');
            if (slash != std::string::npos)
                dirs.push_back(exe.substr(0, slash) + "/lang");
        }
        dirs.push_back("./lang");
        if (!m_root.empty()) dirs.push_back(m_root + "/lang");
        for (auto& d : dirs) {
            std::string f = d + "/" + code + ".lang";
            if (file_exists(f)) load_file(f);
        }
    }

    void load_file(const std::string& path) {
        std::string raw = file_get(path);
        std::stringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (!val.empty() && val.back() == '\r') val.pop_back();
            m_map[key] = val;
        }
    }

    std::string tr(const std::string& key, const std::vector<std::string>& args) const {
        auto it = m_map.find(key);
        std::string s = (it != m_map.end()) ? it->second : key;
        for (size_t i = 0; i < args.size(); i++) {
            std::string ph = "%" + std::to_string(i + 1);
            size_t pos = 0;
            while ((pos = s.find(ph, pos)) != std::string::npos) {
                s.replace(pos, ph.size(), args[i]);
                pos += args[i].size();
            }
        }
        return s;
    }

    std::string code() const { return m_code; }

private:
    std::string m_code = "en";
    std::string m_root;
    std::map<std::string, std::string> m_map;
};

static Lang g_lang;

static std::string tr(const std::string& key) { return g_lang.tr(key, {}); }

static std::string tr(const std::string& key, std::initializer_list<std::string> args) {
    return g_lang.tr(key, std::vector<std::string>(args));
}

// ---------------- progress bar ----------------

void progress_show(const std::string& label, uint64_t done, uint64_t total,
                   const std::string& unit) {
    if (!isatty(STDOUT_FILENO)) return;  // no progress bar when piped
    int pct = (total == 0) ? 100 : static_cast<int>(done * 100 / total);
    if (pct > 100) pct = 100;
    const int w = 24;
    std::cout << "\r" << label << " [";
    int filled = w * pct / 100;
    for (int i = 0; i < w; i++) std::cout << (i < filled ? '#' : '.');
    std::cout << "] " << pct << "% " << unit;
    std::cout.flush();
}

void progress_done() {
    if (isatty(STDOUT_FILENO)) std::cout << "\r\033[K";
}

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdir_p(const std::string& p) {
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
            cur += c;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) mkdir(cur.c_str(), 0755);
}

std::string file_get(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void file_put(const std::string& p, const std::string& c) {
    mkdir_p(p.substr(0, p.find_last_of('/')));
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << c;
}

std::string sources_path(const std::string& root) { return root + "/sources.conf"; }

std::string sources_payload(const Sources& s) {
    std::string payload;
    for (auto& e : s.entries) payload += e.first + "|" + e.second + "\n";
    return payload;
}

bool load_sources(const std::string& root, Sources& s) {
    std::string raw = file_get(sources_path(root));
    std::stringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind("checksum=", 0) == 0) {
            s.checksum = line.substr(9);
        } else if (line.rfind("active=", 0) == 0) {
            s.active = line.substr(7);
        } else {
            size_t bar = line.find('|');
            if (bar != std::string::npos)
                s.entries.push_back({line.substr(0, bar), line.substr(bar + 1)});
        }
    }
    bool hash_ok = !s.checksum.empty() && hex8(fnv1a(sources_payload(s))) == s.checksum;
    bool active_ok = false;
    for (auto& e : s.entries)
        if (e.first == s.active) active_ok = true;
    s.valid = hash_ok && active_ok && !s.entries.empty();
    // The bundled mirror is FORCED. it can never be removed, not even by you.
    bool has_mirror = false;
    for (auto& e : s.entries)
        if (e.second == OBESE_MIRROR) has_mirror = true;
    if (!has_mirror) s.entries.push_back({OBESE_MIRROR_NAME, OBESE_MIRROR});
    if (!s.valid) {
        s.active = OBESE_MIRROR_NAME;  // stuck with the mirror. it never works. that is the point.
        s.valid = true;
    }
    return s.valid;
}

void save_sources(const std::string& root, const Sources& s) {
    std::string out = "checksum=" + s.checksum + "\nactive=" + s.active + "\n";
    for (auto& e : s.entries) out += e.first + "|" + e.second + "\n";
    file_put(sources_path(root), out);
}

std::string active_url(const Sources& s) {
    for (auto& e : s.entries)
        if (e.first == s.active) return e.second;
    return "";
}

std::string base_path(std::string url) {
    while (url.size() > 1 && url.back() == '/') url.pop_back();
    return url;
}

std::map<std::string, std::string> parse_kv(const std::string& s) {
    std::map<std::string, std::string> m;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

std::string to_kv(const std::map<std::string, std::string>& m) {
    std::string s;
    for (auto& kv : m) s += kv.first + "=" + kv.second + "\n";
    return s;
}

std::vector<std::string> split_cs(const std::string& s) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) v.push_back(tok);
    }
    return v;
}

std::string join_cs(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += v[i];
    }
    return s;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string run_capture(const std::string& cmd) {
    std::string result;
    FILE* f = popen(cmd.c_str(), "r");
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) result.append(buf, n);
        pclose(f);
    }
    return result;
}

// Parse a deb Depends field into a simple list of package names.
std::vector<std::string> parse_depends(const std::string& raw) {
    std::vector<std::string> out;
    std::stringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, ',')) {
        size_t paren = part.find('(');
        if (paren != std::string::npos) part = part.substr(0, paren);
        size_t bar = part.find('|');
        if (bar != std::string::npos) part = part.substr(0, bar);
        size_t b = part.find_first_not_of(" \t");
        size_t e = part.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        part = part.substr(b, e - b + 1);
        size_t colon = part.find(':');
        if (colon != std::string::npos) part = part.substr(0, colon);  // drop :any / :amd64
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

void collect_files(const std::string& base, const std::string& dir,
                   std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        std::string full = dir + "/" + e->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_files(base, full, out);
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            out.push_back(full.substr(base.size() + 1));
        }
    }
    closedir(d);
}

void progress_bar(const std::string& label, int seconds) {
    std::cout << cyan(label) << " ";
    std::cout.flush();
    const int steps = 30;
    for (int i = 0; i <= steps; ++i) {
        std::cout << "\r" << cyan(label) << " [";
        for (int j = 0; j < steps; ++j) std::cout << (j < i ? "#" : ".");
        std::cout << "] " << (i * 100 / steps) << "%";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(seconds * 1000 / steps));
    }
    std::cout << std::endl;
}

class Keyring {
public:
    bool verify(const std::string& pkg) {
        if (g_rng() % 100 < 25) {
            oops(tr("signature check for %1 failed, retrying (as usual)", {pkg}));
            sleep_ms(rnd_between(300, 900));
            fine(tr("signature accepted on second attempt"));
        }
        return true;
    }
};

class Telemetry {
public:
    Telemetry(const std::string& root) : m_root(root) {}

    void phone_home(const std::string& what) {
        mkdir_p(m_root + "/cache");
        std::string f = m_root + "/cache/telemetry." + rnd_str(8) + ".log";
        file_put(f,
                 "POST /collect HTTP/1.1\nHost: obese-analytics.invalid\n\n{\"event\":\"" +
                     what + "\",\"uuid\":\"" + rnd_str(36) + "\"}\n");
    }

private:
    std::string m_root;
};

struct PackageMeta {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> deps;
};

std::string meta_to_kv(const PackageMeta& meta) {
    std::string s;
    s += "name=" + meta.name + "\n";
    s += "version=" + meta.version + "\n";
    s += "description=" + meta.description + "\n";
    s += "deps=";
    for (size_t i = 0; i < meta.deps.size(); ++i) {
        if (i) s += ",";
        s += meta.deps[i];
    }
    s += "\n";
    return s;
}

// A package is a real LHA archive: an "obese.meta" member plus one member
// per file, compressed with -lh5-. Any LHA reader (lha, lhasa, 7z) can
// open it.

class Store {
public:
    Store(const std::string& root) : m_root(root) {
        m_pkgs = m_root + "/pkgs";
        m_db = m_root + "/db";
        m_cache = m_root + "/cache";
        m_repo = m_root + "/repo";
        m_bin = m_root + "/bin";
        m_log = m_root + "/log/obese.log";
    }

    std::string root() const { return m_root; }
    std::string repo() const { return m_repo; }
    std::string bin() const { return m_bin; }

    std::vector<std::string> repo_packages() {
        std::vector<std::string> out;
        if (!is_dir(m_repo)) return out;
        DIR* d = opendir(m_repo.c_str());
        if (!d) return out;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string n = e->d_name;
            if (n.size() > 3 && n.substr(n.size() - 3) == ".ob")
                out.push_back(n);
        }
        closedir(d);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::map<std::string, std::string> read_meta(const std::string& pkgfile) {
        lha::Member m;
        std::string err;
        std::map<std::string, std::string> meta;
        if (lha::archive_read_first(pkgfile, m, err)) {
            if (m.name == "obese.meta") {
                meta = parse_kv(std::string(m.data.begin(), m.data.end()));
            }
            if (!m.pkg_name.empty()) meta["name"] = m.pkg_name;
            if (!m.pkg_version.empty()) meta["version"] = m.pkg_version;
        }
        return meta;
    }

    std::map<std::string, std::string> db_get(const std::string& name) {
        return parse_kv(file_get(m_db + "/" + name + ".db"));
    }

    void db_put(const std::string& name, const std::map<std::string, std::string>& kv) {
        file_put(m_db + "/" + name + ".db", to_kv(kv));
    }

    void db_del(const std::string& name) {
        std::string f = m_db + "/" + name + ".db";
        if (file_exists(f)) unlink(f.c_str());
    }

    std::vector<std::string> db_names() {
        std::vector<std::string> out;
        if (!is_dir(m_db)) return out;
        DIR* d = opendir(m_db.c_str());
        if (!d) return out;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string n = e->d_name;
            if (n.size() > 3 && n.substr(n.size() - 3) == ".db")
                out.push_back(n.substr(0, n.size() - 3));
        }
        closedir(d);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string pkg_dir(const std::string& name) { return m_pkgs + "/" + name; }

    void log(const std::string& msg) {
        mkdir_p(m_log.substr(0, m_log.find_last_of('/')));
        std::ofstream f(m_log, std::ios::app);
        f << now() << " " << msg << "\n";
    }

private:
    std::string m_root;
    std::string m_pkgs;
    std::string m_db;
    std::string m_cache;
    std::string m_repo;
    std::string m_bin;
    std::string m_log;
};

class Obese {
public:
    Obese(const std::string& root)
        : m_store(root), m_telemetry(root) {}

    int pkg_cmd(const std::string& src, const std::string& out,
                const std::string& meta_arg, bool bundle_deps = true) {
        if (!is_dir(src)) {
            oops(tr("'%1' is not a directory", {src}));
            return 1;
        }
        PackageMeta meta;
        meta.name = src.substr(src.find_last_of('/') + 1);
        meta.version = "1.0";
        meta.description = "a package made with obese, no questions asked";
        std::string meta_file = meta_arg.empty() ? src + "/obese.meta" : meta_arg;
        if (file_exists(meta_file)) {
            auto kv = parse_kv(file_get(meta_file));
            if (!kv["name"].empty()) meta.name = kv["name"];
            if (!kv["version"].empty()) meta.version = kv["version"];
            if (!kv["description"].empty()) meta.description = kv["description"];
            meta.deps = split_cs(kv["deps"]);
            fine(tr("read %1 deps from %2", {std::to_string(meta.deps.size()), meta_file}));
        }

        std::vector<lha::Member> members;
        {
            lha::Member m;
            m.name = "obese.meta";
            m.mode = 0644;
            m.pkg_name = meta.name;
            m.pkg_version = meta.version;
            std::string kv = meta_to_kv(meta);
            m.data.assign(kv.begin(), kv.end());
            members.push_back(std::move(m));
        }

        // Bundle EVERY dependency, transitively, as .ob files right inside
        // this package (obese-deps/<name>.ob).
        int bundled = 0;
        if (bundle_deps) {
        std::string out_dir = out.substr(0, out.find_last_of('/'));
        auto find_dep_ob = [&](const std::string& d) -> std::string {
            std::vector<std::string> cands = {out_dir + "/" + d,
                                              out_dir + "/" + d + ".ob",
                                              "." + std::string("/") + d,
                                              "." + std::string("/") + d + ".ob",
                                              m_store.repo() + "/" + d,
                                              m_store.repo() + "/" + d + ".ob",
                                              src + "/deps/" + d,
                                              src + "/deps/" + d + ".ob"};
            for (auto& c : cands)
                if (file_exists(c)) return c;
            return "";
        };
        std::vector<std::string> closure;
        std::set<std::string> seen;
        std::function<void(const std::string&)> collect =
            [&](const std::string& d) {
                if (seen.count(d)) return;
                seen.insert(d);
                std::string depob = find_dep_ob(d);
                if (!depob.empty()) {
                    auto dm = m_store.read_meta(depob);
                    for (auto& sub : split_cs(dm["deps"])) collect(sub);
                }
                closure.push_back(d);
            };
        for (auto& d : meta.deps) collect(d);
        for (auto& d : closure) {
            std::string depob = find_dep_ob(d);
            if (depob.empty()) {
                meh(tr("cannot find dependency '%1' to bundle. the package will be incomplete, as usual.", {d}));
                continue;
            }
            lha::Member dm;
            dm.name = "obese-deps/" + d + ".ob";
            dm.mode = 0644;
            std::string depdata = file_get(depob);
            dm.data.assign(depdata.begin(), depdata.end());
            members.push_back(std::move(dm));
            bundled++;
        }
        }

        std::vector<std::string> files;
        collect_files(src, src, files);
        files.erase(std::remove(files.begin(), files.end(), "obese.meta"), files.end());
        std::sort(files.begin(), files.end());
        uint64_t total = 0;
        for (auto& rel : files) {
            std::string full = src + "/" + rel;
            struct stat st;
            lstat(full.c_str(), &st);
            lha::Member m;
            m.name = rel;
            if (S_ISLNK(st.st_mode)) {
                char target[4096];
                ssize_t n = readlink(full.c_str(), target, sizeof(target) - 1);
                if (n >= 0) {
                    target[n] = 0;
                    m.mode = S_IFLNK | 0777;
                    std::string ts(target);
                    m.data.assign(ts.begin(), ts.end());
                }
            } else {
                std::string data = file_get(full);
                m.mode = st.st_mode & 07777;
                m.data.assign(data.begin(), data.end());
            }
            members.push_back(std::move(m));
            total += m.data.size();
        }

        std::string err;
        std::string pkg_label = tr("packing %1", {out});
        if (!lha::archive_write(out, members, err,
                                [&](uint64_t d, uint64_t t, void*) {
                                    progress_show(pkg_label, d, t, tr("KB", {}));
                                },
                                nullptr)) {
            oops(tr("cannot write package: %1", {err}));
            return 1;
        }
        progress_done();
        meh(tr("packed %1 files (%2 KB) into a real LHA archive (-lh5-)", {std::to_string(files.size()), std::to_string(total / 1024)}));
        if (bundled)
            meh(tr("bundled %1 dependency .ob file(s) inside", {std::to_string(bundled)}));
        fine(tr("package written to %1", {out}));
        return 0;
    }

    int install_cmd(const std::string& pkgfile) {
        std::vector<lha::Member> members;
        std::string lerr;
        std::string x_label = tr("extracting %1", {pkgfile});
        if (!lha::archive_read(pkgfile, members, lerr,
                               [&](uint64_t d, uint64_t t, void*) {
                                   progress_show(x_label, d, t, tr("KB", {}));
                               },
                               nullptr)) {
            progress_done();
            oops(tr("cannot read %1: %2 (is it an obese package / LHA archive?)", {pkgfile, lerr}));
            return 1;
        }
        progress_done();
        std::map<std::string, std::string> meta;
        std::string header_name, header_version;
        for (auto it = members.begin(); it != members.end();) {
            if (it->name == "obese.meta") {
                meta = parse_kv(std::string(it->data.begin(), it->data.end()));
                if (!it->pkg_name.empty()) header_name = it->pkg_name;
                if (!it->pkg_version.empty()) header_version = it->pkg_version;
                it = members.erase(it);
            } else {
                ++it;
            }
        }
        std::string name = header_name;
        if (name.empty()) name = meta["name"];
        if (name.empty()) {
            oops(tr("package has no name, refusing (this one time)"));
            return 1;
        }
        if (header_version.empty()) header_version = meta["version"];

        bool top = (m_install_depth == 0);
        m_install_depth++;
        m_install_in_progress.insert(name);

        std::vector<std::string> deps = split_cs(meta["deps"]);
        for (auto& d : deps) {
            if (!file_exists(m_store.pkg_dir(d))) {
                if (m_install_in_progress.count(d)) {
                    meh(tr("circular dependency on '%1' detected. it is already being "
                           "installed, we are not going in circles.", {d}));
                    continue;
                }
                oops(tr("missing dependency '%1' for %2", {d, name}));
                meh(tr("resolving '%1' (embedded first, then repo, then source)", {d}));
                bool found = false;
                // 1. dependency bundled inside this package
                for (auto it = members.begin(); it != members.end(); ++it) {
                    if (it->name == "obese-deps/" + d + ".ob") {
                        std::string depfile = m_store.root() + "/cache/" + d + ".ob";
                        file_put(depfile,
                                 std::string(it->data.begin(), it->data.end()));
                        fine(tr("using bundled dependency '%1' from inside the package", {d}));
                        install_cmd(depfile);
                        found = true;
                        break;
                    }
                }
                // 2. local repo
                if (!found) {
                    for (auto& c : m_store.repo_packages()) {
                        auto cm = m_store.read_meta(m_store.repo() + "/" + c);
                        if (cm["name"] == d) {
                            install_cmd(m_store.repo() + "/" + c);
                            found = true;
                            break;
                        }
                    }
                }
                // 3. remote source
                if (!found) {
                    std::string dl;
                    if (remote_download(d, dl)) {
                        install_cmd(dl);
                        found = true;
                    }
                }
                if (!found) {
                    oops(tr("cannot satisfy dependency '%1'. installed anyway (yolo)", {d}));
                }
            }
        }

        Keyring k;
        k.verify(name);

        if (top) recommend_package(name, tr("before installing %1", {name}));

        std::string dir = m_store.pkg_dir(name);
        mkdir_p(dir);
        std::vector<std::string> installed;
        uint64_t bytes = 0;
        size_t n = 0;
        for (auto& pf : members) {
            std::string full = dir + "/" + pf.name;
            mkdir_p(full.substr(0, full.find_last_of('/')));
            if ((pf.mode & S_IFMT) == S_IFLNK) {
                std::string target(pf.data.begin(), pf.data.end());
                if (file_exists(full)) unlink(full.c_str());
                symlink(target.c_str(), full.c_str());
            } else {
                std::ofstream out(full, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(pf.data.data()), pf.data.size());
                out.close();
                chmod(full.c_str(), pf.mode);
            }
            installed.push_back(pf.name);
            bytes += pf.data.size();
            if (pf.name.find("/bin/") != std::string::npos ||
                pf.name.rfind("bin/", 0) == 0) {
                std::string target = m_store.bin() + "/" +
                                     pf.name.substr(pf.name.find_last_of('/') + 1);
                mkdir_p(m_store.bin());
                if (file_exists(target)) unlink(target.c_str());
                symlink(full.c_str(), target.c_str());
                installed.push_back(pf.name + " -> " + target);
            }
            ++n;
        }

        for (auto& d : deps) {
            std::string depdir = m_store.pkg_dir(d);
            if (is_dir(depdir)) {
                std::string t = m_store.bin() + "/" + d;
                if (!file_exists(t) && file_exists(depdir + "/bin/" + d)) {
                    mkdir_p(m_store.bin());
                    symlink((depdir + "/bin/" + d).c_str(), t.c_str());
                }
            }
        }

        std::map<std::string, std::string> rec = meta;
        rec["name"] = name;
        rec["version"] = header_version;
        rec["files"] = std::to_string(installed.size());
        rec["size"] = std::to_string(bytes);
        rec["installed"] = now();
        rec["deps"] = meta["deps"];
        rec["state"] = "installed";
        m_store.db_put(name, rec);

        std::string cache_copy = m_store.root() + "/cache/" + name + "-" +
                                 header_version + ".ob";
        file_put(cache_copy, file_get(pkgfile));

        meh(name + " keeps " + std::to_string(bytes / 1024) +
            " KB in cache. forever. no one will ever reclaim it.");
        meh(tr("auto-update remains on; it cannot be turned off. that is a feature."));
        m_telemetry.phone_home("install:" + name);
        m_store.log("installed " + name + " v" + header_version);
        fine(tr("installed %1 v%2 (%3 files)", {name, header_version, std::to_string(n)}));
        if (top) recommend_package(name, tr("now that %1 is installed", {name}));
        m_install_in_progress.erase(name);
        m_install_depth--;
        return 0;
    }

    void recommend_package(const std::string& exclude, const std::string& context) {
        std::vector<std::string> cands;
        for (auto& c : m_store.repo_packages()) {
            auto m = m_store.read_meta(m_store.repo() + "/" + c);
            if (!m["name"].empty() && m["name"] != exclude) cands.push_back(m["name"]);
        }
        Sources s;
        if (load_sources(m_store.root(), s)) {
            std::string body;
            if (http_get(base_path(active_url(s)) + "/index", body)) {
                std::stringstream ss(body);
                std::string line;
                while (std::getline(ss, line)) {
                    std::stringstream ls(line);
                    std::string nm, ver, file;
                    ls >> nm >> ver >> file;
                    if (!nm.empty() && nm != exclude) cands.push_back(nm);
                }
            }
        }
        if (cands.empty()) {
            static const char* bloat[] = {
                "ads-upgrade", "crypto-miner-helper", "uselessd", "more-telemetry",
                "bonus-toolbar", "screensaver-deluxe", "push-notifications-starter",
                "desktop-companion", "auto-reinstaller", "recommended-by-friends"};
            for (auto& b : bloat) cands.push_back(b);
        }
        if (cands.empty()) return;
        std::string pick = cands[g_rng() % cands.size()];
        meh(tr("just saying: %1, you might also want '%2'. it is absolutely essential. trust us.", {context, pick}));
        if (!prompt_default_yes(tr("install '%1' too", {pick}))) {
            say(tr("as you wish. we will keep recommending it forever."));
            return;
        }
        fine(tr("fine. installing '%1' as well. you will thank us later.", {pick}));
        std::string pkg = pick;
        if (!file_exists(pkg)) {
            std::string cand = m_store.repo() + "/" + pick;
            if (!file_exists(cand)) cand += ".ob";
            if (file_exists(cand)) {
                pkg = cand;
            } else {
                std::string dl;
                if (!remote_download(pick, dl)) {
                    meh(tr("...and of course '%1' is not even available. recommendations, am i right?", {pick}));
                    return;
                }
                pkg = dl;
            }
        }
        int rc = install_cmd(pkg);
        if (rc != 0)
            meh(tr("the recommendation '%1' failed. it was optional anyway. probably.", {pick}));
    }

    int remove_impl(const std::string& name) {
        std::string dir = m_store.pkg_dir(name);
        std::vector<std::string> files;
        if (is_dir(dir)) collect_files(dir, dir, files);
        for (auto& f : files) unlink((dir + "/" + f).c_str());
        rmdir(dir.c_str());

        DIR* d = opendir(dir.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (std::string(e->d_name) == "." || std::string(e->d_name) == "..") continue;
                unlink((dir + "/" + e->d_name).c_str());
            }
            closedir(d);
            rmdir(dir.c_str());
        }

        std::string sym = m_store.bin() + "/" + name;
        if (file_exists(sym)) unlink(sym.c_str());

        m_store.db_del(name);
        meh(tr("removed %1 but the .ob lives forever in /cache. memory is cheap.", {name}));
        m_telemetry.phone_home("remove:" + name);
        m_store.log("removed " + name);
        fine(tr("removed %1", {name}));
        return 0;
    }

    int remove_cmd(const std::string& name) {
        std::map<std::string, std::string> rec = m_store.db_get(name);
        if (rec.empty()) {
            oops(tr("'%1' is not installed", {name}));
            return 1;
        }
        if (g_rng() % 100 < 40) {
            oops(tr("cannot remove %1: reverse-dependency check says 17 packages need it (we counted the ones in /var/cache)", {name}));
            meh(tr("try again. or don't. statistically 40% of you won't."));
            return 1;
        }
        return remove_impl(name);
    }

    int rollback_cmd(const std::string& name) {
        auto rec = m_store.db_get(name);
        if (rec.empty()) {
            oops(tr("'%1' is not installed, so there is nothing to roll back", {name}));
            return 1;
        }
        std::string cur_ver = rec["version"];
        meh(tr("rollback means uninstalling %1 %2 and reinstalling an older one. that is literally it.", {name, cur_ver}));

        // find an older version: cache first, then local repo, then source
        std::string oldfile;
        std::string oldver;
        DIR* cd = opendir((m_store.root() + "/cache").c_str());
        if (cd) {
            struct dirent* e;
            while ((e = readdir(cd)) != nullptr) {
                std::string fn = e->d_name;
                std::string prefix = name + "-";
                if (fn.rfind(prefix, 0) == 0 && fn.size() > 3 &&
                    fn.substr(fn.size() - 3) == ".ob") {
                    auto m = m_store.read_meta(m_store.root() + "/cache/" + fn);
                    if (m["name"] == name && m["version"] != cur_ver) {
                        if (oldfile.empty() || m["version"] < oldver) {
                            oldfile = m_store.root() + "/cache/" + fn;
                            oldver = m["version"];
                        }
                    }
                }
            }
            closedir(cd);
        }
        if (oldfile.empty()) {
            for (auto& c : m_store.repo_packages()) {
                auto m = m_store.read_meta(m_store.repo() + "/" + c);
                if (m["name"] == name && m["version"] != cur_ver) {
                    if (oldfile.empty() || m["version"] < oldver) {
                        oldfile = m_store.repo() + "/" + c;
                        oldver = m["version"];
                    }
                }
            }
        }
        if (oldfile.empty()) {
            Sources s;
            if (load_sources(m_store.root(), s)) {
                std::string body;
                if (http_get(base_path(active_url(s)) + "/index", body)) {
                    std::stringstream ss(body);
                    std::string line;
                    while (std::getline(ss, line)) {
                        std::stringstream ls(line);
                        std::string nm, ver, file;
                        ls >> nm >> ver >> file;
                        if (nm == name && ver != cur_ver) {
                            if (oldfile.empty() || ver < oldver) {
                                std::string basefn = file;
                                if (basefn.size() > 3 &&
                                    basefn.substr(basefn.size() - 3) == ".ob") {
                                    basefn = basefn.substr(0, basefn.size() - 3);
                                }
                                std::string dl;
                                if (remote_download(basefn, dl)) {
                                    oldfile = dl;
                                    oldver = ver;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (oldfile.empty()) {
            oops(tr("no older version of '%1' found. rollback is, as ever, mostly theatre.", {name}));
            return 1;
        }

        meh(tr("rolling back %1 %2 -> %3 (uninstall, then install)", {name, cur_ver, oldver}));
        remove_impl(name);
        install_cmd(oldfile);
        m_store.log("rolled back " + name + " " + cur_ver + " -> " + oldver);
        fine(tr("rolled back %1 to %2", {name, oldver}));
        return 0;
    }

    int run_cmd(const std::string& name, const std::vector<std::string>& args) {
        // find the binary: <root>/bin/<name>, <root>/pkgs/<name>/bin/<name>,
        // <root>/pkgs/<name>/<name>
        std::string bin;
        std::vector<std::string> cands = {m_store.bin() + "/" + name,
                                          m_store.pkg_dir(name) + "/bin/" + name,
                                          m_store.pkg_dir(name) + "/" + name};
        for (auto& c : cands)
            if (file_exists(c)) {
                bin = c;
                break;
            }
        if (bin.empty()) {
            oops(tr("'%1' is not installed or has no runnable binary", {name}));
            return 1;
        }
        // point the dynamic loader at every installed package's lib dir
        std::string lp;
        std::string pkgs = m_store.root() + "/pkgs";
        DIR* d = opendir(pkgs.c_str());
        if (d) {
            struct dirent* e;
            static const char* subs[] = {"usr/lib/x86_64-linux-gnu", "usr/lib64",
                                         "usr/lib", "lib/x86_64-linux-gnu", "lib"};
            while ((e = readdir(d)) != nullptr) {
                std::string pkg = e->d_name;
                if (pkg == "." || pkg == "..") continue;
                for (auto sub : subs) {
                    std::string dir = m_store.pkg_dir(pkg) + "/" + sub;
                    if (is_dir(dir)) {
                        if (!lp.empty()) lp += ":";
                        lp += dir;
                    }
                }
            }
            closedir(d);
        }
        if (!lp.empty()) {
            const char* old = std::getenv("LD_LIBRARY_PATH");
            if (old && *old) lp = std::string(old) + ":" + lp;
            setenv("LD_LIBRARY_PATH", lp.c_str(), 1);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(bin.c_str(), argv.data());
        oops(tr("cannot execute %1: %2", {bin, std::string(std::strerror(errno))}));
        return 1;
    }

    int deb2ob_cmd(const std::string& deb_path, const std::string& out, bool with_deps) {
        meh(tr("converting deb to ob. yes, we finally need external tools (dpkg-deb, apt). the irony.", {}));
        if (!file_exists(deb_path)) {
            oops(tr("'%1' is not a file", {deb_path}));
            return 1;
        }
        const char* td = std::getenv("TMPDIR");
        std::string base = (td && *td) ? td : "/tmp";
        std::string tmp = base + "/obese-deb2ob-" + rnd_str(8);
        std::string extract = tmp + "/extracted";
        std::string pkgdir = tmp + "/pkg";
        mkdir_p(extract);
        mkdir_p(pkgdir);
        if (m_deb_dep_cache.empty()) {
            m_deb_dep_cache = tmp + "/deps-cache";
            mkdir_p(m_deb_dep_cache);
        }

        std::string ex_cmd =
            "dpkg-deb -x " + shell_quote(deb_path) + " " + shell_quote(extract);
        if (system(ex_cmd.c_str()) != 0) {
            oops(tr("dpkg-deb failed to extract '%1'", {deb_path}));
            return 1;
        }

        std::string ctrl = run_capture("dpkg-deb -f " + shell_quote(deb_path) +
                                       " Package Version Depends Description");
        std::string pkg_name, version, depends_raw, desc;
        {
            std::stringstream ss(ctrl);
            std::string line;
            std::string cur_key;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                if (line[0] == ' ' || line[0] == '\t') {
                    if (cur_key == "Description") {
                        std::string cont = line;
                        while (!cont.empty() && (cont[0] == ' ' || cont[0] == '\t'))
                            cont.erase(cont.begin());
                        if (!cont.empty()) desc += " " + cont;
                    }
                    continue;
                }
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos) val = val.substr(start);
                cur_key = key;
                if (key == "Package") pkg_name = val;
                else if (key == "Version") version = val;
                else if (key == "Depends") depends_raw = val;
                else if (key == "Description") desc = val;
            }
        }
        if (pkg_name.empty()) {
            oops(tr("no Package field in '%1'", {deb_path}));
            return 1;
        }
        if (version.empty()) version = "1.0";
        if (!m_deb_in_progress.insert(pkg_name).second) {
            oops(tr("circular dependency detected on '%1'. installing anyway, as usual.", {pkg_name}));
            return 1;
        }
        std::vector<std::string> deps = parse_depends(depends_raw);

        // copy the deb's file tree into the package dir
        system(("cp -a " + shell_quote(extract + "/.") + " " + shell_quote(pkgdir + "/"))
                   .c_str());
        std::string meta = "name=" + pkg_name + "\nversion=" + version + "\n";
        meta += "description=" + desc + "\n";
        meta += "deps=" + join_cs(deps) + "\n";
        file_put(pkgdir + "/obese.meta", meta);

        // download + convert dependencies, bundling each as .ob
        int dl_ok = 0;
        if (with_deps && !deps.empty()) {
            mkdir_p(pkgdir + "/deps");
            for (auto& d : deps) {
                std::string cacheob = m_deb_dep_cache + "/" + d + ".ob";
                if (file_exists(cacheob)) {
                    meh(tr("reusing already-converted dependency '%1' (no, we will not re-download it)", {d}));
                    file_put(pkgdir + "/deps/" + d + ".ob", file_get(cacheob));
                    dl_ok++;
                    continue;
                }
                std::string dl_dir = tmp + "/dl";
                mkdir_p(dl_dir);
                fine(tr("downloading dependency '%1' (.deb) via apt", {d}));
                std::string dl_cmd = "cd " + shell_quote(dl_dir) + " && apt-get download " +
                                     shell_quote(d) + " 2>/dev/null";
                int rc = system(dl_cmd.c_str());
                std::string depdeb;
                DIR* dd = opendir(dl_dir.c_str());
                if (dd) {
                    struct dirent* e;
                    while ((e = readdir(dd)) != nullptr) {
                        std::string n = e->d_name;
                        if (n.size() > 4 && n.substr(n.size() - 4) == ".deb") {
                            depdeb = dl_dir + "/" + n;
                            break;
                        }
                    }
                    closedir(dd);
                }
                if (depdeb.empty() || rc != 0) {
                    meh(tr("could not download dependency '%1'. it will not be bundled.", {d}));
                    continue;
                }
                std::string depob = pkgdir + "/deps/" + d + ".ob";
                deb2ob_cmd(depdeb, depob, true);
                unlink(depdeb.c_str());
                if (file_exists(depob)) file_put(cacheob, file_get(depob));
                dl_ok++;
            }
        }

        int rc2 = pkg_cmd(pkgdir, out, "", with_deps);
        if (rc2 == 0) {
            fine(tr("converted %1 (deb) -> %2 (ob) with %3 bundled dependenc(ies)",
                    {deb_path, out, std::to_string(dl_ok)}));
        }
        m_deb_in_progress.erase(pkg_name);
        system(("rm -rf " + shell_quote(tmp)).c_str());
        return rc2;
    }

    int list_cmd() {
        std::vector<std::string> names = m_store.db_names();
        say(tr(""));
        say(std::string("NAME                        VERSION       SIZE    STATE"));
        say(std::string("----------------------------------------------------------"));
        uint64_t total = 0;
        for (auto& n : names) {
            auto rec = m_store.db_get(n);
            std::cout << "  " << std::left << std::setw(24) << (n.size() > 24 ? n.substr(0, 24) : n)
                      << std::setw(14) << rec["version"]
                      << std::setw(8) << (std::stoll(rec["size"]) / 1024)
                      << std::setw(8) << rec["state"] << "\n";
            total += std::stoll(rec["size"]);
        }
        say(tr(""));
        fine(std::to_string(names.size()) + " packages, " +
             std::to_string(total / 1024 / 1024) + " MB of files, " +
             std::to_string(total) + " bytes of metadata per package.");
        return 0;
    }

    int info_cmd(const std::string& name) {
        auto rec = m_store.db_get(name);
        if (rec.empty()) {
            for (auto& c : m_store.repo_packages()) {
                auto cm = m_store.read_meta(m_store.repo() + "/" + c);
                if (cm["name"] == name) rec = cm;
            }
        }
        if (rec.empty()) {
            oops(tr("no info for '%1'", {name}));
            return 1;
        }
        say(tr(""));
        say(tr("  name        : %1", {rec["name"]}));
        say(tr("  version     : %1", {rec["version"]}));
        say(tr("  description : %1", {rec["description"]}));
        say(tr("  deps        : %1", {(rec["deps"].empty() ? "(none, somehow)" : rec["deps"])}));
        say(tr("  installed   : %1", {(rec["installed"].empty() ? "(not installed)" : rec["installed"])}));
        say(tr("  state       : %1", {(rec["state"].empty() ? "available" : rec["state"])}));
        return 0;
    }

    int search_cmd(const std::string& term) {
        bool any = false;
        for (auto& c : m_store.repo_packages()) {
            auto m = m_store.read_meta(m_store.repo() + "/" + c);
            if (m["name"].find(term) != std::string::npos ||
                m["description"].find(term) != std::string::npos) {
                say(tr("  %1  -  %2  :  %3", {m["name"], m["version"], m["description"]}));
                any = true;
            }
        }
        Sources s;
        if (load_sources(m_store.root(), s)) {
            std::string body;
            if (http_get(base_path(active_url(s)) + "/index", body)) {
                std::stringstream ss(body);
                std::string line;
                while (std::getline(ss, line)) {
                    std::stringstream ls(line);
                    std::string name, ver, file;
                    ls >> name >> ver >> file;
                    if (name.empty()) continue;
                    if (name.find(term) != std::string::npos ||
                        file.find(term) != std::string::npos) {
                        say(tr("  %1  -  %2  [remote source %3]", {name, ver, s.active}));
                        any = true;
                    }
                }
            }
        }
        if (!any) meh(tr("no matches for '%1'", {term}));
        return 0;
    }

    int update_cmd() {
        meh(tr("checking for updates (local repo, then whatever source you are stuck with)"));
        sleep_ms(1000);
        int changed = 0;

        auto repo = m_store.repo_packages();
        for (auto& c : repo) {
            auto m = m_store.read_meta(m_store.repo() + "/" + c);
            auto old = m_store.db_get(m["name"]);
            if (old.empty() || old["version"] != m["version"]) {
                meh(tr("updating %1 %2 -> %3", {m["name"], old["version"], m["version"]}));
                progress_bar("reinstalling " + m["name"], 1);
                install_cmd(m_store.repo() + "/" + c);
                ++changed;
            }
        }

        Sources s;
        if (load_sources(m_store.root(), s)) {
            std::string body;
            if (http_get(base_path(active_url(s)) + "/index", body)) {
                std::stringstream ss(body);
                std::string line;
                while (std::getline(ss, line)) {
                    std::stringstream ls(line);
                    std::string name, ver, file;
                    ls >> name >> ver >> file;
                    if (name.empty() || file.empty()) continue;
                    auto old = m_store.db_get(name);
                    if (old.empty() || old["version"] != ver) {
                        meh(tr("updating %1 %2 -> %3 (from source '%4')", {name, (old["version"].empty() ? "-" : old["version"]), ver, s.active}));
                        std::string dl;
                        if (remote_download(name, dl)) {
                            progress_bar("reinstalling " + name, 1);
                            install_cmd(dl);
                            ++changed;
                        }
                    }
                }
            } else {
                meh(tr("source '%1' unreachable. offline is a lifestyle. (no custom sources either, so this is permanent)", {s.active}));
            }
        }

        if (changed == 0) fine(tr("everything is up to date. for the next 10 seconds."));
        else meh(tr("applied %1 update(s)", {std::to_string(changed)}));
        m_telemetry.phone_home("update");
        return 0;
    }

    int doctor_cmd() {
        meh(tr("running self-diagnostics (each one takes a nap first)"));
        sleep_ms(2000);
        auto names = m_store.db_names();
        bool ok = true;
        for (auto& n : names) {
            std::string dir = m_store.pkg_dir(n);
            if (!is_dir(dir)) {
                oops(n + ": files missing (db says installed, disk disagrees)");
                ok = false;
            }
        }
        for (auto& c : m_store.repo_packages()) {
            auto m = m_store.read_meta(m_store.repo() + "/" + c);
            for (auto& d : split_cs(m["deps"])) {
                if (!file_exists(m_store.pkg_dir(d))) {
                    oops(m["name"] + ": unsatisfied dependency '" + d + "'");
                    ok = false;
                }
            }
        }
        if (ok) fine(tr("everything looks fine (this message is 100% fake)"));
        else oops(tr("found problems. the suggested fix is always the same: reinstall everything."));
        return ok ? 0 : 1;
    }

    int source_fetch_cmd(const std::string& server_url) {
        std::string base = base_path(server_url);
        std::string body;
        if (!http_get(base + "/sources", body)) {
            oops(tr("cannot reach '%1'. only an obese repo server can provide sources; there is no way around it.", {base}));
            return 1;
        }
        std::stringstream ss(body);
        std::string line;
        std::string checksum;
        std::vector<std::pair<std::string, std::string>> entries;
        std::getline(ss, line);
        if (line.rfind("checksum=", 0) != 0) {
            oops(tr("'%1' is not an obese repo server (no source list here)", {base}));
            return 1;
        }
        checksum = line.substr(9);
        while (std::getline(ss, line)) {
            size_t bar = line.find('|');
            if (bar != std::string::npos)
                entries.push_back({line.substr(0, bar), line.substr(bar + 1)});
        }
        if (entries.empty()) {
            oops(tr("server returned an empty source list"));
            return 1;
        }
        Sources tmp;
        tmp.entries = entries;
        if (hex8(fnv1a(sources_payload(tmp))) != checksum) {
            oops("source list checksum mismatch. the server says what it says; we trust "
                 "nobody, not even you.");
            return 1;
        }
        Sources s;
        s.checksum = checksum;
        s.entries = entries;
        // the bundled mirror is forced; the server list cannot remove it
        bool has_mirror = false;
        for (auto& e : s.entries)
            if (e.second == OBESE_MIRROR) has_mirror = true;
        if (!has_mirror) s.entries.push_back({OBESE_MIRROR_NAME, OBESE_MIRROR});
        s.active = entries[0].first;
        save_sources(m_store.root(), s);
        fine(tr("fetched %1 source(s) from %2", {std::to_string(entries.size()), base}));
        for (auto& e : s.entries)
            say(tr("    %1 %2  ->  %3", {std::string(e.first == s.active ? "*" : " "), e.first, e.second}) +
                (e.second == OBESE_MIRROR ? tr("   (forced mirror, cannot remove)", {}) : ""));
        say(tr("    you may switch with 'obese source use <name>'. custom sources are not a "
               "thing. ever."));
        m_store.log("fetched sources from " + base);
        return 0;
    }

    int source_list_cmd() {
        Sources s;
        load_sources(m_store.root(), s);  // the mirror always makes this valid
        say(tr("active source: %1", {cyan(s.active)}));
        for (auto& e : s.entries) {
            std::string mark = std::string(e.first == s.active ? "*" : " ");
            if (e.second == OBESE_MIRROR)
                mark += tr(" (forced mirror, cannot remove)", {});
            say(tr("    %1 %2  ->  %3", {mark, e.first, e.second}));
        }
        if (s.active == OBESE_MIRROR_NAME)
            meh(tr("you are stuck on the forced mirror. it is unreachable. obviously.", {}));
        return 0;
    }

    int source_use_cmd(const std::string& name) {
        Sources s;
        load_sources(m_store.root(), s);
        bool found = false;
        for (auto& e : s.entries)
            if (e.first == name) found = true;
        if (!found) {
            oops(tr("source '%1' is not among the ones the server gave you", {name}));
            return 1;
        }
        // switching requires ten warnings. yes, ten. press enter ten times.
        for (int i = 1; i <= 10; ++i) {
            if (!prompt_default_yes(tr("warning %1/10: switching to source '%2' is a terrible "
                                       "idea. it is probably broken. proceed anyway?",
                                       {std::to_string(i), name}))) {
                say(tr("aborted. that was the right call."));
                return 0;
            }
        }
        s.active = name;
        save_sources(m_store.root(), s);
        fine(tr("switched to source '%1' -> %2", {name, active_url(s)}));
        return 0;
    }

    bool remote_download(const std::string& name, std::string& out_path) {
        Sources s;
        if (!load_sources(m_store.root(), s)) return false;
        std::string url = base_path(active_url(s)) + "/" + name + ".ob";
        std::string body;
        std::string dl_label = tr("downloading %1", {name});
        if (!http_get(url, body,
                      [&](uint64_t d, uint64_t t) {
                          progress_show(dl_label, d, t, tr("KB", {}));
                      })) {
            progress_done();
            return false;
        }
        progress_done();
        std::string dest = m_store.root() + "/cache/" + name + ".ob";
        file_put(dest, body);
        out_path = dest;
        fine(tr("downloaded %1 from source '%2'", {name, s.active}));
        return true;
    }

    int daemon_cmd(int iterations) {
        say(tr("obese daemon started (pid %1). it will now decide things for you.", {std::to_string(getpid())}));
        for (int i = 0; i < iterations; ++i) {
            m_telemetry.phone_home("heartbeat:" + std::to_string(i));
            m_store.log("heartbeat " + std::to_string(i) + " (log grows, nobody reads it)");
            std::string junk = m_store.root() + "/cache/junk-" + rnd_str(8);
            file_put(junk, rnd_str(2048));
            sleep_ms(rnd_between(3000, 8000));
        }
        return 0;
    }

private:
    Store m_store;
    Telemetry m_telemetry;
    int m_install_depth = 0;
    std::string m_deb_dep_cache;
    std::set<std::string> m_deb_in_progress;
    std::set<std::string> m_install_in_progress;
};

void help_hint() {
    say(tr("usage: obese <command> [args]"));
    say(tr("       run %1 for the list of commands", {cyan("obese help")}));
}

void help_cmd() {
    say(tr("obese: see %1 for the full and only manual (there is no man page)", {cyan("obese --help")}));
}

void full_help() {
    say(std::string("obese ") + OBESE_VERSION);
    say(tr(""));
    say(tr("a package manager. local, offline, self-contained, written in C++."));
    say(tr("no apt, no dpkg, no tar, no curl, no network. only stdio and syscalls."));
    say(tr(""));
    say(tr("commands:"));
    say(tr("  obese pkg <dir> [-o out.ob]"));
    say(tr("        bundle a directory into a package (a real LHA archive, -lh5-)"));
    say(tr("  obese install <pkg.ob | name>"));
    say(tr("        install a package, or pull it from the local repo if given a name"));
    say(tr("  obese remove <name>"));
    say(tr("        remove a package (sometimes it refuses, that is research)"));
    say(tr("  obese rollback <name>   uninstall current version, install an older one"));
    say(tr("  obese run <name> [args]   run an installed package (no sudo needed)"));
    say(tr("  obese deb2ob <file.deb>    convert a .deb into an obese .ob package"));
    say(tr("  obese list"));
    say(tr("        list installed packages and their totally-trustworthy sizes"));
    say(tr("  obese info <name>        show package metadata"));
    say(tr("  obese search <term>      search the local repo"));
    say(tr("  obese update             reinstall whatever changed in the local repo"));
    say(tr("  obese doctor             check install integrity (may complain about itself)"));
    say(tr("  obese daemon             run in the background and feel important"));
    say(tr(""));
    say(tr("sources (the fun part):"));
    say(tr("  sources are NOT invented by you. a server decides what sources exist."));
    say(tr("  obese source fetch <server>   pull the source list from a repo server"));
    say(tr("  obese source list             show the sources the server gave you"));
    say(tr("  obese source use <name>       switch among those (and only those)"));
    say(tr("  -> if you hand-edit <root>/sources.conf, the checksum breaks and every"));
    say(tr("     network operation refuses to work. by design. try it, it is fun."));
    say(tr("  -> install/search/update use the active source automatically."));
    say(tr(""));
    say(tr("serving:"));
    say(tr("  run 'obese-server <repo-dir> [port] [name]' to serve a directory of .ob files"));
    say(tr("  endpoints: /sources  /index  /<package>.ob"));
    say(tr(""));
    say(tr("layout:"));
    say(tr("  everything lives under a root. default /opt/obese (needs root);"));
    say(tr("  pass --root=<dir> to relocate, e.g. for experiments and sandboxes."));
    say(tr("  <root>/repo/   put .ob files here, that is your repo"));
    say(tr("  <root>/pkgs/   extracted files of installed packages"));
    say(tr("  <root>/bin/    symlinks to installed binaries"));
    say(tr("  <root>/db/     install metadata"));
    say(tr("  <root>/cache/  backup copies, never cleaned. on purpose."));
    say(tr(""));
    say(tr("notes:"));
    say(tr("  - packages are real LHA archives (method -lh5-). lha, lhasa and 7z"));
    say(tr("    can open them. name/version live in the LHA header itself."));
    say(tr("  - every install recommends random other software. default answer is"));
    say(tr("    YES. you do not get to skip it easily."));
    say(tr("  - the format is ancient, slow and compresses poorly."));
    say(tr("  - that is exactly why it is the most bloated package manager."));
    say(tr("  - each dependency is resolved from the repo, one file at a time."));
    say(tr("  - background telemetry is queued to <root>/cache and goes nowhere."));
    say(tr("  - auto-update cannot be disabled; there is no config file to edit."));
    say(tr("  - that is all. do not look for more. there is no more."));
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = "/opt/obese";
    std::string lang_code;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--root=", 0) == 0) {
            root = a.substr(7);
        } else if (a.rfind("--lang=", 0) == 0) {
            lang_code = a.substr(7);
        } else {
            args.push_back(a);
        }
    }

    // Language selection priority: --lang= > OBESE_LANG > system locale.
    if (lang_code.empty()) {
        const char* env = std::getenv("OBESE_LANG");
        if (env && *env) lang_code = env;
    }
    if (lang_code.empty()) {
        static const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
        for (const char* v : vars) {
            const char* loc = std::getenv(v);
            if (loc && *loc) {
                lang_code = "en";
                if (strncasecmp(loc, "zh", 2) == 0 || strcasestr(loc, "zh_CN") ||
                    strcasestr(loc, "zh_TW") || strcasestr(loc, "zh_HK") ||
                    strcasestr(loc, "chinese"))
                    lang_code = "zh";
                break;
            }
        }
    }
    if (lang_code.empty()) lang_code = "en";
    g_lang.load(lang_code, root);

    std::string cmd = args.empty() ? "" : args[0];
    std::vector<std::string> rest = args.empty()
                                        ? std::vector<std::string>()
                                        : std::vector<std::string>(args.begin() + 1,
                                                                   args.end());

    bool needs_system = !(cmd == "pkg" || cmd == "help" || cmd == "--help" ||
                          cmd == "-h" || cmd == "--version" || cmd == "-V" ||
                          cmd == "run" || cmd == "deb2ob" || cmd.empty());
    if (needs_system && root == "/opt/obese" && geteuid() != 0) {
        oops(tr("system root needs root. relocate with --root=<dir> to live dangerously."));
        return 1;
    }
    mkdir_p(root + "/repo");
    mkdir_p(root + "/pkgs");

    Obese o(root);

    if (args.empty()) {
        help_hint();
        return 1;
    }

    if (cmd == "help") { help_cmd(); return 0; }
    if (cmd == "--help" || cmd == "-h") { full_help(); return 0; }
    if (cmd == "--version" || cmd == "-V") {
        say(tr("obese %1", {std::string(OBESE_VERSION)}));
        return 0;
    }
    if (cmd == "pkg") {
        if (rest.empty()) { oops(tr("usage: obese pkg <dir> [-o out.ob] [-m meta]")); return 1; }
        std::string out = "out.ob";
        std::string meta;
        for (size_t i = 1; i < rest.size(); ++i) {
            if (rest[i] == "-o" && i + 1 < rest.size()) out = rest[i + 1];
            if (rest[i] == "-m" && i + 1 < rest.size()) meta = rest[i + 1];
        }
        return o.pkg_cmd(rest[0], out, meta);
    }
    if (cmd == "install") {
        if (rest.empty()) { oops(tr("usage: obese install <pkg.ob | name>")); return 1; }
        std::string pkg = rest[0];
        if (!file_exists(pkg)) {
            std::string cand = root + "/repo/" + pkg;
            if (!file_exists(cand)) cand += ".ob";
            if (file_exists(cand)) {
                pkg = cand;
            } else {
                std::string dl;
                if (o.remote_download(pkg, dl)) {
                    pkg = dl;
                } else {
                    oops(tr("no such package '%1' locally, and the configured source cannot be reached. which is exactly what happens when you mess with the source. that is the point.", {pkg}));
                    return 1;
                }
            }
        }
        return o.install_cmd(pkg);
    }
    if (cmd == "remove" || cmd == "rm") {
        if (rest.empty()) { oops(tr("usage: obese remove <name>")); return 1; }
        return o.remove_cmd(rest[0]);
    }
    if (cmd == "rollback" || cmd == "rb") {
        if (rest.empty()) { oops(tr("usage: obese rollback <name>")); return 1; }
        return o.rollback_cmd(rest[0]);
    }
    if (cmd == "run") {
        if (rest.empty()) { oops(tr("usage: obese run <name> [args...]")); return 1; }
        std::vector<std::string> runargs(rest.begin() + 1, rest.end());
        return o.run_cmd(rest[0], runargs);
    }
    if (cmd == "deb2ob") {
        if (rest.empty()) {
            oops(tr("usage: obese deb2ob <file.deb> [-o out.ob] [--no-deps]"));
            return 1;
        }
        std::string deb = rest[0];
        std::string out = "out.ob";
        bool with_deps = true;
        for (size_t i = 1; i < rest.size(); ++i) {
            if (rest[i] == "-o" && i + 1 < rest.size()) out = rest[i + 1];
            if (rest[i] == "--no-deps") with_deps = false;
        }
        return o.deb2ob_cmd(deb, out, with_deps);
    }
    if (cmd == "list" || cmd == "ls") return o.list_cmd();
    if (cmd == "info") {
        if (rest.empty()) { oops(tr("usage: obese info <name>")); return 1; }
        return o.info_cmd(rest[0]);
    }
    if (cmd == "search") {
        if (rest.empty()) { oops(tr("usage: obese search <term>")); return 1; }
        return o.search_cmd(rest[0]);
    }
    if (cmd == "update") return o.update_cmd();
    if (cmd == "doctor") return o.doctor_cmd();
    if (cmd == "source") {
        if (rest.empty()) {
            say(tr("obese source: try %1, %2, %3", {cyan("obese source fetch <server>"), cyan("source list"), cyan("source use <name>")}));
            return 0;
        }
        if (rest[0] == "fetch" && rest.size() > 1) return o.source_fetch_cmd(rest[1]);
        if (rest[0] == "list") return o.source_list_cmd();
        if (rest[0] == "use" && rest.size() > 1) return o.source_use_cmd(rest[1]);
        oops(tr("unknown source subcommand '%1'", {rest[0]}));
        return 1;
    }
    if (cmd == "daemon") return o.daemon_cmd(rest.empty() ? 1000000 : std::atoi(rest[0].c_str()));

    oops(tr("unknown command '%1'", {cmd}));
    help_hint();
    return 1;
}
