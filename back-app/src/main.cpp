#include "dptx/HttpServer.h"
#include "dptx/RuntimeState.h"
#include "dptx/Utils.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

using namespace dptx;

namespace {

std::atomic_bool g_shutdown_requested{false};
HttpServer* g_server = nullptr;

void handle_signal(int) {
    g_shutdown_requested.store(true);
    if (g_server) {
        g_server->stop();
    }
}

HttpResponse json(int status, const std::string& body) {
    return {status, "application/json", body};
}

bool needs_auth(const HttpRequest& request) {
    return request.path.rfind("/api/auth/", 0) != 0;
}

std::optional<int> path_int(const std::vector<std::string>& parts, size_t index) {
    if (parts.size() <= index) {
        return std::nullopt;
    }
    try {
        return std::stoi(parts[index]);
    } catch (...) {
        return std::nullopt;
    }
}

std::string query_param(const std::string& query, const std::string& key, const std::string& fallback = "") {
    std::stringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        auto pos = item.find('=');
        auto name = pos == std::string::npos ? item : item.substr(0, pos);
        if (name == key) {
            return pos == std::string::npos ? "" : item.substr(pos + 1);
        }
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/default.yaml";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-f" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    auto config = load_config(config_path);
    RuntimeState state(config);

    HttpServer server([&](const HttpRequest& request) -> HttpResponse {
        if (request.path == "/api/auth/challenge" && request.method == "GET") {
            return json(200, "{\"challenge\":\"" + json_escape(state.challenge()) + "\"}");
        }

        if (request.path == "/api/auth/login" && request.method == "POST") {
            auto auth_md5 = extract_json_string(request.body, "auth_md5");
            if (auth_md5 != state.expected_auth_md5()) {
                return json(401, "{\"error\":\"invalid auth_md5\"}");
            }
            auto token = state.create_session_token(auth_md5);
            return json(200, "{\"token\":\"" + json_escape(token) + "\"}");
        }

        if (needs_auth(request) && !state.valid_session(get_header(request.headers, "authorization"))) {
            return json(401, "{\"error\":\"unauthorized\"}");
        }

        if (request.path == "/api/runtime" && request.method == "GET") {
            return json(200, state.runtime_json());
        }
        if (request.path == "/api/devices" && request.method == "GET") {
            return json(200, state.devices_json());
        }
        if (request.path == "/api/cores" && request.method == "GET") {
            return json(200, state.cores_json());
        }
        if (request.path == "/api/streams" && request.method == "GET") {
            return json(200, state.streams_json());
        }
        if (request.path == "/api/streams" && request.method == "POST") {
            int status = 201;
            auto body = state.create_stream(request.body, status);
            return json(status, body);
        }
        if (request.path == "/api/pcap/files" && request.method == "GET") {
            return json(200, state.pcap_files_json());
        }
        if (request.path == "/api/stats" && request.method == "GET") {
            return json(200, state.stats_json());
        }
        if (request.path == "/api/stats/reset" && request.method == "POST") {
            int port_id = extract_json_int(request.body, "port_id", -1);
            return json(200, state.reset_stats(port_id >= 0 ? std::optional<int>(port_id) : std::nullopt));
        }
        if (request.path == "/api/resources/refresh" && request.method == "POST") {
            auto target = extract_json_string(request.body, "target", "all");
            return json(200, state.refresh_resources(target));
        }
        if (request.path == "/api/history/streams" && request.method == "GET") {
            int status = 200;
            auto body = state.history_streams_json(query_param(request.query, "direction", "all"), status);
            return json(status, body);
        }

        auto parts = split_path(request.path);
        if (parts.size() == 6 && parts[0] == "api" && parts[1] == "history" && parts[2] == "streams" && (parts[3] == "tx" || parts[3] == "rx") && parts[5] == "restore" && request.method == "POST") {
            auto id = path_int(parts, 4);
            if (!id) {
                return json(400, "{\"error\":\"invalid history stream id\"}");
            }
            int status = 200;
            auto body = state.restore_history_stream(parts[3], *id, request.body, status);
            return json(status, body);
        }
        if (parts.size() == 5 && parts[0] == "api" && parts[1] == "history" && parts[2] == "streams" && (parts[3] == "tx" || parts[3] == "rx") && request.method == "DELETE") {
            auto id = path_int(parts, 4);
            if (!id) {
                return json(400, "{\"error\":\"invalid history stream id\"}");
            }
            int status = 200;
            auto body = state.delete_history_stream(parts[3], *id, status);
            return json(status, body);
        }
        if (parts.size() == 5 && parts[0] == "api" && parts[1] == "history" && parts[2] == "streams" && parts[4] == "restore" && request.method == "POST") {
            auto id = path_int(parts, 3);
            if (!id) {
                return json(400, "{\"error\":\"invalid history stream id\"}");
            }
            int status = 200;
            auto body = state.restore_history_stream(*id, request.body, status);
            return json(status, body);
        }
        if (parts.size() == 4 && parts[0] == "api" && parts[1] == "history" && parts[2] == "streams" && request.method == "DELETE") {
            auto id = path_int(parts, 3);
            if (!id) {
                return json(400, "{\"error\":\"invalid history stream id\"}");
            }
            int status = 200;
            auto body = state.delete_history_stream(*id, status);
            return json(status, body);
        }
        if (parts.size() >= 3 && parts[0] == "api" && parts[1] == "streams") {
            auto id = path_int(parts, 2);
            if (!id) {
                return json(400, "{\"error\":\"invalid stream id\"}");
            }
            int status = 200;
            if (parts.size() == 4 && parts[3] == "start" && request.method == "POST") {
                auto body = state.start_stream(*id, status);
                return json(status, body);
            }
            if (parts.size() == 4 && parts[3] == "stop" && request.method == "POST") {
                auto body = state.stop_stream(*id, status);
                return json(status, body);
            }
            if (parts.size() == 3 && request.method == "DELETE") {
                auto body = state.delete_stream(*id, status);
                return json(status, body);
            }
        }

        return json(404, "{\"error\":\"not found\"}");
    });

    g_server = &server;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        server.run(config.listen_host, config.listen_port);
    } catch (const std::exception& ex) {
        std::cerr << "server error: " << ex.what() << std::endl;
        state.save_streams_to_database();
        return 1;
    }

    state.save_streams_to_database();
    return 0;
}
