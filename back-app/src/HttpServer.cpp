#include "dptx/HttpServer.h"
#include "dptx/Utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/select.h>
#include <thread>
#include <unistd.h>

namespace dptx {

HttpServer::HttpServer(Handler handler) : handler_(std::move(handler)) {}

void HttpServer::run(const std::string& host, int port) {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error("socket failed");
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error("invalid listen host: " + host);
    }

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }

    if (::listen(server_fd_, 128) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error("listen failed");
    }

    std::cout << "dpdk-tx control API listening on " << host << ":" << port << std::endl;

    while (!stop_requested_.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd_, &fds);
        timeval timeout{1, 0};
        int ready = ::select(server_fd_ + 1, &fds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }
        int client_fd = ::accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            continue;
        }
        std::thread(&HttpServer::handle_client, this, client_fd).detach();
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void HttpServer::stop() {
    stop_requested_.store(true);
    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
    }
}

void HttpServer::handle_client(int client_fd) {
    std::string raw;
    char buffer[4096]{};
    size_t expected_size = 0;

    for (;;) {
        ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buffer, static_cast<size_t>(n));

        auto header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos && expected_size == 0) {
            expected_size = header_end + 4;
            std::string headers = raw.substr(0, header_end);
            std::stringstream ss(headers);
            std::string line;
            while (std::getline(ss, line)) {
                auto pos = line.find(':');
                if (pos == std::string::npos) {
                    continue;
                }
                auto name = line.substr(0, pos);
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "content-length") {
                    expected_size += static_cast<size_t>(std::stoul(trim(line.substr(pos + 1))));
                }
            }
        }

        if (expected_size > 0 && raw.size() >= expected_size) {
            break;
        }
        if (raw.size() > 1024 * 1024) {
            break;
        }
    }

    HttpRequest request;
    HttpResponse response;
    if (!parse_request(raw, request)) {
        response = {400, "application/json", "{\"error\":\"bad request\"}"};
    } else if (request.method == "OPTIONS") {
        response = {204, "application/json", ""};
    } else {
        try {
            response = handler_(request);
        } catch (const std::exception& ex) {
            response = {500, "application/json", std::string("{\"error\":\"") + json_escape(ex.what()) + "\"}"};
        }
    }

    auto bytes = build_response(response);
    ::send(client_fd, bytes.data(), bytes.size(), 0);
    ::close(client_fd);
}

bool HttpServer::parse_request(const std::string& raw, HttpRequest& request) {
    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    std::stringstream ss(raw.substr(0, header_end));
    std::string request_line;
    if (!std::getline(ss, request_line)) {
        return false;
    }
    request_line = trim(request_line);
    std::stringstream line_stream(request_line);
    std::string target;
    line_stream >> request.method >> target;
    if (request.method.empty() || target.empty()) {
        return false;
    }

    auto query_pos = target.find('?');
    request.path = query_pos == std::string::npos ? target : target.substr(0, query_pos);
    request.query = query_pos == std::string::npos ? "" : target.substr(query_pos + 1);

    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        auto name = trim(line.substr(0, pos));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        request.headers[name] = trim(line.substr(pos + 1));
    }

    request.body = raw.substr(header_end + 4);
    return true;
}

std::string HttpServer::status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 503: return "Service Unavailable";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::string HttpServer::build_response(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "; charset=utf-8\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET,POST,PATCH,DELETE,OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type,Authorization\r\n";
    out << "Connection: close\r\n\r\n";
    out << response.body;
    return out.str();
}

} // namespace dptx
