// SPDX-License-Identifier: MIT
// AR-12: the installed firmware surface links after SDK initialization.
#include "pico_http/http/default_service.hpp"

int main() {
    auto service = pico_http::make_http_service();
    return service ? 0 : 1;
}
