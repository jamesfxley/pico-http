// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pico_http/http/message.hpp"

/// \file body_source.hpp
/// The body a response is currently emitting, in whichever of the three modes
/// the handler chose. Gathering the mode, its storage and its declared length
/// into one type keeps the three representations (the handler's HTTPResponse,
/// the connection's in-flight copy, and the transfer switch that moved between
/// them) from having to be edited in step.

namespace pico_http::detail {

class BodySource {
   public:
    using Mode = HTTPResponse::BodyMode;
    using Generator = HTTPResponse::BodyGenerator;

    /// Take the body over from a handler's response. BUFFERED swaps the vector
    /// rather than copying elements, so the handler's bytes are moved and the
    /// response is left with the empty buffer reset() leaves here;
    /// STATIC_VIEW and GENERATOR retain the caller's source, so no body copy
    /// happens at all (AR-2).
    void adopt(HTTPResponse& response) noexcept {
        reset();
        mode_ = response.body_mode;
        streaming_ = response.is_chunked_body();
        // A length-less generator streams until it returns 0, so it has no
        // total to count down.
        total_ = streaming_ ? 0U : response.body_size();
        switch (mode_) {
            case Mode::STATIC_VIEW:
                static_ = response.static_body.bytes();
                break;
            case Mode::GENERATOR:
                generator_ = response.generator;
                context_ = response.generator_ctx;
                break;
            case Mode::BUFFERED:
            default:
                owned_.swap(response.body);
                break;
        }
    }

    /// Drop back to an empty buffered body, freeing any copied one. set_body
    /// builds a fresh exact-sized vector for every copied response, so capacity
    /// kept here would never be reused, only held (AR-1).
    void reset() noexcept {
        mode_ = Mode::BUFFERED;
        std::vector<std::uint8_t>().swap(owned_);
        static_ = {};
        generator_ = nullptr;
        context_ = nullptr;
        total_ = 0;
        streaming_ = false;
    }

    /// Free the buffered bytes once they are on the wire, so an idle slot holds
    /// no copied-body heap (AR-1).
    void release_buffer() noexcept { std::vector<std::uint8_t>().swap(owned_); }

    [[nodiscard]] Mode mode() const noexcept { return mode_; }

    /// Declared body length. Meaningless for a streaming body, which reports 0.
    [[nodiscard]] std::size_t total() const noexcept { return total_; }

    /// True for a length-less generator body: framed as chunked, or
    /// close-delimited for an HTTP/1.0 client (NR-6).
    [[nodiscard]] bool streaming() const noexcept { return streaming_; }

    [[nodiscard]] bool is_generator() const noexcept {
        return mode_ == Mode::GENERATOR;
    }

    /// The bytes of a directly-addressable body (BUFFERED or STATIC_VIEW).
    /// Empty for a generator, which produces its bytes on demand instead.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        switch (mode_) {
            case Mode::STATIC_VIEW:
                return static_;
            case Mode::BUFFERED:
                return owned_;
            case Mode::GENERATOR:
            default:
                return {};
        }
    }

    /// Pull body bytes beginning at `offset`. The generator is a pure function
    /// of the offset — the writer owns the position — so a partial send simply
    /// resumes from where the transport stopped. Returns bytes written, never
    /// more than `out` holds.
    [[nodiscard]] std::size_t generate(std::size_t offset,
                                       std::span<std::uint8_t> out) const {
        if (generator_ == nullptr) {
            return 0;
        }
        return std::min(generator_(context_, offset, out), out.size());
    }

   private:
    Mode mode_ = Mode::BUFFERED;
    std::vector<std::uint8_t> owned_;
    std::span<const std::uint8_t> static_;
    Generator generator_ = nullptr;
    void* context_ = nullptr;
    std::size_t total_ = 0;
    bool streaming_ = false;
};

}  // namespace pico_http::detail
