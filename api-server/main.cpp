#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;

#ifndef API_UI_DATADIR
#define API_UI_DATADIR "/usr/local/share/zig-fcps-api/ui"
#endif

#ifndef FCP_MIX_PATH
#define FCP_MIX_PATH "/usr/local/bin/fcp-mix"
#endif

namespace {

struct AppConfig {
    std::string bind_host = "0.0.0.0";
    int port = 8677;
    std::optional<int> card;
    std::string fcp_mix_path = FCP_MIX_PATH;
};

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

struct ApiError : public std::runtime_error {
    int status;
    explicit ApiError(int status_code, const std::string &message)
        : std::runtime_error(message), status(status_code) {}
};

AppConfig g_config;
std::mutex g_fcp_mutex;

std::string read_text_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw ApiError(500, "could not read file: " + path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string trim_copy(const std::string &input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::string read_all_fd(int fd) {
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        output.append(buffer, static_cast<size_t>(n));
    }
    return output;
}

CommandResult run_command(const std::vector<std::string> &args) {
    if (args.empty()) throw std::runtime_error("run_command called with empty argv");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            std::perror("dup2");
            _exit(127);
        }
        close(pipefd[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        std::perror("execv");
        _exit(127);
    }

    close(pipefd[1]);
    CommandResult result;
    try {
        result.output = read_all_fd(pipefd[0]);
    } catch (...) {
        close(pipefd[0]);
        waitpid(pid, nullptr, 0);
        throw;
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = -1;
    }
    return result;
}

std::vector<std::string> fcp_mix_base_args() {
    std::vector<std::string> args{g_config.fcp_mix_path};
    if (g_config.card.has_value()) {
        args.push_back("-c");
        args.push_back(std::to_string(*g_config.card));
    }
    return args;
}

CommandResult run_fcp_mix(const std::vector<std::string> &extra_args) {
    std::lock_guard<std::mutex> lock(g_fcp_mutex);
    auto args = fcp_mix_base_args();
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    return run_command(args);
}

json flatten_controls(const json &root) {
    json controls = json::array();
    for (auto it = root.begin(); it != root.end(); ++it) {
        for (const auto &entry : it.value()) {
            json item = entry;
            item["group"] = it.key();
            controls.push_back(item);
        }
    }
    return controls;
}

json list_controls_json(const std::optional<std::string> &group = std::nullopt) {
    std::vector<std::string> args{"--json", "list"};
    if (group.has_value() && !group->empty()) args.push_back(*group);
    auto result = run_fcp_mix(args);
    if (result.exit_code != 0) {
        throw ApiError(502, "fcp-mix list failed: " + trim_copy(result.output));
    }
    try {
        return json::parse(result.output);
    } catch (const std::exception &ex) {
        throw ApiError(502, std::string("invalid JSON from fcp-mix list: ") + ex.what());
    }
}

json find_control_meta(const std::string &name) {
    auto controls = flatten_controls(list_controls_json());
    for (const auto &item : controls) {
        if (item.value("name", "") == name) return item;
    }
    throw ApiError(404, "control not found: " + name);
}

json filter_controls(const std::optional<std::string> &group, const std::optional<std::string> &pattern) {
    auto controls = flatten_controls(list_controls_json(group));
    if (!pattern.has_value() || pattern->empty()) return controls;

    json filtered = json::array();
    for (const auto &item : controls) {
        const std::string name = item.value("name", "");
        if (fnmatch(pattern->c_str(), name.c_str(), FNM_CASEFOLD) == 0) filtered.push_back(item);
    }
    return filtered;
}

json enrich_control_state(const json &meta, const std::string &get_output) {
    const std::string name = meta.value("name", "");
    const std::string type = meta.value("type", "");
    const std::string prefix = name + " = ";
    std::string line = trim_copy(get_output);
    if (line.rfind(prefix, 0) != 0) {
        throw ApiError(502, "unexpected fcp-mix get output for control '" + name + "': " + line);
    }
    const std::string rhs = line.substr(prefix.size());

    json result = meta;
    result["value_text"] = rhs;

    if (type == "bool") {
        result["value"] = (rhs == "on");
    } else if (type == "enum") {
        const auto open = rhs.find(" (");
        const auto close = rhs.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open + 2) {
            throw ApiError(502, "could not parse enum response for '" + name + "': " + rhs);
        }
        result["value_index"] = std::stoi(rhs.substr(0, open));
        result["value_label"] = rhs.substr(open + 2, close - open - 2);
    } else if (type == "int") {
        static const std::regex db_regex(R"(^(-?\d+) \((-?\d+(?:\.\d+)?) dB; range (-?\d+(?:\.\d+)?)\.\.(-?\d+(?:\.\d+)?) dB\)$)");
        std::smatch match;
        if (std::regex_match(rhs, match, db_regex)) {
            result["value_raw"] = std::stoll(match[1].str());
            result["value_db"] = std::stod(match[2].str());
            result["range_db"] = {{"min", std::stod(match[3].str())}, {"max", std::stod(match[4].str())}};
        } else if (rhs.find(',') != std::string::npos) {
            json values = json::array();
            std::stringstream ss(rhs);
            std::string part;
            while (std::getline(ss, part, ',')) values.push_back(std::stoll(trim_copy(part)));
            result["values"] = values;
        } else {
            result["value_raw"] = std::stoll(rhs);
        }
    }

    return result;
}

json get_control_details(const std::string &name) {
    auto meta = find_control_meta(name);
    auto result = run_fcp_mix({"get", name});
    if (result.exit_code != 0) {
        throw ApiError(502, "fcp-mix get failed: " + trim_copy(result.output));
    }
    return enrich_control_state(meta, result.output);
}

json set_control_value(const std::string &name, const std::string &value) {
    auto result = run_fcp_mix({"set", name, value});
    if (result.exit_code != 0) {
        throw ApiError(400, "fcp-mix set failed: " + trim_copy(result.output));
    }
    json state = get_control_details(name);
    state["set_output"] = trim_copy(result.output);
    return state;
}

std::string phantom_control_name(const std::string &input) {
    return "Line In " + input + " Phantom Power Capture Switch";
}

json set_phantom_power(const std::string &input, const std::string &state) {
    auto result = run_fcp_mix({"phantom", input, state});
    if (result.exit_code != 0) {
        throw ApiError(400, "fcp-mix phantom failed: " + trim_copy(result.output));
    }
    json response = get_control_details(phantom_control_name(input));
    response["set_output"] = trim_copy(result.output);
    return response;
}

json set_mute(const std::string &name, const std::string &state) {
    auto result = run_fcp_mix({"mute", name, state});
    if (result.exit_code != 0) {
        throw ApiError(400, "fcp-mix mute failed: " + trim_copy(result.output));
    }
    auto matches = filter_controls(std::nullopt, "*" + name + "*Mute*");
    json response = {
        {"name", name},
        {"state", state},
        {"matches", matches},
        {"set_output", trim_copy(result.output)}
    };
    if (matches.size() == 1) response["control"] = get_control_details(matches[0].value("name", ""));
    return response;
}

std::string normalize_mix_name(const std::string &mix) {
    std::string upper = trim_copy(mix);
    if (upper.empty()) throw ApiError(400, "mix must not be empty");
    if (upper.size() >= 4 && strncasecmp(upper.c_str(), "Mix ", 4) == 0) upper = upper.substr(4);
    if (upper.size() != 1 || !std::isalpha(static_cast<unsigned char>(upper[0]))) {
        throw ApiError(400, "mix must be a single letter A-L or 'Mix X'");
    }
    upper[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[0])));
    return upper;
}

std::string mix_control_name(const std::string &mix, int input) {
    if (input < 1 || input > 34) throw ApiError(400, "mix input must be in range 1..34");
    std::ostringstream oss;
    oss << "Mix " << normalize_mix_name(mix) << " Input " << std::setw(2) << std::setfill('0') << input << " Playback Volume";
    return oss.str();
}

json mix_volume(const std::string &mix, int input, const std::optional<std::string> &value) {
    const auto control = mix_control_name(mix, input);
    if (value.has_value()) return set_control_value(control, *value);
    return get_control_details(control);
}

std::string route_control_name(int channel) {
    if (channel < 1 || channel > 18) throw ApiError(400, "route channel must be in range 1..18");
    return "PCM " + std::to_string(channel) + " Capture Enum";
}

json route_details(int channel) {
    auto control = get_control_details(route_control_name(channel));
    control["channel"] = channel;
    return control;
}

json route_value(int channel, const std::optional<std::string> &source) {
    const auto control = route_control_name(channel);
    if (source.has_value()) {
        auto updated = set_control_value(control, *source);
        updated["channel"] = channel;
        return updated;
    }
    return route_details(channel);
}

json list_routes() {
    auto routes = filter_controls(std::optional<std::string>("Mux Routing"), std::nullopt);
    json result = json::array();
    const std::regex name_regex(R"(^PCM (\d+) Capture Enum$)");
    for (const auto &route : routes) {
        json item = route;
        std::smatch match;
        const auto name = route.value("name", "");
        if (std::regex_match(name, match, name_regex)) item["channel"] = std::stoi(match[1].str());
        const int index = std::stoi(route.value("value", "0"));
        if (route.contains("items") && route["items"].is_array() && index >= 0 && static_cast<size_t>(index) < route["items"].size()) {
            item["value_index"] = index;
            item["value_label"] = route["items"][index];
        }
        result.push_back(item);
    }
    return result;
}

json meter_snapshot() {
    return get_control_details("Level Meter");
}

json save_snapshot(const std::string &path) {
    if (trim_copy(path).empty()) throw ApiError(400, "snapshot path must not be empty");
    auto result = run_fcp_mix({"save", path});
    if (result.exit_code != 0) throw ApiError(400, "fcp-mix save failed: " + trim_copy(result.output));
    return {{"path", path}, {"output", trim_copy(result.output)}};
}

json load_snapshot(const std::string &path) {
    if (trim_copy(path).empty()) throw ApiError(400, "snapshot path must not be empty");
    auto result = run_fcp_mix({"load", path});
    if (result.exit_code != 0) throw ApiError(400, "fcp-mix load failed: " + trim_copy(result.output));
    return {{"path", path}, {"output", trim_copy(result.output)}};
}

json health_check() {
    auto start = std::chrono::steady_clock::now();
    auto result = run_fcp_mix({"--json", "list", "Inputs"});
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (result.exit_code != 0) {
        throw ApiError(503, "health check failed: " + trim_copy(result.output));
    }
    json payload;
    payload["ok"] = true;
    payload["card"] = g_config.card.has_value() ? json(*g_config.card) : json(nullptr);
    payload["port"] = g_config.port;
    payload["bind_host"] = g_config.bind_host;
    payload["fcp_mix_path"] = g_config.fcp_mix_path;
    payload["check"] = {{"command", "fcp-mix --json list Inputs"}, {"exit_code", result.exit_code}, {"duration_ms", elapsed_ms}};
    return payload;
}

json parse_json_body(const httplib::Request &req) {
    if (req.body.empty()) return json::object();
    try {
        return json::parse(req.body);
    } catch (const std::exception &ex) {
        throw ApiError(400, std::string("invalid JSON body: ") + ex.what());
    }
}

std::string require_string(const json &body, const char *key) {
    if (!body.contains(key) || !body[key].is_string()) {
        throw ApiError(400, std::string("missing string field: ") + key);
    }
    return body[key].get<std::string>();
}

int require_int(const json &body, const char *key) {
    if (!body.contains(key) || !body[key].is_number_integer()) {
        throw ApiError(400, std::string("missing integer field: ") + key);
    }
    return body[key].get<int>();
}

json api_success(const json &data) {
    return {{"ok", true}, {"data", data}};
}

json api_error_json(const std::string &message) {
    return {{"ok", false}, {"error", message}};
}

void send_json(httplib::Response &res, int status, const json &body) {
    res.status = status;
    res.set_content(body.dump(2) + "\n", "application/json");
}

void handle_api(httplib::Response &res, const std::function<json()> &fn) {
    try {
        send_json(res, 200, api_success(fn()));
    } catch (const ApiError &ex) {
        send_json(res, ex.status, api_error_json(ex.what()));
    } catch (const std::exception &ex) {
        send_json(res, 500, api_error_json(ex.what()));
    }
}

json mcp_tools() {
    return json::array({
        {{"name", "fcp_list_controls"}, {"description", "List Scarlett ALSA controls by optional group and shell-glob pattern."}, {"inputSchema", {{"type", "object"}, {"properties", {{"group", {{"type", "string"}}}, {"pattern", {{"type", "string"}}}}}}}},
        {{"name", "fcp_get_control"}, {"description", "Get one Scarlett ALSA control by exact name."}, {"inputSchema", {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", json::array({"name"})}}}},
        {{"name", "fcp_set_control"}, {"description", "Set one Scarlett ALSA control by exact name."}, {"inputSchema", {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}, {"value", {{"type", "string"}}}}}, {"required", json::array({"name", "value"})}}}},
        {{"name", "fcp_phantom_power"}, {"description", "Toggle phantom power for a Line In input."}, {"inputSchema", {{"type", "object"}, {"properties", {{"input", {{"type", "string"}}}, {"state", {{"type", "string"}}}}}, {"required", json::array({"input", "state"})}}}},
        {{"name", "fcp_mute"}, {"description", "Toggle a mute control via the fcp-mix shortcut."}, {"inputSchema", {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}, {"state", {{"type", "string"}}}}}, {"required", json::array({"name", "state"})}}}},
        {{"name", "fcp_mix_volume"}, {"description", "Get or set one mix input playback volume."}, {"inputSchema", {{"type", "object"}, {"properties", {{"mix", {{"type", "string"}}}, {"input", {{"type", "integer"}}}, {"value", {{"type", "string"}}}}}, {"required", json::array({"mix", "input"})}}}},
        {{"name", "fcp_route"}, {"description", "Get or set one PCM capture route."}, {"inputSchema", {{"type", "object"}, {"properties", {{"channel", {{"type", "integer"}}}, {"source", {{"type", "string"}}}}}, {"required", json::array({"channel"})}}}},
        {{"name", "fcp_meter"}, {"description", "Read the current level meter snapshot."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
        {{"name", "fcp_save_snapshot"}, {"description", "Save the full Scarlett control state to a JSON file."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}}}},
        {{"name", "fcp_load_snapshot"}, {"description", "Load the full Scarlett control state from a JSON file."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}}}},
        {{"name", "fcp_health"}, {"description", "Run the live health probe against the Scarlett ALSA controls."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}}
    });
}

json call_mcp_tool(const std::string &name, const json &args) {
    if (name == "fcp_list_controls") {
        std::optional<std::string> group;
        std::optional<std::string> pattern;
        if (args.contains("group") && args["group"].is_string()) group = args["group"].get<std::string>();
        if (args.contains("pattern") && args["pattern"].is_string()) pattern = args["pattern"].get<std::string>();
        return filter_controls(group, pattern);
    }
    if (name == "fcp_get_control") return get_control_details(require_string(args, "name"));
    if (name == "fcp_set_control") return set_control_value(require_string(args, "name"), require_string(args, "value"));
    if (name == "fcp_phantom_power") return set_phantom_power(require_string(args, "input"), require_string(args, "state"));
    if (name == "fcp_mute") return set_mute(require_string(args, "name"), require_string(args, "state"));
    if (name == "fcp_mix_volume") {
        std::optional<std::string> value;
        if (args.contains("value") && args["value"].is_string()) value = args["value"].get<std::string>();
        return mix_volume(require_string(args, "mix"), require_int(args, "input"), value);
    }
    if (name == "fcp_route") {
        std::optional<std::string> source;
        if (args.contains("source") && args["source"].is_string()) source = args["source"].get<std::string>();
        return route_value(require_int(args, "channel"), source);
    }
    if (name == "fcp_meter") return meter_snapshot();
    if (name == "fcp_save_snapshot") return save_snapshot(require_string(args, "path"));
    if (name == "fcp_load_snapshot") return load_snapshot(require_string(args, "path"));
    if (name == "fcp_health") return health_check();
    throw ApiError(404, "unknown MCP tool: " + name);
}

json make_mcp_result(const json &id, const json &content, bool is_error) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"content", json::array({{{"type", "text"}, {"text", content.dump(2)}}})},
            {"structuredContent", content},
            {"isError", is_error}
        }}
    };
}

json make_mcp_rpc_error(const json &id, int code, const std::string &message) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}}
    };
}

void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            g_config.bind_host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            g_config.port = std::stoi(argv[++i]);
        } else if (arg == "--card" && i + 1 < argc) {
            g_config.card = std::stoi(argv[++i]);
        } else if (arg == "--fcp-mix" && i + 1 < argc) {
            g_config.fcp_mix_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: zig-fcps-api [--listen HOST] [--port PORT] [--card CARD] [--fcp-mix PATH]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        parse_args(argc, argv);

        httplib::Server server;
        server.set_read_timeout(30, 0);
        server.set_write_timeout(30, 0);
        server.set_idle_interval(30, 0);
        server.set_payload_max_length(1024 * 1024);

        if (!server.set_mount_point("/ui", API_UI_DATADIR)) {
            std::cerr << "warning: could not mount UI directory " << API_UI_DATADIR << std::endl;
        }


        server.Get("/", [](const httplib::Request &, httplib::Response &res) {
            try {
                res.set_content(read_text_file(std::string(API_UI_DATADIR) + "/index.html"), "text/html");
            } catch (const ApiError &ex) {
                send_json(res, ex.status, api_error_json(ex.what()));
            } catch (const std::exception &ex) {
                send_json(res, 500, api_error_json(ex.what()));
            }
        });

        server.Get("/health", [](const httplib::Request &, httplib::Response &res) {
            try {
                send_json(res, 200, health_check());
            } catch (const ApiError &ex) {
                send_json(res, ex.status, api_error_json(ex.what()));
            } catch (const std::exception &ex) {
                send_json(res, 500, api_error_json(ex.what()));
            }
        });

        server.Get("/api/v1/controls", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                std::optional<std::string> group;
                std::optional<std::string> pattern;
                if (req.has_param("group")) group = req.get_param_value("group");
                if (req.has_param("pattern")) pattern = req.get_param_value("pattern");
                return filter_controls(group, pattern);
            });
        });

        server.Get("/api/v1/control", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                if (!req.has_param("name")) throw ApiError(400, "missing query parameter: name");
                return get_control_details(req.get_param_value("name"));
            });
        });

        server.Post("/api/v1/control", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return set_control_value(require_string(body, "name"), require_string(body, "value"));
            });
        });

        server.Post("/api/v1/phantom", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return set_phantom_power(require_string(body, "input"), require_string(body, "state"));
            });
        });

        server.Post("/api/v1/mute", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return set_mute(require_string(body, "name"), require_string(body, "state"));
            });
        });

        server.Get("/api/v1/mix", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                if (!req.has_param("mix") || !req.has_param("input")) {
                    throw ApiError(400, "missing query parameters: mix and input");
                }
                return mix_volume(req.get_param_value("mix"), std::stoi(req.get_param_value("input")), std::nullopt);
            });
        });

        server.Post("/api/v1/mix", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return mix_volume(require_string(body, "mix"), require_int(body, "input"), require_string(body, "value"));
            });
        });

        server.Get("/api/v1/routes", [](const httplib::Request &, httplib::Response &res) {
            handle_api(res, []() { return list_routes(); });
        });

        server.Get("/api/v1/route", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                if (!req.has_param("channel")) throw ApiError(400, "missing query parameter: channel");
                return route_value(std::stoi(req.get_param_value("channel")), std::nullopt);
            });
        });

        server.Post("/api/v1/route", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return route_value(require_int(body, "channel"), require_string(body, "source"));
            });
        });

        server.Get("/api/v1/meter", [](const httplib::Request &, httplib::Response &res) {
            handle_api(res, []() { return meter_snapshot(); });
        });

        server.Post("/api/v1/snapshot/save", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return save_snapshot(require_string(body, "path"));
            });
        });

        server.Post("/api/v1/snapshot/load", [](const httplib::Request &req, httplib::Response &res) {
            handle_api(res, [&]() {
                const auto body = parse_json_body(req);
                return load_snapshot(require_string(body, "path"));
            });
        });

        server.Post("/mcp", [](const httplib::Request &req, httplib::Response &res) {
            try {
                const auto body = parse_json_body(req);
                const json id = body.contains("id") ? body["id"] : json(nullptr);
                const std::string method = body.value("method", "");

                if (method == "tools/list") {
                    send_json(res, 200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", mcp_tools()}}}});
                    return;
                }

                if (method != "tools/call") {
                    send_json(res, 200, make_mcp_rpc_error(id, -32601, "method not found"));
                    return;
                }

                if (!body.contains("params") || !body["params"].is_object()) {
                    send_json(res, 200, make_mcp_rpc_error(id, -32602, "missing params object"));
                    return;
                }

                const auto &params = body["params"];
                const std::string tool_name = params.value("name", "");
                const json arguments = params.contains("arguments") && params["arguments"].is_object()
                    ? params["arguments"]
                    : json::object();

                try {
                    send_json(res, 200, make_mcp_result(id, call_mcp_tool(tool_name, arguments), false));
                } catch (const ApiError &ex) {
                    send_json(res, 200, make_mcp_result(id, {{"error", ex.what()}}, true));
                }
            } catch (const std::exception &ex) {
                send_json(res, 200, make_mcp_rpc_error(nullptr, -32700, ex.what()));
            }
        });

        server.set_error_handler([](const httplib::Request &, httplib::Response &res) {
            if (res.status == 404) {
                send_json(res, 404, api_error_json("not found"));
            }
        });

        std::cerr << "zig-fcps-api listening on " << g_config.bind_host << ':' << g_config.port << std::endl;
        if (!server.listen(g_config.bind_host, g_config.port)) {
            std::cerr << "failed to listen on " << g_config.bind_host << ':' << g_config.port << std::endl;
            return 1;
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
}
