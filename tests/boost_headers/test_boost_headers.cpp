#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/version.hpp>

#include <iostream>
#include <string>

int main() {
    boost::asio::io_context ioContext;
    boost::beast::flat_buffer buffer;
    boost::beast::http::request<boost::beast::http::string_body> request;
    request.method(boost::beast::http::verb::get);
    request.target("/health");

    boost::circular_buffer<int> values(2);
    values.push_back(1);
    values.push_back(2);

    std::string value = " TVSTREAMER5 ";
    boost::algorithm::trim(value);
    boost::algorithm::to_lower(value);

    if (ioContext.stopped() || buffer.size() != 0 ||
        request.target() != "/health" || values.front() != 1 ||
        value != "tvstreamer5" || BOOST_BEAST_VERSION == 0 ||
        BOOST_VERSION != 109200) {
        std::cerr << "vendored Boost header test failed\n";
        return 1;
    }

    std::cout << "PASS: vendored Boost Asio, Beast, Algorithm and CircularBuffer\n";
    return 0;
}
