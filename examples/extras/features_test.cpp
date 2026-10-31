// Self-contained checks for the example helpers. Does NOT link the cppmcp library:
// it provides a tiny stand-in for core::serialize_message, so build it standalone.
//
//   g++ -std=c++20 -pthread -I include -I examples/common examples/tests/features_test.cpp -o features_test

#include <cassert>
#include <cstdio>
#include <iostream>

#include "demo_features.hpp"
#include "ext_client.hpp"
#include "http_frontend.hpp"

namespace cppmcp::core {
// Stand-in for the SDK implementation (only the pieces these tests touch).
std::string serialize_message(const Message& message) {
    Json out = {{"jsonrpc", "2.0"}};
    const auto id_json = [](const RequestId& id) {
        return std::visit([](const auto& v) -> Json { return v; }, id);
    };
    if (const auto* r = std::get_if<ResultResponse>(&message)) {
        out["id"] = id_json(r->id);
        out["result"] = r->result;
    } else if (const auto* e = std::get_if<ErrorResponse>(&message)) {
        out["id"] = e->id ? id_json(*e->id) : Json();
        out["error"] = {{"code", e->error.code}, {"message", e->error.message}};
    }
    return out.dump();
}
const std::vector<ProtocolVersion>& known_protocol_versions() {
    static const std::vector<ProtocolVersion> v{{"2025-06-18", ProtocolEra::legacy},
                                                {"2026-07-28", ProtocolEra::modern}};
    return v;
}
}  // namespace cppmcp::core

using namespace cppmcp;
using namespace cppmcp::examples;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                             \
    } while (0)

static core::Message send(FeatureRegistry& reg, std::string method, Json params, std::int64_t id = 1) {
    auto reply = reg.try_handle(core::Request{id, std::move(method), std::move(params)});
    assert(reply);
    return *reply;
}
static Json result_of(const core::Message& m) {
    const auto* r = std::get_if<core::ResultResponse>(&m);
    if (!r) { std::fprintf(stderr, "expected result, got error: %s\n", std::get<core::ErrorResponse>(m).error.message.c_str()); return Json(); }
    return r->result;
}
static std::int32_t error_code(const core::Message& m) {
    const auto* e = std::get_if<core::ErrorResponse>(&m);
    return e ? e->error.code : 0;
}

// A fake "tools/call" backend: sleeps `ms` unless cancelled; "boom" fails; "bad" returns isError.
static core::Message fake_tool(const core::Request& req, std::stop_token token) {
    const auto name = req.params.value("name", std::string{});
    CHECK(!req.params.contains("task"));  // the task wrapper must be stripped
    if (name == "boom") return core::ErrorResponse{req.id, {-32601, "no such tool", std::nullopt}};
    if (name == "bad") return core::ResultResponse{req.id, {{"isError", true}, {"content", Json::array()}}};
    const int ms = req.params.value("arguments", Json::object()).value("ms", 10);
    for (int i = 0; i < ms / 10 && !token.stop_requested(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return core::ResultResponse{req.id, {{"content", Json::array({{{"type", "text"}, {"text", "done " + name}}})}}};
}

static void test_templates() {
    auto m = detail::match_uri_template("demo://cities/{city}", "demo://cities/New%20York");
    CHECK(m && m->at("city") == "New York");
    CHECK(!detail::match_uri_template("demo://cities/{city}", "demo://cities/a/b"));
    CHECK(!detail::match_uri_template("demo://cities/{city}", "demo://cities/"));
    CHECK(!detail::match_uri_template("demo://cities/{city}", "demo://other/x"));
    auto two = detail::match_uri_template("x://{a}/files/{b}", "x://one/files/two");
    CHECK(two && two->at("a") == "one" && two->at("b") == "two");
}

static void test_prompts_resources_completions() {
    FeatureRegistry reg;
    add_demo_features(reg);

    auto prompts = result_of(send(reg, "prompts/list", Json::object()));
    CHECK(prompts["prompts"].size() == 2);
    CHECK(prompts["prompts"][0]["arguments"][0]["required"] == true);

    auto got = result_of(send(reg, "prompts/get", {{"name", "code_review"}, {"arguments", {{"code", "int x;"}, {"language", "cpp"}}}}));
    CHECK(got["messages"][0]["content"]["text"].get<std::string>().find("int x;") != std::string::npos);
    CHECK(error_code(send(reg, "prompts/get", {{"name", "code_review"}, {"arguments", Json::object()}})) == -32602);
    CHECK(error_code(send(reg, "prompts/get", {{"name", "nope"}})) == -32602);

    auto brief = result_of(send(reg, "prompts/get", {{"name", "city_briefing"}, {"arguments", {{"city", "tokyo"}}}}));
    CHECK(brief["messages"][1]["content"]["type"] == "resource");

    auto res = result_of(send(reg, "resources/list", Json::object()));
    CHECK(res["resources"].size() == 4);
    auto tmpl = result_of(send(reg, "resources/templates/list", Json::object()));
    CHECK(tmpl["resourceTemplates"].size() == 2);

    auto readme = result_of(send(reg, "resources/read", {{"uri", "demo://readme"}}));
    CHECK(readme["contents"][0]["mimeType"] == "text/markdown");
    auto png = result_of(send(reg, "resources/read", {{"uri", "demo://pixel.png"}}));
    CHECK(png["contents"][0].contains("blob") && !png["contents"][0].contains("text"));
    auto city = result_of(send(reg, "resources/read", {{"uri", "demo://cities/Lisbon"}}));
    CHECK(Json::parse(city["contents"][0]["text"].get<std::string>())["temp_c"] == 22);
    CHECK(error_code(send(reg, "resources/read", {{"uri", "demo://cities/Atlantis"}})) == -32002);
    CHECK(error_code(send(reg, "resources/read", {{"uri", "demo://nope"}})) == -32002);

    // pagination
    FeatureOptions small;
    small.page_size = 2;
    FeatureRegistry paged(small);
    add_demo_features(paged);
    auto p1 = result_of(send(paged, "resources/list", Json::object()));
    CHECK(p1["resources"].size() == 2 && p1["nextCursor"] == "2");
    auto p2 = result_of(send(paged, "resources/list", {{"cursor", "2"}}));
    CHECK(p2["resources"].size() == 2 && !p2.contains("nextCursor"));
    CHECK(error_code(send(paged, "resources/list", {{"cursor", "abc"}})) == -32602);

    // completions
    auto c1 = result_of(send(reg, "completion/complete", {{"ref", {{"type", "ref/prompt"}, {"name", "code_review"}}}, {"argument", {{"name", "language"}, {"value", "ru"}}}}));
    CHECK(c1["completion"]["values"] == Json::array({"rust"}));
    auto c2 = result_of(send(reg, "completion/complete", {{"ref", {{"type", "ref/resource"}, {"uri", "demo://cities/{city}"}}}, {"argument", {{"name", "city"}, {"value", ""}}}}));
    CHECK(c2["completion"]["total"] == 4);
    CHECK(error_code(send(reg, "completion/complete", {{"ref", {{"type", "ref/prompt"}, {"name", "zzz"}}}, {"argument", {{"name", "a"}, {"value", ""}}}})) == -32602);

    // subscriptions -> notifier only for subscribed uris
    std::vector<std::string> pushed;
    reg.set_notifier([&](const std::string& method, const Json& p) { pushed.push_back(method + " " + p.value("uri", "")); });
    reg.resource_updated("demo://clock");
    CHECK(pushed.empty());
    send(reg, "resources/subscribe", {{"uri", "demo://clock"}});
    reg.resource_updated("demo://clock");
    CHECK(pushed.size() == 1 && pushed[0] == "notifications/resources/updated demo://clock");
    send(reg, "resources/unsubscribe", {{"uri", "demo://clock"}});
    reg.resource_updated("demo://clock");
    CHECK(pushed.size() == 1);

    // capability augmentation: only discover-like payloads, never overwriting
    Json discover = {{"serverInfo", {{"name", "x"}}}, {"capabilities", {{"tools", Json::object()}, {"prompts", {{"listChanged", false}}}}}};
    reg.augment_discover(discover);
    CHECK(discover["capabilities"].contains("resources") && discover["capabilities"].contains("tasks"));
    CHECK(discover["capabilities"]["prompts"]["listChanged"] == false);
    Json other = {{"content", Json::array()}};
    reg.augment_discover(other);
    CHECK(!other.contains("capabilities"));

    // unknown methods fall through
    CHECK(!reg.try_handle(core::Request{std::int64_t{9}, "tools/list", Json::object()}));
    CHECK(!reg.try_handle(core::Request{std::int64_t{9}, "tools/call", {{"name", "x"}}}));
}

static void test_tasks() {
    FeatureRegistry reg;
    reg.set_tool_executor(fake_tool);
    std::vector<std::string> statuses;
    std::mutex m;
    reg.set_notifier([&](const std::string& method, const Json& p) {
        std::lock_guard l(m);
        if (method == "notifications/tasks/status") statuses.push_back(p["status"]);
    });

    // success path + tasks/result blocks until done
    auto created = result_of(send(reg, "tools/call", {{"name", "ok"}, {"arguments", {{"ms", 80}}}, {"task", {{"ttl", 5000}}}}));
    const std::string id = created["task"]["taskId"];
    CHECK(created["task"]["status"] == "working" && created["task"]["ttl"] == 5000);
    auto final_result = result_of(send(reg, "tasks/result", {{"taskId", id}}));
    CHECK(final_result["content"][0]["text"] == "done ok");
    CHECK(final_result["_meta"]["io.modelcontextprotocol/related-task"]["taskId"] == id);
    CHECK(result_of(send(reg, "tasks/get", {{"taskId", id}}))["status"] == "completed");

    // failure paths
    auto boom = result_of(send(reg, "tools/call", {{"name", "boom"}, {"task", Json::object()}}))["task"]["taskId"].get<std::string>();
    auto boom_err = send(reg, "tasks/result", {{"taskId", boom}});
    CHECK(error_code(boom_err) == -32601);
    CHECK(result_of(send(reg, "tasks/get", {{"taskId", boom}}))["status"] == "failed");
    auto bad = result_of(send(reg, "tools/call", {{"name", "bad"}, {"task", Json::object()}}))["task"]["taskId"].get<std::string>();
    send(reg, "tasks/result", {{"taskId", bad}});
    CHECK(result_of(send(reg, "tasks/get", {{"taskId", bad}}))["status"] == "failed");

    // cancellation interrupts the worker and sticks
    auto slow = result_of(send(reg, "tools/call", {{"name", "slow"}, {"arguments", {{"ms", 5000}}}, {"task", Json::object()}}))["task"]["taskId"].get<std::string>();
    auto t0 = std::chrono::steady_clock::now();
    CHECK(result_of(send(reg, "tasks/cancel", {{"taskId", slow}}))["status"] == "cancelled");
    CHECK(error_code(send(reg, "tasks/cancel", {{"taskId", slow}})) == -32602);  // already terminal
    CHECK(error_code(send(reg, "tasks/result", {{"taskId", slow}})) != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(result_of(send(reg, "tasks/get", {{"taskId", slow}}))["status"] == "cancelled");  // worker did not overwrite
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));

    // listing and unknown ids
    auto list = result_of(send(reg, "tasks/list", Json::object()));
    CHECK(list["tasks"].size() == 4);
    CHECK(error_code(send(reg, "tasks/get", {{"taskId", "nope"}})) == -32602);
    CHECK(error_code(send(reg, "tools/call", {{"name", "x"}, {"task", {{"ttl", -5}}}})) == -32602);

    {
        std::lock_guard l(m);
        CHECK(std::count(statuses.begin(), statuses.end(), "completed") == 1);
        CHECK(std::count(statuses.begin(), statuses.end(), "failed") == 2);
        CHECK(std::count(statuses.begin(), statuses.end(), "cancelled") == 1);
    }

    // ttl expiry purges terminal tasks only
    FeatureRegistry expiring;
    expiring.set_tool_executor(fake_tool);
    auto quick = result_of(send(expiring, "tools/call", {{"name", "q"}, {"arguments", {{"ms", 10}}}, {"task", {{"ttl", 50}}}}))["task"]["taskId"].get<std::string>();
    send(expiring, "tasks/result", {{"taskId", quick}});
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(error_code(send(expiring, "tasks/get", {{"taskId", quick}})) == -32602);

    // task limit
    FeatureOptions capped;
    capped.max_tasks = 2;
    FeatureRegistry limited(capped);
    limited.set_tool_executor(fake_tool);
    send(limited, "tools/call", {{"name", "a"}, {"arguments", {{"ms", 3000}}}, {"task", Json::object()}});
    send(limited, "tools/call", {{"name", "b"}, {"arguments", {{"ms", 3000}}}, {"task", Json::object()}});
    CHECK(error_code(send(limited, "tools/call", {{"name", "c"}, {"task", Json::object()}})) != 0);
    // destructor must cancel + join the two sleeping workers promptly
}

static void test_side_channel() {
    auto [client_end, server_end] = core::make_memory_transport_pair();
    FeatureRegistry reg;
    add_demo_features(reg);
    reg.set_tool_executor(fake_tool);
    std::atomic<int> forwarded{0};

    // "server": answer feature methods, bounce tools/list as a plain result.
    server_end->on_message([&, server = server_end.get()](core::Message m) {
        const auto* req = std::get_if<core::Request>(&m);
        if (!req) return;
        if (auto reply = reg.try_handle(*req)) { server->send(*reply); return; }
        server->send(core::ResultResponse{req->id, {{"tools", Json::array()}}});
        server->send(core::Notification{"notifications/prompts/list_changed", Json::object()});
    });

    auto side = std::make_shared<SideChannel>(client_end);
    std::vector<std::string> tapped;
    side->on_notification([&](const core::Notification& n) { tapped.push_back(n.method); });
    // Stand-in for cppmcp::Client's Connection: it must see its own traffic but not "ext-" ids.
    side->on_message([&](core::Message m) {
        if (const auto* r = std::get_if<core::ResultResponse>(&m)) { CHECK(std::get<std::int64_t>(r->id) == 77); ++forwarded; }
        else if (std::holds_alternative<core::Notification>(m)) ++forwarded;
    });

    FeatureClient features(side, make_modern_meta({{"name", "t"}}, Json::object()));
    CHECK(features.list_prompts()["prompts"].size() == 2);
    CHECK(features.list_all("resources/list", "resources").size() == 4);
    CHECK(features.read_resource("demo://readme")["contents"][0]["text"].get<std::string>().find("cppmcp") != std::string::npos);
    CHECK(features.complete_prompt("code_review", "language", "py")["completion"]["values"][0] == "python");
    CHECK(features.complete_resource("demo://notes/{id}", "id", "")["completion"]["total"] == 3);

    try { features.get_prompt("code_review"); CHECK(false); }
    catch (const RpcFailure& e) { CHECK(e.code() == -32602); }

    auto task = features.call_tool_as_task("ok", {{"ms", 40}}, 10000);
    const auto id = task["task"]["taskId"].get<std::string>();
    int polls = 0;
    auto done = features.wait_for_task(id, std::chrono::seconds(5), [&](const Json&) { ++polls; });
    CHECK(done["status"] == "completed" && polls >= 1);
    CHECK(features.task_result(id)["content"][0]["text"] == "done ok");

    // Client-owned traffic (int id) passes through untouched, notifications are tapped AND forwarded.
    side->send(core::Request{std::int64_t{77}, "tools/list", Json::object()});
    CHECK(forwarded == 2);
    CHECK(tapped.size() == 1 && tapped[0] == "notifications/prompts/list_changed");

    // timeouts and closed transports surface as exceptions, not hangs
    server_end->on_message([](core::Message) {});
    try { side->call("prompts/list", Json::object(), std::chrono::milliseconds(50)); CHECK(false); }
    catch (const std::runtime_error& e) { CHECK(std::string(e.what()).find("timed out") != std::string::npos); }
}

// --- raw HTTP helpers for the front-end test -------------------------------
static std::string http_exchange(unsigned short port, const std::string& request, bool read_all = true) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { ::close(fd); return "CONNECT_FAILED"; }
    ::send(fd, request.data(), request.size(), 0);
    std::string out; char buf[4096];
    timeval tv{3, 0}; ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (read_all) { auto n = ::recv(fd, buf, sizeof buf, 0); if (n <= 0) break; out.append(buf, n); }
    ::close(fd);
    return out;
}
static std::string post(const std::string& body, const std::string& extra = "") {
    return "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n" + extra +
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
static std::string body_of(const std::string& response) {
    auto p = response.find("\r\n\r\n"); return p == std::string::npos ? "" : response.substr(p + 4);
}

static void test_http_frontend() {
    FeatureRegistry reg;
    add_demo_features(reg);
    std::atomic<int> fallback_calls{0};

    FrontendOptions opts;
    opts.port = 0;
    opts.bearer_token = "s3cret";
    HttpFrontend fe(opts,
        [&](const core::Request& r) { return reg.try_handle(r); },
        [&](Json& result) { reg.augment_discover(result); },
        [&](const std::shared_ptr<transport::IHttpExchange>& ex) {
            ++fallback_calls;
            const auto& req = ex->request();
            if (req.method == "GET") {   // long-lived SSE, written from ANOTHER thread
                auto& sse = ex->begin_sse(200, {{"Content-Type", "text/event-stream"}});
                std::thread([&sse, ex] {
                    sse.write_comment("hi");
                    sse.write_event(R"({"jsonrpc":"2.0","method":"notifications/x"})");
                    sse.write_event(R"({"jsonrpc":"2.0","id":1,"result":{"serverInfo":{},"capabilities":{"tools":{}}}})");
                    sse.close();
                }).detach();
                return;
            }
            if (req.body.find("\"discover\"") != std::string::npos) {
                ex->respond(200, {{"Content-Type", "application/json"}},
                            R"({"jsonrpc":"2.0","id":5,"result":{"serverInfo":{"name":"s"},"capabilities":{"tools":{}}}})");
                return;
            }
            ex->respond(200, {{"Content-Type", "application/json"}, {"X-From", "sdk"}},
                        std::string("{\"echo\":\"") + std::to_string(req.body.size()) + "\"}");
        });
    fe.start();
    const auto port = fe.port();
    CHECK(port != 0);
    const std::string auth = "Authorization: Bearer s3cret\r\n";

    // auth + routing
    CHECK(http_exchange(port, post("{}")).find("401 Unauthorized") != std::string::npos);
    CHECK(http_exchange(port, post("{}", "Authorization: Bearer wrong\r\n")).find("401") != std::string::npos);
    CHECK(http_exchange(port, "GET /other HTTP/1.1\r\nHost: x\r\n" + auth + "\r\n").find("404") != std::string::npos);
    CHECK(http_exchange(port, "garbage\r\n\r\n").find("400") != std::string::npos);
    CHECK(fallback_calls == 0);

    // feature method answered locally (no fallback), real JSON over the wire
    auto r1 = http_exchange(port, post(R"({"jsonrpc":"2.0","id":3,"method":"prompts/list","params":{}})", auth));
    CHECK(r1.find("200 OK") != std::string::npos && r1.find("Content-Length:") != std::string::npos);
    auto j1 = Json::parse(body_of(r1));
    CHECK(j1["id"] == 3 && j1["result"]["prompts"].size() == 2);
    CHECK(fallback_calls == 0);

    // other methods, notifications and batches go to the SDK handler untouched
    auto r2 = http_exchange(port, post(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})", auth));
    CHECK(r2.find("X-From: sdk") != std::string::npos && fallback_calls == 1);
    http_exchange(port, post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", auth));
    http_exchange(port, post(R"([{"jsonrpc":"2.0","id":1,"method":"prompts/list"}])", auth));
    CHECK(fallback_calls == 3);

    // capabilities patched on the way out of the SDK handler
    auto r3 = http_exchange(port, post(R"({"jsonrpc":"2.0","id":5,"method":"discover"})", auth));
    auto j3 = Json::parse(body_of(r3));
    CHECK(j3["result"]["capabilities"].contains("tasks") && j3["result"]["capabilities"].contains("prompts"));

    // large body split over many packets + Expect: 100-continue
    std::string big(200000, 'a');
    auto r4 = http_exchange(port, post(big, auth + "Expect: 100-continue\r\n"));
    CHECK(body_of(r4).find("\"echo\":\"200000\"") != std::string::npos);

    // chunked SSE written from a foreign thread, with the capability patch applied per event
    auto r5 = http_exchange(port, "GET /mcp HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n" + auth + "\r\n");
    CHECK(r5.find("Transfer-Encoding: chunked") != std::string::npos);
    CHECK(r5.find("event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}") != std::string::npos);
    CHECK(r5.find("\"tasks\"") != std::string::npos);
    CHECK(r5.find("0\r\n\r\n") != std::string::npos);

    // oversized and chunked request bodies are refused
    opts.max_body_bytes = 10;
    CHECK(http_exchange(port, "POST /mcp HTTP/1.1\r\nHost: x\r\n" + auth + "Transfer-Encoding: chunked\r\n\r\n").find("501") != std::string::npos);

    // slow client that never finishes its headers must not wedge shutdown
    int idle = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::connect(idle, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::send(idle, "POST /mcp HTT", 13, 0);
    auto t0 = std::chrono::steady_clock::now();
    fe.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(7));
    ::close(idle);
}

int main() {
    test_templates();
    test_prompts_resources_completions();
    test_tasks();
    test_side_channel();
    test_http_frontend();
    std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : std::string("all checks passed")) << '\n';
    return failures ? 1 : 0;
}
