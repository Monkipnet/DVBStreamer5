#include "NativeHttpClient.h"

#include <cpp-httplib/httplib.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    if (tvs::http::encodeQueryComponent("a b&я") != "a%20b%26%D1%8F") {
        std::cerr << "query encoding test failed\n";
        return 1;
    }

    httplib::Server server;
    server.Get("/payload", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("TVStreamer5", "application/octet-stream");
    });
    server.Get("/redirect", [](const httplib::Request&, httplib::Response& response) {
        response.set_redirect("/payload", 302);
    });
    server.Get("/nested/redirect", [](const httplib::Request&, httplib::Response& response) {
        response.set_redirect("../payload?token=a%20b", 302);
    });
    server.Get("/header", [](const httplib::Request& request, httplib::Response& response) {
        response.set_content(request.get_header_value("X-TVStreamer-Test"), "text/plain");
    });
    server.Get("/large", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(std::string(4096, 'x'), "application/octet-stream");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::cerr << "failed to bind test HTTP server\n";
        return 2;
    }
    std::thread serverThread([&] { server.listen_after_bind(); });

    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    tvs::http::RequestOptions options;
    options.headers.emplace_back("X-TVStreamer-Test", "native-client");
    options.maxBodyBytes = 1024;
    tvs::http::Response response;
    std::string error;

    bool ok = tvs::http::get(origin + "/redirect", options, response, error);
    if (!ok || response.status != 200 || response.effectiveUrl != origin + "/payload" ||
        std::string(response.body.begin(), response.body.end()) != "TVStreamer5") {
        std::cerr << "redirected buffered GET failed: " << error << '\n';
        server.stop();
        serverThread.join();
        return 3;
    }

    ok = tvs::http::get(origin + "/nested/redirect", options, response, error);
    if (!ok || response.effectiveUrl != origin + "/payload?token=a%20b") {
        std::cerr << "relative redirect resolution failed: " << error << '\n';
        server.stop();
        serverThread.join();
        return 4;
    }

    std::vector<std::uint8_t> streamed;
    ok = tvs::http::get(origin + "/header", options, response, error,
        [&](const std::uint8_t* data, std::size_t size) {
            streamed.insert(streamed.end(), data, data + size);
            return true;
        });
    if (!ok || std::string(streamed.begin(), streamed.end()) != "native-client") {
        std::cerr << "streaming/header GET failed: " << error << '\n';
        server.stop();
        serverThread.join();
        return 5;
    }

    if (tvs::http::get(origin + "/large", options, response, error) ||
        error.find("body limit") == std::string::npos || response.body.size() != 1024) {
        std::cerr << "response body limit test failed\n";
        server.stop();
        serverThread.join();
        return 6;
    }

    std::atomic<bool> stopping{true};
    options.stopping = &stopping;
    if (tvs::http::get(origin + "/payload", options, response, error) ||
        error != "request cancelled") {
        std::cerr << "pre-cancelled request test failed\n";
        server.stop();
        serverThread.join();
        return 7;
    }

    server.stop();
    serverThread.join();
    std::cout << "PASS: native HTTP client buffered, redirect, streaming and cancellation\n";
    return 0;
}
