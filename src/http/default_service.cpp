// SPDX-License-Identifier: MIT
#include "pico_http/http/default_service.hpp"

#include <memory>

#include "pico_http/http/http_service.hpp"
#include "pico_http/tcp/lwip_tcp_transport.hpp"

namespace pico_http {

std::unique_ptr<HttpService> make_http_service() {
    return std::make_unique<HttpService>(std::make_unique<LwipTcpTransport>());
}

}  // namespace pico_http
