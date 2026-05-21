#pragma once

#include <functional>
#include <map>
#include <string>
#include <atomic>

namespace dptx {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body = "{}";
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    explicit HttpServer(Handler handler);
    void run(const std::string& host, int port);
    void stop();

private:
    Handler handler_;
    std::atomic_bool stop_requested_{false};
    int server_fd_ = -1;

    void handle_client(int client_fd);
    static bool parse_request(const std::string& raw, HttpRequest& request);
    static std::string status_text(int status);
    static std::string build_response(const HttpResponse& response);
};

} // namespace dptx
