# Vendored Boost headers

Source: official modular repository `https://github.com/boostorg/boost.git`.

- Release: Boost 1.92.0
- Superproject tag: `boost-1.92.0`
- Superproject commit: `afdfa32505af73e3d208144b3f623f0096cb62b6`
- Release archive SHA-256: `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`
- License: Boost Software License 1.0; see `LICENSE_1_0.txt`

The header tree was generated with the official Boost `bcp` tool from these
application entry points:

- `boost/asio.hpp`
- `boost/beast/core.hpp`
- `boost/beast/http.hpp`
- `boost/beast/version.hpp`
- `boost/algorithm/string.hpp`
- `boost/circular_buffer.hpp`

TVStreamer5 defines `BOOST_ERROR_CODE_HEADER_ONLY`; no compiled Boost library
is linked or required at build or runtime.
