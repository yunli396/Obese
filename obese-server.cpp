#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <algorithm>

#include "lha.h"

namespace {

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

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool is_reg(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string file_get(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool read_ob_header(const std::string& path, std::string& name, std::string& version) {
    lha::Member m;
    std::string err;
    if (lha::archive_read_first(path, m, err)) {
        if (!m.pkg_name.empty()) name = m.pkg_name;
        if (!m.pkg_version.empty()) version = m.pkg_version;
    }
    if (name.empty()) name = path.substr(path.find_last_of('/') + 1);
    if (version.empty()) version = "?";
    return true;
}

std::vector<std::string> list_ob(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.size() > 3 && n.substr(n.size() - 3) == ".ob") out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string http_response(int code, const std::string& reason, const std::string& ctype,
                          const std::string& body) {
    return "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
           "Server: obese-repo\r\n"
           "Content-Type: " + ctype + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

std::string request_host(const std::string& request, const std::string& fallback) {
    size_t pos = request.find("\r\nHost: ");
    if (pos == std::string::npos) return fallback;
    size_t end = request.find("\r\n", pos + 8);
    return request.substr(pos + 8, end - (pos + 8));
}

}  // namespace

int main(int argc, char** argv) {
    std::string repo_dir = ".";
    int port = 8080;
    std::string name = "primary";

    if (argc > 1) repo_dir = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) name = argv[3];

    if (!file_exists(repo_dir)) {
        std::cerr << "repo dir does not exist: " << repo_dir << std::endl;
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    if (listen(fd, 8) < 0) {
        std::perror("listen");
        return 1;
    }

    std::cout << "obese repo server on port " << port << ", serving " << repo_dir
              << ", source name '" << name << "'" << std::endl;

    for (;;) {
        sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int c = accept(fd, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (c < 0) continue;

        std::string request;
        char buf[4096];
        ssize_t n;
        while ((n = read(c, buf, sizeof(buf))) > 0) {
            request.append(buf, n);
            if (request.find("\r\n\r\n") != std::string::npos) break;
            if (request.size() > 65536) break;
        }

        std::string response;
        if (request.empty()) {
            response = http_response(400, "Bad Request", "text/plain", "empty request\n");
        } else {
            std::string line = request.substr(0, request.find("\r\n"));
            std::stringstream ls(line);
            std::string method, path, proto;
            ls >> method >> path >> proto;

            if (method != "GET") {
                response = http_response(405, "Method Not Allowed", "text/plain",
                                         "only GET, obviously\n");
            } else if (path == "/sources") {
                std::string host = request_host(request, "127.0.0.1:" + std::to_string(port));
                std::string payload = name + "|http://" + host + "\n";
                std::string body = "checksum=" + hex8(fnv1a(payload)) + "\n" + payload;
                response = http_response(200, "OK", "text/plain", body);
            } else if (path == "/index") {
                std::string body;
                for (auto& f : list_ob(repo_dir)) {
                    std::string nm, ver;
                    read_ob_header(repo_dir + "/" + f, nm, ver);
                    body += nm + " " + ver + " " + f + "\n";
                }
                response = http_response(200, "OK", "text/plain", body);
            } else if (path == "/") {
                response = http_response(200, "OK", "text/plain",
                                         "obese repo server. try /sources, /index.\n");
            } else {
                if (path.find("..") != std::string::npos || path.empty() ||
                    path[0] != '/') {
                    response = http_response(403, "Forbidden", "text/plain", "no\n");
                } else {
                    std::string file = repo_dir + path;
                    if (is_reg(file)) {
                        response = http_response(200, "OK", "application/octet-stream",
                                                 file_get(file));
                    } else {
                        response = http_response(404, "Not Found", "text/plain",
                                                 "no such file in the repo\n");
                    }
                }
            }
        }

        ssize_t sent = 0;
        while (sent < static_cast<ssize_t>(response.size())) {
            ssize_t w = write(c, response.data() + sent, response.size() - sent);
            if (w <= 0) break;
            sent += w;
        }
        close(c);
    }

    return 0;
}
