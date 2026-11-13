// SPDX-License-Identifier: MIT
// The installed firmware links after SDK initialisation (AR-12).
#include "pico_http/http/default_service.hpp"

int main() {
    auto service = pico_http::make_http_service();
    return service ? 0 : 1;
}
