#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-side benchmark for the pico-http benchmark firmware.

README.md describes the routes, tests, usage and why raw sockets are used.
"""

import argparse
import contextlib
import datetime
import json
import math
import socket
import statistics
import struct
import sys
import threading
import time
from pathlib import Path

DEFAULT_DOWNLOAD_SIZES = [1024, 2048, 4096, 8192, 12288, 16384, 20480, 24576, 32768]
DEFAULT_UPLOAD_SIZES = [256, 1024, 2048, 2700, 4096]
ALL_TESTS = ["t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8",
             "t9", "t10", "t11", "t12", "t13", "t14", "t15", "t16", "t17",
             "t18", "t19", "t20", "t21", "t22"]
DEFAULT_TESTS = ["t1", "t2", "t3", "t4", "t5", "t6", "t8",
                 "t9", "t10", "t11", "t12", "t13", "t15", "t16",
                 "t19", "t20", "t22"]

# t1 takes this many samples by default for its p99. Both t1 and t2 discard
# WARMUP_SAMPLES first. A smaller --count is recorded in the result.
MIN_P99_SAMPLES = 500
WARMUP_SAMPLES = 50

# The concurrent-load expectation is requests per second (PR-3).
PR3_MIN_AGGREGATE_RPS = 250.0
# The accept-rate expectation is measured separately from serial latency
# (PR-4). Its unit is accepts per second (PR-4).
PR4_MIN_ACCEPTS_PER_S = 150.0
# The download expectation uses nearest-rank p10 from 16 KiB (PR-5).
# Rates use 1024-byte KB throughout this script.
PR5_MIN_KBPS = 400.0
PR5_REPS = 20
PR5_PERCENTILE = 10
# The share of receive callbacks that must finish processing within 1 ms,
# read from /stats (PR-2).
PR2_WITHIN_1MS_SHARE = 0.99
PR5_MIN_SIZE = 16384
# The serial-latency expectation is in ms (PR-4).
PR4_MAX_CONNECT_P50_MS = 25.0
# Serial samples estimate p50.
CHURN_LATENCY_SAMPLES = 200
# Samples are shared across concurrent workers.
CHURN_RATE_CONNECTIONS = 1600
# A churn exchange slower than this, in seconds, counts as a client-path stall.
CHURN_STALL_S = 0.1
# The reuse-rate expectation is a fraction (RR-5).
RR5_MIN_REUSE_RATE = 0.99
# The reuse rate is measured over this many connections.
RR5_REUSE_CYCLES = 8
# The timeout tolerance is in seconds.
TIMEOUT_PRECISION_S = 0.2
# The full-load delivery allowance is in seconds and is added to the
# two-RTO expectation (PR-9).
BURST_SEGMENT_DELIVERY_S = 0.25


# The in-flight copied-body bound uses device-reported limits (BR-14).
def copied_body_heap_bound(cfg):
    maxc = cfg.info.get("max_connections",
                        cfg.info.get("memp_num_tcp_pcb", 8))
    max_body = cfg.limits.get("max_buffered_body", 2048)
    return maxc * max_body

# t7's plateau check passes when the second half of its 10 s free-heap
# samples spans at most this many bytes; it needs at least
# HEAP_PLATEAU_MIN_SAMPLES of those samples.
HEAP_PLATEAU_TOLERANCE_BYTES = 64
HEAP_PLATEAU_MIN_SAMPLES = 3


class Response:
    def __init__(self, status, headers, body, elapsed_s,
                 short_read=False, server_closed=False, ttfb_s=None):
        self.status = status
        # Header keys are lowercased.
        self.headers = headers
        # The body is bytes.
        self.body = body
        self.elapsed_s = elapsed_s
        self.short_read = short_read
        self.server_closed = server_closed
        # First-byte time is in seconds. None means unmeasured (PR-2).
        self.ttfb_s = ttfb_s


class HttpError(Exception):
    pass

# Retries cover transient connection errors, including saturation by lingering
# PCBs.
TRANSIENT_ERRNOS = frozenset({
    104,  # ECONNRESET
    111,  # ECONNREFUSED
    110,  # ETIMEDOUT
    113,  # EHOSTUNREACH (briefly during device reboot)
})


def _is_transient(exc):
    if isinstance(exc, socket.timeout):
        return True
    if isinstance(exc, (ConnectionResetError, ConnectionRefusedError,
                        ConnectionAbortedError)):
        return True
    if isinstance(exc, OSError):
        return exc.errno in TRANSIENT_ERRNOS
    return False


def connect_with_retry(host, port, timeout, attempts=12, backoff=0.5,
                       max_backoff=2.0):
    """Open a TCP connection with bounded retries for transient errors."""
    delay = backoff
    last = None
    for _ in range(attempts):
        try:
            sock = socket.create_connection((host, port), timeout=timeout)
            sock.settimeout(timeout)
        except OSError as exc:
            if not _is_transient(exc):
                raise
            last = exc
            time.sleep(delay)
            delay = min(delay * 1.5, max_backoff)
        else:
            return sock
    raise last or OSError("connection failed")


class HttpConn:
    """A raw TCP connection supports sequential requests."""

    def __init__(self, host, port, timeout):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = connect_with_retry(host, port, timeout)
        self.buf = b""
        self.closed = False

    def recv_until(self, marker):
        while marker not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                return False
            self.buf += chunk
        return True

    def request(self, method, path, body=b"", extra_headers=None,
                keep_alive=True):
        if isinstance(body, str):
            body = body.encode()
        headers = {
            "Host": self.host,
            "Connection": "keep-alive" if keep_alive else "close",
            "Content-Length": str(len(body)),
        }
        if extra_headers:
            headers.update(extra_headers)
        head = f"{method} {path} HTTP/1.1\r\n"
        head += "".join(f"{k}: {v}\r\n" for k, v in headers.items())
        head += "\r\n"

        start = time.perf_counter()
        self.sock.sendall(head.encode() + body)
        return self.read_response(start, keep_alive)

    def read_response(self, start=None, keep_alive=False):
        """Read one response independently of request transmission."""
        if start is None:
            start = time.perf_counter()
        # Buffered bytes have already arrived and count as zero TTFB (PR-2).
        if self.buf:
            ttfb_s = 0.0
        else:
            first = self.sock.recv(4096)
            if not first:
                self.closed = True
                raise HttpError("connection closed before response headers")
            self.buf += first
            ttfb_s = time.perf_counter() - start
        if not self.recv_until(b"\r\n\r\n"):
            self.closed = True
            raise HttpError("connection closed before response headers")
        header_blob, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = header_blob.split(b"\r\n")
        status_line = lines[0].decode(errors="replace")
        try:
            status = int(status_line.split(" ")[1])
        except (IndexError, ValueError) as exc:
            raise HttpError(f"bad status line: {status_line!r}") from exc

        resp_headers = {}
        for line in lines[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                resp_headers[k.decode().strip().lower()] = v.decode().strip()

        # The reader handles chunked framing or Content-Length (NR-6).
        if "chunked" in resp_headers.get("transfer-encoding", "").lower():
            body_bytes, short_read = self._read_chunked()
        else:
            content_length = int(resp_headers.get("content-length", "0"))
            short_read = False
            try:
                while len(self.buf) < content_length:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        short_read = True
                        break
                    self.buf += chunk
            except TimeoutError:
                short_read = True
            body_bytes = self.buf[:content_length]
            self.buf = self.buf[content_length:]
        elapsed = time.perf_counter() - start

        server_closed = resp_headers.get("connection", "").lower() == "close"
        if server_closed or not keep_alive:
            self.close()

        return Response(status, resp_headers, body_bytes, elapsed,
                        short_read=short_read, server_closed=server_closed,
                        ttfb_s=ttfb_s)

    def _read_chunked(self):
        """Return the decoded chunked body and a short-read flag."""
        body = b""
        try:
            while True:
                # Chunk extensions after the semicolon are ignored.
                while b"\r\n" not in self.buf:
                    more = self.sock.recv(4096)
                    if not more:
                        return body, True
                    self.buf += more
                line, self.buf = self.buf.split(b"\r\n", 1)
                size = int(line.split(b";", 1)[0].strip() or b"0", 16)
                if size == 0:
                    while b"\r\n" not in self.buf:
                        more = self.sock.recv(4096)
                        if not more:
                            break
                        self.buf += more
                    if self.buf.startswith(b"\r\n"):
                        self.buf = self.buf[2:]
                    return body, False
                while len(self.buf) < size + 2:
                    more = self.sock.recv(4096)
                    if not more:
                        return body, True
                    self.buf += more
                body += self.buf[:size]
                self.buf = self.buf[size + 2:]
        except TimeoutError:
            return body, True

    def close(self):
        if not self.closed:
            with contextlib.suppress(OSError):
                self.sock.close()
            self.closed = True


def one_shot(host, port, timeout, method, path, body=b"", extra_headers=None,
             attempts=4):
    """Retry a single exchange after connection errors or a mid-flight reset."""
    last = None
    for _ in range(attempts):
        conn = HttpConn(host, port, timeout)
        try:
            return conn.request(method, path, body, extra_headers,
                                keep_alive=False)
        except HttpError as exc:
            last = exc
            time.sleep(0.5)
        except OSError as exc:
            last = exc
            if not _is_transient(exc):
                raise
            time.sleep(0.5)
        finally:
            conn.close()
    raise last or HttpError("one_shot failed")


def get_json(host, port, timeout, path):
    resp = one_shot(host, port, timeout, "GET", path)
    return json.loads(resp.body.decode())


def get_info(host, port, timeout):
    return get_json(host, port, timeout, "/info")


def get_stats(host, port, timeout):
    return get_json(host, port, timeout, "/stats")


def reset_stats(host, port, timeout):
    """Reset counters and return the reply, including connections open at reset."""
    resp = one_shot(host, port, timeout, "POST", "/stats/reset")
    try:
        return json.loads(resp.body.decode())
    except ValueError:
        return {}


def wait_until_up(host, port, timeout, deadline_s=120):
    start = time.time()
    while time.time() - start < deadline_s:
        try:
            resp = one_shot(host, port, timeout, "GET", "/ping")
            if resp.status == 200 and resp.body == b"pong":
                return True
        except (OSError, HttpError):
            pass
        time.sleep(1)
    return False


def expected_pattern(size):
    return bytes((ord("A") + (i % 26)) for i in range(size))


def percentiles(samples_ms):
    if not samples_ms:
        return {}
    s = sorted(samples_ms)

    def pct(p):
        idx = min(len(s) - 1, round(p / 100.0 * (len(s) - 1)))
        return round(s[idx], 3)

    return {
        "min_ms": round(s[0], 3),
        "p50_ms": pct(50),
        "p90_ms": pct(90),
        "p99_ms": pct(99),
        "max_ms": round(s[-1], 3),
    }


def t1_latency(cfg):
    """Request latency (PR-1) and first-byte timing (PR-2) are recorded.

    The internal processing share gates; whole-callback timing is review
    evidence (PR-2).
    """
    n = cfg.count or MIN_P99_SAMPLES
    warmup = getattr(cfg, "warmup", WARMUP_SAMPLES)
    max_per_conn = max(1, cfg.info.get("keep_alive_max_requests", 16) - 1)
    samples = []
    ttfb_samples = []
    conn = None
    sent_on_conn = 0
    # Warm-up uses the measured connections, excluding initial link setup from
    # p99.
    start = None
    for index in range(warmup + n):
        if conn is None or sent_on_conn >= max_per_conn:
            if conn:
                conn.close()
            conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
            sent_on_conn = 0
        if index == warmup:
            reset_stats(cfg.host, cfg.port, cfg.timeout)
            start = time.perf_counter()
        resp = conn.request("GET", "/ping")
        sent_on_conn += 1
        if resp.status != 200:
            raise HttpError(f"unexpected status {resp.status}")
        if index >= warmup:
            samples.append(resp.elapsed_s * 1000.0)
            if resp.ttfb_s is not None:
                ttfb_samples.append(resp.ttfb_s * 1000.0)
        if resp.server_closed:
            conn = None
    total = time.perf_counter() - start
    if conn:
        conn.close()
    result = {"n": n, "warmup": warmup, "rps": round(n / total, 1)}
    if n < MIN_P99_SAMPLES:
        result["p99_samples_below_floor"] = MIN_P99_SAMPLES
    result.update(percentiles(samples))
    result["ttfb"] = percentiles(ttfb_samples)
    stats = _safe_stats(cfg) or {}
    timed = stats.get("receive_samples", 0)
    within = stats.get("receive_within_1ms", 0)
    # The reset callback is sampled after zeroing, contributing one of n + 1
    # samples.
    result["internal"] = {
        "receive_samples": timed,
        "receive_within_1ms": within,
        "receive_max_us": stats.get("receive_max_us"),
        "target_share": PR2_WITHIN_1MS_SHARE,
        # Whole-callback timing includes radio handoff and is recorded for
        # review.
        "receive_total_within_1ms": stats.get("receive_total_within_1ms"),
        "receive_total_max_us": stats.get("receive_total_max_us"),
    }
    result["pass"] = bool(timed > 0 and within / timed >= PR2_WITHIN_1MS_SHARE)
    return result


def t2_churn(cfg):
    """Record parallel accept rate and serial latency (PR-4).

    The client must not limit accept load; serial requests include client idle
    time (PR-4).
    """
    workers = cfg.info.get("max_connections", 8)
    warmup = getattr(cfg, "warmup", WARMUP_SAMPLES)

    # The serial phase measures connection latency (PR-4).
    latency = []
    failures = 0
    for index in range(warmup + CHURN_LATENCY_SAMPLES):
        try:
            resp = one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")
            if resp.status != 200:
                if index >= warmup:
                    failures += 1
            elif index >= warmup:
                latency.append(resp.elapsed_s * 1000.0)
        except (OSError, HttpError):
            if index >= warmup:
                failures += 1

    settle(cfg, quiet_s=0.5, deadline_s=20)

    # The concurrent phase measures accept rate (PR-4).
    target = cfg.count or CHURN_RATE_CONNECTIONS
    per_worker = max(1, target // workers)
    completed = [0] * workers
    stalled = [0] * workers
    stall_s = [0.0] * workers
    worker_failures = [0] * workers

    def churn(idx):
        for _ in range(per_worker):
            began = time.monotonic()
            try:
                resp = one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")
                if resp.status != 200:
                    worker_failures[idx] += 1
                    continue
            except (OSError, HttpError):
                worker_failures[idx] += 1
                continue
            completed[idx] += 1
            took = time.monotonic() - began
            if took > CHURN_STALL_S:
                stalled[idx] += 1
                stall_s[idx] += took

    before = _safe_stats(cfg)
    started = time.monotonic()
    threads = [threading.Thread(target=churn, args=(i,)) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.monotonic() - started
    after = _safe_stats(cfg)

    # One stats connection is accepted inside the measurement interval.
    accepts = None
    if before and after:
        accepts = after.get("accepts", 0) - before.get("accepts", 0) - 1
    refusals = None
    if before and after:
        refusals = (after.get("refusals_slot_full", 0)
                    - before.get("refusals_slot_full", 0))

    client_rate = round(sum(completed) / elapsed, 1)
    device_rate = round(accepts / elapsed, 1) if accepts is not None else None
    failures += sum(worker_failures)

    result = {
        "workers": workers,
        "connections": sum(completed),
        "interval_s": round(elapsed, 3),
        "device_accepts_per_s": device_rate,
        "target_accepts_per_s": PR4_MIN_ACCEPTS_PER_S,
        "client_completions_per_s": client_rate,
        "client_stalls": sum(stalled),
        "client_stall_s": round(sum(stall_s), 2),
        "slot_refusals": refusals,
        "latency_samples": len(latency),
        "warmup": warmup,
        "failures": failures,
        "target_p50_ms": PR4_MAX_CONNECT_P50_MS,
    }
    result.update(percentiles(latency))
    # Failed exchanges fail the case; speed is review evidence (PR-4).
    result["pass"] = bool(failures == 0 and device_rate is not None)
    return result


def t3_download(cfg):
    # Single-connection throughput (PR-5).
    """Record throughput by response size (PR-5).

    Every transfer must complete bit-exactly; speed is assessed at review.
    """
    sizes = cfg.sizes or DEFAULT_DOWNLOAD_SIZES
    reps = PR5_REPS
    per_size = []
    max_reliable = 0
    for size in sizes:
        reset_stats(cfg.host, cfg.port, cfg.timeout)
        complete = 0
        received_min = size
        throughputs = []
        truncated = False
        for _ in range(reps):
            try:
                resp = one_shot(cfg.host, cfg.port, cfg.timeout,
                                "GET", "{}{}".format("/payload/", size))
                received_min = min(received_min, len(resp.body))
                if resp.short_read or len(resp.body) < size:
                    truncated = True
                    # The delay lets the half-open connection expire so
                    # repetitions do not exhaust the device's PCB pool.
                    time.sleep(1.0)
                    _recover(cfg)
                    continue
                if resp.body != expected_pattern(size):
                    truncated = True
                    continue
                complete += 1
                if resp.elapsed_s > 0:
                    throughputs.append(len(resp.body) / 1024.0 / resp.elapsed_s)
            except (OSError, HttpError):
                truncated = True
                time.sleep(1.0)
                _recover(cfg)
        stats = _safe_stats(cfg)
        per_size.append({
            "size": size,
            "reps": reps,
            "complete": complete,
            "received_min": received_min,
            "kbps_avg": round(statistics.mean(throughputs), 1) if throughputs else None,
            "kbps_median": (round(statistics.median(throughputs), 1)
                            if throughputs else None),
            # The slowest repetition is recorded beside p10 (PR-5).
            "kbps_p10": _nearest_rank(throughputs, PR5_PERCENTILE),
            "kbps_min": round(min(throughputs), 1) if throughputs else None,
            "truncated": truncated,
            # A nonzero backpressure count confirms the writer paused (AR-5).
            "backpressure_events": stats.get("backpressure_events") if stats else None,
        })
        if complete == reps:
            max_reliable = size
    result = {"per_size": per_size, "max_reliable_size": max_reliable,
              "target_kbps": PR5_MIN_KBPS, "judged_from_size": PR5_MIN_SIZE,
              "judged_percentile": PR5_PERCENTILE}
    # Completion and integrity decide the verdict; speed is recorded (PR-5,
    # AR-5).
    result["pass"] = all(e["complete"] == reps and not e["truncated"]
                         for e in per_size)
    return result


def _nearest_rank(samples, percentile):
    """Return the nearest-rank percentile, the ceil(p/100 * n)-th smallest sample."""
    if not samples:
        return None
    rank = max(1, math.ceil(percentile / 100.0 * len(samples)))
    return round(sorted(samples)[rank - 1], 1)


def t4_upload(cfg):
    # Buffered upload throughput up to the 413 boundary (PR-7).
    """Upload throughput and integrity cover the body-size boundary."""
    sizes = cfg.upload_sizes or DEFAULT_UPLOAD_SIZES
    per_size = []
    max_reliable = 0
    for size in sizes:
        payload = expected_pattern(size)
        expected_checksum = sum(payload) & 0xFFFFFFFF
        entry = {"size": size}
        try:
            resp = one_shot(cfg.host, cfg.port, cfg.timeout, "POST",
                            "/upload", body=payload,
                            extra_headers={"Content-Type": "application/octet-stream"})
            entry["status"] = resp.status
            if resp.status == 200 and not resp.short_read:
                doc = json.loads(resp.body.decode())
                ok = (doc.get("received") == size and
                      doc.get("checksum") == expected_checksum)
                entry["checksum_ok"] = ok
                if resp.elapsed_s > 0:
                    entry["kbps"] = round(size / 1024.0 / resp.elapsed_s, 1)
                if ok:
                    max_reliable = max(max_reliable, size)
            else:
                entry["checksum_ok"] = False
        except (OSError, HttpError) as exc:
            entry["status"] = None
            entry["checksum_ok"] = False
            entry["error"] = str(exc)
        per_size.append(entry)
    return {"per_size": per_size, "max_reliable_upload": max_reliable}


def t5_concurrency(cfg):
    # Connection scaling reaches the slot cap (PR-3). The extra worker checks
    # rejection (RR-3).
    """Parallel keep-alive workers check the connection cap."""
    # The application slot cap differs from the larger PCB pool (BR-6).
    max_conns = cfg.info.get("max_connections", cfg.info.get("memp_num_tcp_pcb", 3))
    per_n = []
    lock_samples, lock_max_us, lock_p99_ok = 0, None, False
    for n in range(1, max_conns + 2):
        reset_stats(cfg.host, cfg.port, cfg.timeout)
        counts = [0] * n
        failures = [0] * n
        stop = threading.Event()

        # Default arguments bind round state so a changed join order cannot
        # expose the next round to running threads.
        def worker(idx, stop=stop, counts=counts, failures=failures):
            conn = None
            sent = 0
            try:
                conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                while not stop.is_set():
                    resp = conn.request("GET", "/ping")
                    if resp.status == 200:
                        counts[idx] += 1
                    else:
                        failures[idx] += 1
                    sent += 1
                    if resp.server_closed or sent >= 15:
                        conn.close()
                        conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                        sent = 0
            except (OSError, HttpError):
                failures[idx] += 1
            finally:
                if conn:
                    conn.close()

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
        start = time.perf_counter()
        for t in threads:
            t.start()
        time.sleep(3)
        stop.set()
        for t in threads:
            t.join()
        total = time.perf_counter() - start
        stats = _safe_stats(cfg)
        worker_failures = sum(1 for f in failures if f > 0)
        per_n.append({
            "n": n,
            "completed": sum(counts),
            "rps": round(sum(counts) / total, 1),
            "worker_failures": worker_failures,
            # Slot-exhaustion rejections are counted (RR-3 / OR-1).
            "refusals_slot_full_delta": (
                stats.get("refusals_slot_full") if stats else None),
        })
        # Lock-hold timing is sampled at the connection cap (AR-3).
        if n == max_conns and stats:
            lock_samples = stats.get("lock_hold_samples", 0)
            lock_within = stats.get("lock_hold_within_500us", 0)
            lock_max_us = stats.get("lock_hold_max_us")
            # A 99% within-budget share corresponds to p99 at 500 us.
            lock_p99_ok = (lock_samples > 0 and
                           lock_within / lock_samples >= 0.99)
        # Lingering PCBs clear between rounds.
        time.sleep(2)
    # Release builds compile out lock-hold sampling, so zero samples mean
    # unmeasured (AR-3).
    ar3_measured = lock_samples > 0

    # Workload errors and internal checks fail the case; speed is review
    # evidence (PR-3, RR-3, AR-3).
    at_cap = next((e for e in per_n if e["n"] == max_conns), None)
    over_cap = next((e for e in per_n if e["n"] == max_conns + 1), None)
    workload_ok = bool(at_cap and at_cap["worker_failures"] == 0
                       and at_cap["completed"] > 0)
    # The cap+1 round expects one failed worker. Extra failures affect
    # connections already holding slots (RR-3).
    refusals = (over_cap or {}).get("refusals_slot_full_delta")
    rr3_pass = bool(over_cap
                    and refusals is not None and refusals >= 1
                    and over_cap["worker_failures"] <= 1)

    return {
        "per_n": per_n,
        "connection_cap": max_conns,
        "rps_at_cap": (at_cap or {}).get("rps"),
        "target_rps_at_cap": PR3_MIN_AGGREGATE_RPS,
        "workload_ok": workload_ok,
        "refusals_over_cap": refusals,
        "rr3_pass": rr3_pass,
        "lock_hold_samples": lock_samples,
        "lock_hold_max_us": lock_max_us,
        "lock_hold_p99_within_500us": lock_p99_ok,
        "ar3_measured": ar3_measured,
        "ar3_pass": bool(lock_p99_ok) if ar3_measured else None,
        "pass": bool(workload_ok and rr3_pass
                     and (lock_p99_ok if ar3_measured else True)),
    }


def t6_keepalive(cfg):
    """Check the keep-alive header, the request cap and connection reuse (RR-5).

    Unissued requests after an early close count against the reuse rate
    (RR-5).
    """
    limit = cfg.info.get("keep_alive_max_requests", 16)
    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
    keep_alive_seen = False
    close_on_last = False
    max_observed = 0
    eof_after_limit = False
    try:
        for i in range(1, limit + 2):
            try:
                resp = conn.request("GET", "/ping")
            except HttpError:
                if i > limit:
                    eof_after_limit = True
                break
            max_observed = i
            conn_hdr = resp.headers.get("connection", "").lower()
            if conn_hdr == "keep-alive":
                keep_alive_seen = True
            if i == limit and conn_hdr == "close":
                close_on_last = True
            if resp.server_closed:
                if i >= limit:
                    eof_after_limit = True
                break
    finally:
        conn.close()
    reuse_attempts = 0
    reuses_honoured = 0
    premature_closes = 0
    for _ in range(RR5_REUSE_CYCLES):
        reuse_attempts += limit - 1
        reuse_conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
        try:
            for i in range(1, limit + 1):
                resp = reuse_conn.request("GET", "/ping")
                if i > 1 and resp.status == 200:
                    reuses_honoured += 1
                if resp.server_closed:
                    if i < limit:
                        premature_closes += 1
                    break
        except (OSError, HttpError):
            premature_closes += 1
        finally:
            reuse_conn.close()
    reuse_rate = (reuses_honoured / reuse_attempts) if reuse_attempts else 0.0

    return {
        "max_requests_observed": max_observed,
        "keep_alive_header_seen": keep_alive_seen,
        "close_header_on_last": close_on_last,
        "closed_after_limit": eof_after_limit,
        "reuse_attempts": reuse_attempts,
        "reuses_honoured": reuses_honoured,
        "premature_closes": premature_closes,
        "reuse_rate": round(reuse_rate, 4),
        "target_reuse_rate": RR5_MIN_REUSE_RATE,
        "pass": bool(keep_alive_seen and max_observed <= limit
                     and reuse_rate >= RR5_MIN_REUSE_RATE),
    }


def t7_soak(cfg):
    """Mixed load checks errors, slots, counters and lwIP allocation (RR-4).

    Heap stays bounded and flat (AR-1).
    """
    duration = cfg.duration
    open_at_reset = reset_stats(cfg.host, cfg.port, cfg.timeout).get(
        "active_connections")
    lwip_before = _safe_lwip(cfg)
    start_stats = get_stats(cfg.host, cfg.port, cfg.timeout)
    heap_start = start_stats.get("free_heap_bytes")
    requests = 0
    errors = 0
    heap_samples = []
    start = time.time()
    last_sample = 0
    conn = None
    sent = 0
    while time.time() - start < duration:
        try:
            if conn is None or sent >= 15:
                if conn:
                    conn.close()
                conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                sent = 0
            path = "/ping" if requests % 2 == 0 else "/payload/4096"
            resp = conn.request("GET", path)
            sent += 1
            requests += 1
            if resp.status != 200 or resp.short_read:
                errors += 1
            if resp.server_closed:
                conn = None
        except (OSError, HttpError):
            errors += 1
            conn = None
        now = time.time() - start
        if now - last_sample >= 10:
            last_sample = now
            s = _safe_stats(cfg)
            if s:
                heap_samples.append(s.get("free_heap_bytes"))
    if conn:
        conn.close()
    end_stats = get_stats(cfg.host, cfg.port, cfg.timeout)
    # The absolute copied-body bound is independent of the plateau check
    # (RR-4, AR-1).
    bound = copied_body_heap_bound(cfg)
    growth = end_stats.get("post_init_heap_growth_bytes")
    heap_ok = growth is not None and growth <= bound
    # A flat free-heap tail detects continued growth below the absolute bound.
    # Short runs lacking enough tail samples report the plateau as unmeasured.
    tail = [s for s in heap_samples if s is not None]
    tail = tail[len(tail) // 2:]
    if len(tail) >= HEAP_PLATEAU_MIN_SAMPLES:
        plateau_spread = max(tail) - min(tail)
        plateau_ok = plateau_spread <= HEAP_PLATEAU_TOLERANCE_BYTES
    else:
        plateau_spread = None
        plateau_ok = True
    released, final_stats = slots_released(cfg)
    consistency = accounting_verdict(final_stats, open_at_reset)
    lwip_failures = lwip_alloc_failures(lwip_before, _safe_lwip(cfg))
    return {
        "duration_s": duration,
        "requests": requests,
        "errors": errors,
        "heap_start": heap_start,
        "heap_end": end_stats.get("free_heap_bytes"),
        "heap_samples": heap_samples,
        "post_init_heap_growth_bytes": growth,
        "copied_body_heap_bound": bound,
        "heap_within_bound": heap_ok,
        "heap_plateau_spread_bytes": plateau_spread,
        "heap_plateau_ok": plateau_ok,
        "slots_released": released,
        "open_at_reset": open_at_reset,
        "counters": consistency,
        "lwip_alloc_failures": lwip_failures,
        "pass": bool(requests > 0 and errors == 0 and heap_ok and plateau_ok
                     and released and consistency["ok"] and
                     lwip_alloc_clean(lwip_failures)),
    }


def t8_rst_robustness(cfg):
    """Repeated resets of partial requests check slot reuse (RR-1).

    SO_LINGER(0) sends RST on close.
    """
    n = cfg.count or 100
    max_conns = cfg.info.get("max_connections", cfg.info.get("memp_num_tcp_pcb", 3))
    reset_stats(cfg.host, cfg.port, cfg.timeout)

    rst_failures = 0
    for _ in range(n):
        try:
            sock = connect_with_retry(cfg.host, cfg.port, cfg.timeout)
            sock.sendall(b"GET /ping HTTP/1.1\r\nHost: x\r\n")  # No final CRLF.
            # SO_LINGER(1, 0) makes close() send RST.
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
            sock.close()
        except OSError:
            rst_failures += 1
        # The delay allows device reset handling between attempts.
        time.sleep(0.02)

    if not settle(cfg, quiet_s=0.5, deadline_s=15):
        return {"n": n, "rst_failures": rst_failures,
                "error": "device unresponsive after RST burst"}

    mid_stats = _safe_stats(cfg) or {}

    # Every slot must be usable immediately after the resets.
    fresh_ok = 0
    fresh_conns = []
    try:
        for _ in range(max_conns):
            c = HttpConn(cfg.host, cfg.port, cfg.timeout)
            resp = c.request("GET", "/ping", keep_alive=True)
            if resp.status == 200 and resp.body == b"pong":
                fresh_ok += 1
            fresh_conns.append(c)
    finally:
        for c in fresh_conns:
            c.close()

    end_stats = _safe_stats(cfg) or {}
    # Each successful reset attempt must count as peer reset. Failed connects
    # and rare plain closes allow slack (RR-1).
    reset_count = mid_stats.get("disconnect_peer_reset", 0)
    expected_resets = n - rst_failures
    resets_ok = reset_count >= expected_resets - 2
    # The /stats connection contributes one to active_connections.
    return {
        "n": n,
        "rst_failures": rst_failures,
        "max_connections": max_conns,
        "fresh_connections_ok": fresh_ok,
        "disconnect_peer_reset": reset_count,
        "expected_resets": expected_resets,
        "resets_ok": resets_ok,
        "active_connections_after_burst": mid_stats.get("active_connections"),
        "active_connections_final": end_stats.get("active_connections"),
        "pass": (fresh_ok == max_conns and resets_ok and
                 mid_stats.get("active_connections", -1) <= 1),
    }


def t9_slowloris(cfg):
    """Check slowloris defence (TR-1).

    A dripped head must close near the header deadline while another client
    remains served (TR-1).
    """
    limits = getattr(cfg, "limits", {}) or {}
    header_ms = limits.get("timeout_header_ms", 5000)
    header_s = header_ms / 1000.0

    reset_stats(cfg.host, cfg.port, cfg.timeout)
    sock = connect_with_retry(cfg.host, cfg.port, cfg.timeout)
    sock.sendall(b"GET / HTTP/1.1\r\n")
    start = time.time()

    served_during = 0
    closed_at = None
    deadline = header_s + 4.0
    while time.time() - start < deadline:
        try:
            sock.sendall(b"X")
        except OSError:
            closed_at = time.time() - start
            break
        try:
            probe = one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")
            if probe.status == 200:
                served_during += 1
        except (OSError, HttpError):
            pass
        sock.settimeout(0.1)
        try:
            if sock.recv(64) == b"":
                closed_at = time.time() - start
                break
        except TimeoutError:
            pass
        except OSError:
            closed_at = time.time() - start
            break
        time.sleep(1.0)
    sock.close()

    stats = _safe_stats(cfg) or {}
    within = (closed_at is not None and
              header_s - 0.5 <= closed_at <= header_s + 2.5)
    return {
        "header_timeout_s": header_s,
        "closed_at_s": round(closed_at, 2) if closed_at else None,
        "served_during": served_during,
        "disconnect_timeout_header": stats.get("disconnect_timeout_header"),
        "pass": bool(within and served_during > 0 and
                     stats.get("disconnect_timeout_header", 0) >= 1),
    }


def t10_boundary_matrix(cfg):
    """Boundary checks cover malformed and oversized requests.

    Each must return the specified status with Connection: close
    (BR-1..9, NR-1..6).
    """
    limits = getattr(cfg, "limits", {}) or {}
    max_uri = limits.get("max_uri", 512)
    max_body = limits.get("max_buffered_body", 2048)

    cases = []

    def check(name, raw, expect_status):
        try:
            conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
            conn.sock.sendall(raw)
            resp = conn.read_response()
            conn.close()
            ok = resp.status == expect_status
            cases.append({"case": name, "status": resp.status,
                          "expect": expect_status,
                          "close": resp.server_closed, "ok": ok})
        except (OSError, HttpError) as exc:
            cases.append({"case": name, "status": None,
                          "expect": expect_status, "close": None,
                          "ok": False, "error": str(exc)})

    host = cfg.host.encode()
    long_uri = b"/" + b"a" * (max_uri + 50)
    check("414_long_uri",
          b"GET " + long_uri + b" HTTP/1.1\r\nHost: " + host + b"\r\n\r\n", 414)
    many = b"".join(b"H%d: v\r\n" % i for i in range(40))
    check("431_many_headers",
          b"GET / HTTP/1.1\r\nHost: " + host + b"\r\n" + many + b"\r\n", 431)
    big = b"x" * (max_body + 100)
    check("413_big_body",
          b"POST / HTTP/1.1\r\nHost: " + host +
          b"\r\nContent-Length: %d\r\n\r\n" % len(big) + big, 413)
    # A syntactically valid unsupported HTTP version returns 505 (BR-11).
    check("505_unsupported_version",
          b"GET / HTTP/2.0\r\nHost: " + host + b"\r\n\r\n", 505)
    check("400_dup_content_length",
          b"POST / HTTP/1.1\r\nHost: " + host +
          b"\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx", 400)
    check("501_te_gzip",
          b"POST / HTTP/1.1\r\nHost: " + host +
          b"\r\nTransfer-Encoding: gzip\r\n\r\n", 501)
    # Only a lone chunked coding is supported (NR-4).
    check("501_te_stacked",
          b"POST / HTTP/1.1\r\nHost: " + host +
          b"\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501)
    # HTTP/1.0 Transfer-Encoding is rejected as faulty framing (RFC 9112
    # §6.1).
    check("400_te_http10",
          b"POST /upload HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"
          b"3\r\nabc\r\n0\r\n\r\n", 400)
    # Unimplemented expectations return 417 (RFC 9110 §10.1.1).
    check("417_unknown_expect",
          b"POST /upload HTTP/1.1\r\nHost: " + host +
          b"\r\nContent-Length: 2\r\nExpect: 200-maybe\r\n\r\nhi", 417)
    # One leading CRLF is ignored (RFC 9112 §2.2).
    check("200_leading_crlf",
          b"\r\nGET /ping HTTP/1.1\r\nHost: " + host +
          b"\r\nConnection: close\r\n\r\n", 200)

    # HEAD returns GET's header section without a body (RFC 9110 §9.3.2).
    try:
        conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
        conn.sock.sendall(b"HEAD /ping HTTP/1.1\r\nHost: " + host +
                          b"\r\nConnection: close\r\n\r\n")
        raw = b""
        conn.sock.settimeout(cfg.timeout)
        while True:
            try:
                got = conn.sock.recv(4096)
            except TimeoutError:
                break
            if not got:
                break
            raw += got
        conn.close()
        head_end = raw.find(b"\r\n\r\n")
        ok = (raw.startswith(b"HTTP/1.1 200") and head_end != -1 and
              b"Content-Length: 4\r\n" in raw and
              raw[head_end + 4:] == b"")
        cases.append({"case": "head_of_get_no_body", "status": 200 if ok else None,
                      "expect": 200, "ok": ok})
    except (OSError, HttpError) as exc:
        cases.append({"case": "head_of_get_no_body", "status": None,
                      "expect": 200, "ok": False, "error": str(exc)})

    # Chunked request decoding (NR-4).
    def enc_chunks(chunks, ext=b"", trailer=b""):
        """Frame chunks with optional extensions and trailers (RFC 9112 §7.1)."""
        out = b""
        for c in chunks:
            out += b"%x" % len(c) + ext + b"\r\n" + c + b"\r\n"
        return out + b"0" + ext + b"\r\n" + trailer + b"\r\n"

    def check_chunked_upload(name, chunks, ext=b"", trailer=b""):
        """Check the decoded upload length and checksum for leaked framing bytes."""
        decoded = b"".join(chunks)
        raw = (b"POST /upload HTTP/1.1\r\nHost: " + host +
               b"\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
               enc_chunks(chunks, ext=ext, trailer=trailer))
        try:
            conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
            conn.sock.sendall(raw)
            resp = conn.read_response()
            conn.close()
            echo = (json.loads(resp.body.decode())
                    if resp.status == 200 else None)
            ok = (resp.status == 200 and
                  echo == {"received": len(decoded),
                           "checksum": sum(decoded)})
            cases.append({"case": name, "status": resp.status, "expect": 200,
                          "echo": echo, "ok": ok})
        except (OSError, HttpError, ValueError) as exc:
            cases.append({"case": name, "status": None, "expect": 200,
                          "ok": False, "error": str(exc)})

    check_chunked_upload("200_chunked_decoded",
                         [b"hello ", b"chunked ", b"x" * 300, b"world"])
    check_chunked_upload("200_chunked_ext_trailer",
                         [b"trailer", b"ed"],
                         ext=b";bench=1", trailer=b"X-Checksum: ignored\r\n")
    check("413_chunked_oversize",
          b"POST /upload HTTP/1.1\r\nHost: " + host +
          b"\r\nTransfer-Encoding: chunked\r\n\r\n" +
          enc_chunks([b"x" * (max_body + 100)]), 413)

    # The interim 100 precedes the body and final response (NR-5).
    try:
        body = b"y" * 64
        conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
        conn.sock.sendall(
            b"POST /upload HTTP/1.1\r\nHost: " + host +
            b"\r\nContent-Length: %d" % len(body) +
            b"\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n")
        interim = conn.read_response(keep_alive=True)
        conn.sock.sendall(body)
        final = conn.read_response()
        conn.close()
        echo = (json.loads(final.body.decode())
                if final.status == 200 else None)
        ok = (interim.status == 100 and final.status == 200 and
              echo == {"received": len(body), "checksum": sum(body)})
        cases.append({"case": "100_continue_then_200",
                      "status": final.status, "expect": 200,
                      "interim": interim.status, "echo": echo, "ok": ok})
    except (OSError, HttpError, ValueError) as exc:
        cases.append({"case": "100_continue_then_200", "status": None,
                      "expect": 200, "ok": False, "error": str(exc)})
    check("413_expect_oversize",
          b"POST /upload HTTP/1.1\r\nHost: " + host +
          b"\r\nContent-Length: %d" % (max_body + 100) +
          b"\r\nExpect: 100-continue\r\n\r\n", 413)

    passed = all(c["ok"] for c in cases)
    return {"cases": cases, "pass": passed}

# The stall threshold separates receiving spans for the rate recorded beside
# wall-clock throughput.
# baselines/README.md explains send-gap interpretation.
STALL_GAP_S = 0.5


def t20_aggregate_download(cfg):
    """Parallel throughput and send gaps are review evidence (PR-6, PR-9).

    The measurement differs from single-connection throughput (PR-5).

    Completion and lwIP allocation failures decide the verdict.
    """
    workers = cfg.info.get("max_connections", 8)
    lwip_before = _safe_lwip(cfg)
    size = cfg.body_bytes or 65536
    target_kbps = cfg.aggregate_kbps
    # The gap expectation follows the configured initial RTO plus delivery
    # allowance (PR-9, AR-10). The allowance is a constant because a probe
    # during full-slot load would be rejected (RR-3).
    rto_ms = cfg.info.get("tcp_rto_ms", 3000)
    gap_ceiling_s = 2.0 * rto_ms / 1000.0 + BURST_SEGMENT_DELIVERY_S
    results = [None] * workers

    def pull(index):
        # A raw socket exposes gaps between reads that HttpConn hides until
        # body completion.
        started = time.monotonic()
        try:
            sock = connect_with_retry(cfg.host, cfg.port, cfg.timeout)
        except OSError:
            return
        try:
            sock.sendall(
                f"GET /payload/{size} HTTP/1.1\r\nHost: bench\r\n"
                f"Connection: close\r\n\r\n".encode())
            buf, last, worst = b"", started, 0.0
            # The intervals when this connection was receiving are in seconds.
            active = []
            span_start = started
            while True:
                chunk = sock.recv(65536)
                now = time.monotonic()
                if not chunk:
                    break
                gap = now - last
                worst = max(worst, gap)
                if gap > STALL_GAP_S:
                    active.append([span_start, last])
                    span_start = now
                last = now
                buf += chunk
            active.append([span_start, last])
            head, _, body = buf.partition(b"\r\n\r\n")
            status = 0
            if head.startswith(b"HTTP/1."):
                with contextlib.suppress(ValueError, IndexError):
                    status = int(head.split()[1])
            results[index] = {"status": status, "bytes": len(body),
                              "elapsed": time.monotonic() - started,
                              "worst_gap": worst, "active": active}
        except OSError:
            results[index] = None
        finally:
            sock.close()

    started = time.monotonic()
    threads = [threading.Thread(target=pull, args=(i,)) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    wall = time.monotonic() - started

    complete = [r for r in results if r and r["status"] == 200
                and r["bytes"] == size]
    total_bytes = sum(r["bytes"] for r in complete)
    worst_gap = max((r["worst_gap"] for r in results if r), default=0.0)
    stalled = sum(1 for r in results if r and r["worst_gap"] > STALL_GAP_S)

    kbps = (total_bytes / 1024.0) / wall if wall > 0 else 0.0
    # The union of receiving spans avoids crediting one connection with time
    # spent serving another.
    spans = sorted(sp for r in complete for sp in r["active"] if sp[1] > sp[0])
    union, end = 0.0, None
    for lo, hi in spans:
        if end is None or lo > end:
            union += hi - lo
            end = hi
        elif hi > end:
            union += hi - end
            end = hi
    stall_free = (total_bytes / 1024.0) / union if union > 0 else 0.0
    lwip_failures = lwip_alloc_failures(lwip_before, _safe_lwip(cfg))

    return {
        "test": "t20",
        "pass": (len(complete) == workers and lwip_alloc_clean(lwip_failures)),
        "lwip_alloc_failures": lwip_failures,
        "connections": workers,
        "size": size,
        "complete": len(complete),
        "wall_s": round(wall, 3),
        "aggregate_kbps": round(kbps, 1),
        "stall_free_kbps": round(stall_free, 1),
        "stalled_connections": stalled,
        "worst_gap_s": round(worst_gap, 3),
        "gap_ceiling_s": round(gap_ceiling_s, 3),
        "tcp_rto_ms": rto_ms,
        "segment_delivery_s": BURST_SEGMENT_DELIVERY_S,
        "target_kbps": target_kbps,
    }


def t11_backpressure_download(cfg):
    """Verify a bit-exact 256 KB body across ring fills (PR-8, AR-5).

    The writer must resume across many ring fills.
    """
    # A byte size, not --count: a request count here shrank the body below
    # one ring fill.
    size = cfg.body_bytes or 262144
    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
    try:
        resp = conn.request("GET", f"/payload/{size}", keep_alive=False)
    finally:
        conn.close()
    expected = expected_pattern(size)
    stats = _safe_stats(cfg) or {}
    events = stats.get("backpressure_events")
    # The writer must resume at least once to exercise streaming (PR-8, AR-5).
    resumed = events is None or events > 0
    ok = resp.status == 200 and resp.body == expected and resumed
    return {
        "size": size,
        "status": resp.status,
        "received": len(resp.body),
        "bit_exact": resp.body == expected,
        "backpressure_events": events,
        "writer_resumed": resumed,
        "pass": bool(ok),
    }


def t12_slow_reader(cfg):
    """Parallel clients remain served beside a stalled download.

    A stalled download must not degrade other clients and must be reclaimed
    by the write timeout (TR-4, AR-5). The disconnect counter avoids client
    buffering and reads that reopen the window.
    """
    limits = getattr(cfg, "limits", {}) or {}
    write_ms = limits.get("timeout_write_ms", 10000)
    write_s = write_ms / 1000.0

    reset_stats(cfg.host, cfg.port, cfg.timeout)
    sock = connect_with_retry(cfg.host, cfg.port, cfg.timeout + write_s)
    # A small receive window makes the server stall before its write timeout
    # (TR-4).
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    sock.sendall(b"GET /payload/262144 HTTP/1.1\r\nHost: " + cfg.host.encode() +
                 b"\r\nConnection: keep-alive\r\n\r\n")
    sock.settimeout(cfg.timeout)
    primed = b""
    try:
        while len(primed) < 2048:
            chunk = sock.recv(512)
            if not chunk:
                break
            primed += chunk
    except TimeoutError:
        pass

    stall_start = time.time()
    probe_ms = []
    probe_failures = 0
    # A second client must remain served while the first is stalled.
    while time.time() - stall_start < write_s + 3.0:
        t0 = time.perf_counter()
        try:
            probe = one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")
            if probe.status == 200:
                probe_ms.append((time.perf_counter() - t0) * 1000.0)
            else:
                probe_failures += 1
        except (OSError, HttpError):
            probe_failures += 1
        time.sleep(0.5)
    sock.close()

    stats = _safe_stats(cfg) or {}
    pct = percentiles(probe_ms)
    isolation_ok = (bool(probe_ms) and probe_failures == 0 and
                    pct.get("p99_ms", 1e9) < 500.0)
    tr4_fired = stats.get("disconnect_timeout_write", 0) >= 1
    return {
        "write_timeout_s": write_s,
        "primed_bytes": len(primed),
        "probes_served": len(probe_ms),
        "probe_failures": probe_failures,
        "probe_p50_ms": pct.get("p50_ms"),
        "probe_p99_ms": pct.get("p99_ms"),
        "disconnect_timeout_write": stats.get("disconnect_timeout_write"),
        "pass": bool(isolation_ok and tr4_fired),
    }


def t13_rx_flow_control(cfg):
    """Pipelined requests wait behind a draining response.

    Their bytes must remain buffered with the window closed while that
    response drains (AR-6, OR-1).

    Both responses must arrive uncorrupted; a full RX buffer must hold the
    window closed without truncation.
    """
    size = cfg.body_bytes or 65536
    reset_stats(cfg.host, cfg.port, cfg.timeout)
    host = cfg.host.encode()
    pipelined = (
        b"GET /payload/%d HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n"
        % (size, host) +
        b"GET /ping HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % host)

    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
    try:
        conn.sock.sendall(pipelined)
        first = _read_response_paced(conn, pace_s=0.02)
        second = conn.read_response(keep_alive=False)
    finally:
        conn.close()

    expected = expected_pattern(size)
    first_ok = first.status == 200 and first.body == expected
    second_ok = second.status == 200 and second.body == b"pong"
    stats = _safe_stats(cfg) or {}
    throttles = stats.get("rx_window_throttle_events", 0)
    # Separate counters distinguish receive-window throttling from discarded
    # bytes.
    drops = stats.get("rx_overflow_drops", 0)
    return {
        "size": size,
        "first_status": first.status,
        "first_bit_exact": first.body == expected,
        "second_status": second.status,
        "second_body_ok": second.body == b"pong",
        "rx_window_throttle_events": throttles,
        "rx_overflow_drops": drops,
        "pass": bool(first_ok and second_ok and throttles > 0 and drops == 0),
    }


def _read_response_paced(conn, pace_s=0.02):
    """Read a response with paced recv() calls to hold backpressure during t13."""
    start = time.perf_counter()
    if not conn.recv_until(b"\r\n\r\n"):
        conn.closed = True
        raise HttpError("connection closed before response headers")
    header_blob, conn.buf = conn.buf.split(b"\r\n\r\n", 1)
    lines = header_blob.split(b"\r\n")
    status = int(lines[0].decode(errors="replace").split(" ")[1])
    resp_headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            resp_headers[k.decode().strip().lower()] = v.decode().strip()
    content_length = int(resp_headers.get("content-length", "0"))
    short_read = False
    while len(conn.buf) < content_length:
        try:
            chunk = conn.sock.recv(1024)
        except TimeoutError:
            short_read = True
            break
        if not chunk:
            short_read = True
            break
        conn.buf += chunk
        time.sleep(pace_s)
    body_bytes = conn.buf[:content_length]
    conn.buf = conn.buf[content_length:]
    return Response(status, resp_headers, body_bytes,
                    time.perf_counter() - start, short_read=short_read)


def t14_timeout_precision(cfg):
    """Timeout precision for the idle and header timeouts (TR-1, TR-3).

    Those two are the ones observable from a client: the others need the device
    to report when it acted. The tolerance is the +/- 200 ms every TR row
    states.

    Both measurements carry network legs the device is not answerable for, and
    they enter the two differently.

    Idle: the device arms on entering the idle phase, which is after the
    response has drained, and the client's clock starts when that response
    arrives -- one leg later. The close then takes a leg to come back, so in the
    steady state the two cancel and the reading is the timeout plus the scan
    interval. A delayed response leg, or client delay before its clock starts,
    reads short, because the device does not close before its deadline. Adding
    half the request's round-trip is a heuristic correction for that shortfall;
    with symmetric legs the reading is then half a round-trip long.

    Header: the device arms when the partial head arrives, a leg after the
    client starts its clock, and the close takes another leg to come back. Both
    add, so a whole round-trip comes off.

    A close is also only evidence about a timeout if that timeout is what
    caused it. Each half asserts its own disconnect counter moved, as t9 does,
    so a close from any other cause fails rather than being timed.
    """
    limits = getattr(cfg, "limits", {}) or {}
    idle_ms = limits.get("timeout_idle_ms", 15000)
    header_ms = limits.get("timeout_header_ms", 5000)

    def wait_for_close(sock, budget_s):
        sock.settimeout(budget_s)
        try:
            while sock.recv(64) != b"":
                pass
        except (TimeoutError, OSError):
            pass

    def measure_idle():
        conn = HttpConn(cfg.host, cfg.port, cfg.timeout + idle_ms / 1000.0)
        sent = time.monotonic()
        conn.request("GET", "/ping", keep_alive=True)
        start = time.monotonic()
        rtt = start - sent
        wait_for_close(conn.sock, idle_ms / 1000.0 + 5.0)
        elapsed = time.monotonic() - start
        conn.close()
        return elapsed, rtt

    def measure_header():
        # A separate probe measures RTT because the timed connection never
        # completes a request.
        probe = HttpConn(cfg.host, cfg.port, cfg.timeout)
        sent = time.monotonic()
        probe.request("GET", "/ping")
        rtt = time.monotonic() - sent
        probe.close()

        sock = connect_with_retry(cfg.host, cfg.port,
                                  cfg.timeout + header_ms / 1000.0)
        sock.sendall(b"GET / HTTP/1.1\r\n")
        start = time.monotonic()
        wait_for_close(sock, header_ms / 1000.0 + 5.0)
        elapsed = time.monotonic() - start
        sock.close()
        return elapsed, rtt

    def moved(before, after, key):
        if not before or not after:
            return None
        return after.get(key, 0) - before.get(key, 0)

    before = _safe_stats(cfg)
    idle_raw, idle_rtt = measure_idle()
    after_idle = _safe_stats(cfg)
    settle(cfg, quiet_s=0.5, deadline_s=20)
    header_raw, header_rtt = measure_header()
    after_header = _safe_stats(cfg)

    idle_actual = idle_raw + idle_rtt / 2.0
    header_actual = header_raw - header_rtt
    idle_closes = moved(before, after_idle, "disconnect_timeout_idle")
    header_closes = moved(after_idle, after_header, "disconnect_timeout_header")

    idle_ok = (abs(idle_actual - idle_ms / 1000.0) <= TIMEOUT_PRECISION_S
               and idle_closes is not None and idle_closes >= 1)
    header_ok = (abs(header_actual - header_ms / 1000.0) <= TIMEOUT_PRECISION_S
                 and header_closes is not None and header_closes >= 1)
    return {
        "idle_configured_s": idle_ms / 1000.0,
        "idle_actual_s": round(idle_actual, 3),
        "idle_raw_s": round(idle_raw, 3),
        "idle_rtt_ms": round(idle_rtt * 1000.0, 1),
        "idle_timeout_closes": idle_closes,
        "header_configured_s": header_ms / 1000.0,
        "header_actual_s": round(header_actual, 3),
        "header_raw_s": round(header_raw, 3),
        "header_rtt_ms": round(header_rtt * 1000.0, 1),
        "header_timeout_closes": header_closes,
        "precision_s": TIMEOUT_PRECISION_S,
        "pass": bool(idle_ok and header_ok),
    }


def t15_footprint(cfg):
    """Post-init heap growth must stay zero on static/generator paths (AR-1)."""
    reset_stats(cfg.host, cfg.port, cfg.timeout)

    def burst(path, count):
        conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
        try:
            for _ in range(count):
                if conn.closed:
                    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                conn.request("GET", path, keep_alive=True)
        finally:
            conn.close()

    burst("/ping", 200)
    for size in (1024, 8192, 65536, 262144):
        burst(f"/payload/{size}", 5)
    stats = _safe_stats(cfg) or {}
    growth = stats.get("post_init_heap_growth_bytes")
    return {
        "post_init_heap_growth_bytes": growth,
        "pass": growth == 0,
    }


def t16_chunked_download(cfg):
    """Chunked output preserves integrity and connection reuse across sizes (NR-6)."""
    sizes = cfg.sizes or [1024, 65536, 262144]
    per_size = []
    all_ok = True
    for size in sizes:
        reset_stats(cfg.host, cfg.port, cfg.timeout)
        resp = one_shot(cfg.host, cfg.port, cfg.timeout,
                        "GET", "{}{}".format("/stream/", size))
        te = resp.headers.get("transfer-encoding", "").lower()
        has_cl = "content-length" in resp.headers
        bit_exact = resp.body == expected_pattern(size)
        ok = (resp.status == 200 and "chunked" in te and not has_cl and
              not resp.short_read and bit_exact)
        all_ok = all_ok and ok
        stats = _safe_stats(cfg) or {}
        per_size.append({
            "size": size,
            "status": resp.status,
            "transfer_encoding_chunked": "chunked" in te,
            "content_length_absent": not has_cl,
            "received": len(resp.body),
            "bit_exact": bit_exact,
            "backpressure_events": stats.get("backpressure_events"),
            "pass": bool(ok),
        })

    # Keep-alive must survive the self-delimiting chunked response.
    keep_alive_ok = False
    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
    try:
        r1 = conn.request("GET", "/stream/4096", keep_alive=True)
        r2 = conn.request("GET", "/ping", keep_alive=False)
        keep_alive_ok = (r1.status == 200 and not r1.short_read and
                         r2.status == 200 and r2.body == b"pong")
    except (OSError, HttpError):
        keep_alive_ok = False
    finally:
        conn.close()

    return {
        "per_size": per_size,
        "keep_alive_after_chunked": keep_alive_ok,
        "pass": bool(all_ok and keep_alive_ok),
    }


def _body_mode_run(cfg, prefix, size, n):
    """Return latency, throughput and integrity results for repeated GETs.

    A first-response 413 skips an oversized buffered body.
    """
    expected = expected_pattern(size)
    max_per_conn = max(1, cfg.info.get("keep_alive_max_requests", 16) - 1)
    samples = []
    throughputs = []
    bit_exact = True
    status_ok = True
    too_large = False
    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
    sent = 0
    try:
        for i in range(n):
            if sent >= max_per_conn:
                conn.close()
                conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                sent = 0
            resp = conn.request("GET", f"{prefix}{size}")
            sent += 1
            if resp.status == 413 and i == 0:
                too_large = True
                status_ok = False
                break
            if resp.status != 200 or resp.short_read:
                status_ok = False
                continue
            if resp.body != expected:
                bit_exact = False
            samples.append(resp.elapsed_s * 1000.0)
            if resp.elapsed_s > 0:
                throughputs.append(len(resp.body) / 1024.0 / resp.elapsed_s)
            if resp.server_closed:
                conn.close()
                conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                sent = 0
    except (OSError, HttpError):
        status_ok = False
    finally:
        conn.close()
    return {
        "status_ok": status_ok,
        "bit_exact": bit_exact,
        "too_large": too_large,
        "kbps_avg": round(statistics.mean(throughputs), 1) if throughputs else None,
        "latency": percentiles(samples),
    }


def t17_body_mode_comparison(cfg):
    """Compare latency and throughput for identical static and copied bodies.

    Above-cap buffered bodies return 413 and are skipped.
    """
    sizes = cfg.sizes or [2048]
    n = cfg.count or 100
    per_size = []
    all_ok = True
    for size in sizes:
        reset_stats(cfg.host, cfg.port, cfg.timeout)
        static = _body_mode_run(cfg, "/bench/static/", size, n)
        buffered = _body_mode_run(cfg, "/bench/buffered/", size, n)
        entry = {"size": size, "requests_per_mode": n,
                 "static": static, "buffered": buffered}
        if buffered["too_large"]:
            entry["buffered_skipped_413"] = True
            entry["pass"] = bool(static["status_ok"] and static["bit_exact"])
        else:
            s_kbps = static["kbps_avg"]
            b_kbps = buffered["kbps_avg"]
            deficit = None
            if s_kbps and b_kbps:
                deficit = round((s_kbps - b_kbps) / s_kbps * 100.0, 1)
            entry["buffered_throughput_deficit_pct"] = deficit
            entry["pass"] = bool(
                static["status_ok"] and buffered["status_ok"] and
                static["bit_exact"] and buffered["bit_exact"])
        all_ok = all_ok and entry["pass"]
        per_size.append(entry)
        settle(cfg)
    return {"per_size": per_size, "pass": bool(all_ok)}


def _ping_under_load(cfg, heavy_path, burn_ms, n_ping, n_heavy):
    """Measure ping latency under heavy-route load.

    Heavy-route latency distinguishes failed load generation from load that
    leaves ping unaffected.
    """
    stop = threading.Event()
    heavy_samples = []
    heavy_errors = []

    def hammer():
        conn = None
        sent = 0
        while not stop.is_set() and sent < n_heavy:
            try:
                if conn is None:
                    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                resp = conn.request("GET", f"{heavy_path}{burn_ms}")
                sent += 1
                if resp.status != 200:
                    heavy_errors.append(resp.status)
                heavy_samples.append(resp.elapsed_s * 1000.0)
                if resp.server_closed:
                    conn = None
            except (HttpError, OSError) as exc:
                heavy_errors.append(str(exc))
                if conn:
                    conn.close()
                conn = None
        if conn:
            conn.close()

    worker = threading.Thread(target=hammer, daemon=True)
    worker.start()
    time.sleep(0.2)  # Load starts before ping sampling.

    ping_samples = []
    conn = None
    sent_on_conn = 0
    max_per_conn = max(1, cfg.info.get("keep_alive_max_requests", 16) - 1)
    try:
        for _ in range(n_ping):
            if conn is None or sent_on_conn >= max_per_conn:
                if conn:
                    conn.close()
                conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                sent_on_conn = 0
            resp = conn.request("GET", "/ping")
            sent_on_conn += 1
            if resp.status != 200:
                raise HttpError(f"unexpected /ping status {resp.status}")
            ping_samples.append(resp.elapsed_s * 1000.0)
            if resp.server_closed:
                conn = None
    finally:
        stop.set()
        worker.join(timeout=30)
        if conn:
            conn.close()

    return {"ping": percentiles(ping_samples), "heavy_requests": len(heavy_samples),
            "heavy": percentiles(heavy_samples) if heavy_samples else None,
            "heavy_errors": heavy_errors[:5]}


def t19_deferred_handler(cfg):
    """Check deferred dispatch, work and pool accounting (AR-9).

    Latency and improvement are review evidence.
    """
    burn = cfg.burn_ms or 10
    n_ping = cfg.count or 100
    n_heavy = max(8, n_ping // 4)

    reset_stats(cfg.host, cfg.port, cfg.timeout)
    sync = _ping_under_load(cfg, "/heavy/", burn, n_ping, n_heavy)
    settle(cfg)

    reset_stats(cfg.host, cfg.port, cfg.timeout)
    deferred = _ping_under_load(cfg, "/heavy-deferred/", burn, n_ping, n_heavy)
    stats = get_stats(cfg.host, cfg.port, cfg.timeout)
    settle(cfg)

    sync_p99 = sync["ping"].get("p99_ms")
    def_p99 = deferred["ping"].get("p99_ms")
    pr1_p99_ceiling = 40.0

    improvement = None
    if sync_p99 and def_p99:
        improvement = round((sync_p99 - def_p99) / sync_p99 * 100.0, 1)

    # The deferred run must defer. Offered-load ratios expose serial workers
    # doing less heavy work when their requests take longer.
    sync_heavy = sync["heavy_requests"]
    def_heavy = deferred["heavy_requests"]
    load_ratio = round(def_heavy / sync_heavy, 3) if sync_heavy else 0.0
    load_matched = load_ratio >= 0.8

    dispatched = stats.get("pending_dispatches", 0)
    exhausted = stats.get("deferrals_refused", 0)
    handler_timeouts = stats.get("disconnect_timeout_handler", 0)
    # pending_responses counts parked deferrals; one is tolerated (BR-12).
    held = stats.get("pending_responses", 0)
    stranded = max(0, held - 1)

    ok = bool(
        dispatched >= def_heavy > 0 and
        stranded == 0 and
        handler_timeouts == 0 and
        not sync["heavy_errors"] and not deferred["heavy_errors"])

    return {
        "burn_ms": burn,
        "synchronous": sync,
        "deferred": deferred,
        "ping_p99_improvement_pct": improvement,
        "sync_heavy_requests": sync_heavy,
        "deferred_heavy_requests": def_heavy,
        "offered_load_ratio": load_ratio,
        "load_matched": load_matched,
        "pr1_p99_ceiling_ms": pr1_p99_ceiling,
        "pending_dispatches": dispatched,
        "deferrals_refused": exhausted,
        "pending_high_watermark": stats.get("pending_high_watermark", 0),
        "pending_responses_held": held,
        "pending_responses_stranded": stranded,
        "disconnect_timeout_handler": handler_timeouts,
        "pass": ok,
    }


def t18_heap_mechanism(cfg):
    """Check idle heap after static, generated and copied-body workloads (AR-1).

    Warm-up and reset establish the baseline without rebooting. Host tests
    check the in-flight bound and oversized rejection (BR-14).
    """
    maxc = cfg.info.get("max_connections",
                        cfg.info.get("memp_num_tcp_pcb", 8))
    max_body = cfg.limits.get("max_buffered_body", 2048)
    bound = maxc * max_body
    probe = min(max_body, 8192)
    # Control and idle copied-body checks treat growth up to this many bytes as
    # zero.
    eps = 64

    def occupy(path, k):
        """Hold k distinct connection slots for one request each, then release them."""
        conns = []
        try:
            conns.extend(HttpConn(cfg.host, cfg.port, cfg.timeout)
                         for _ in range(k))
            for c in conns:
                c.request("GET", path, keep_alive=True)
        finally:
            for c in conns:
                c.close()

    def hammer(path, count):
        conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
        try:
            for _ in range(count):
                if conn.closed:
                    conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                conn.request("GET", path, keep_alive=True)
        finally:
            conn.close()

    def growth():
        return (_safe_stats(cfg) or {}).get("post_init_heap_growth_bytes")

    # Warming /stats on every slot excludes first-use allocations before
    # rebaselining.
    occupy("/stats", maxc)
    reset_stats(cfg.host, cfg.port, cfg.timeout)

    hammer("/ping", 1500)
    hammer(f"/payload/{probe}", 1500)
    hammer(f"/bench/static/{probe}", 1500)
    g_control = growth()

    sat = []
    for _ in range(4):
        occupy(f"/bench/buffered/{probe}", maxc)
        sat.append(growth())

    hammer(f"/bench/buffered/{probe}", 1500)
    g_reuse = growth()
    smaller = max(64, probe // 8)
    hammer(f"/bench/buffered/{smaller}", 1500)
    g_smaller = growth()

    def ok(v):
        return v is not None

    control_zero = ok(g_control) and g_control <= eps
    copied = [*sat, g_reuse, g_smaller]
    copied_released = all(ok(g) and g <= eps for g in copied)
    return {
        "max_connections": maxc,
        "max_buffered_body": max_body,
        "copied_body_heap_bound": bound,
        "probe_body_bytes": probe,
        "control_growth_bytes": g_control,
        "saturation_growth_bytes": sat,
        "reuse_growth_bytes": g_reuse,
        "smaller_body_bytes": smaller,
        "smaller_growth_bytes": g_smaller,
        "control_zero": control_zero,
        "copied_released": copied_released,
        "pass": bool(control_zero and copied_released),
    }


def _recover(cfg):
    """Ping checks device responsiveness."""
    with contextlib.suppress(OSError, HttpError):
        one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")


def settle(cfg, quiet_s=1.5, deadline_s=30):
    """Wait for ping, then pause quiet_s for lingering PCBs to clear."""
    start = time.time()
    while time.time() - start < deadline_s:
        try:
            resp = one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")
            if resp.status == 200:
                time.sleep(quiet_s)
                return True
        except (OSError, HttpError):
            pass
        time.sleep(1.0)
    return False

# Disconnect counts plus live connections account for all accepts (RR-4).
DISCONNECT_REASONS = ("disconnect_peer_closed", "disconnect_peer_reset",
                      "disconnect_aborted", "disconnect_timeout",
                      "disconnect_server_closed")
TIMEOUT_KINDS = ("header", "body", "idle", "write", "drain", "handler")
PARSE_ERROR_CLASSES = (400, 413, 414, 417, 431, 501, 505)
# The deadline is in seconds and exceeds the 15 s idle timeout.
# A stray keep-alive connection expires within it.
SLOT_RELEASE_DEADLINE_S = 20


def counters_consistent(stats, open_at_reset):
    """Counter totals account for accepted and initially open connections (RR-4)."""
    closed = sum(stats.get(k, 0) for k in DISCONNECT_REASONS)
    timeouts = sum(stats.get(f"disconnect_timeout_{k}", 0) for k in TIMEOUT_KINDS)
    parse = sum(stats.get(f"parse_errors_{c}", 0) for c in PARSE_ERROR_CLASSES)
    connections = (stats.get("accepts", 0) + open_at_reset ==
                   closed + stats.get("active_connections", 0))
    return {
        "connections_balance": connections,
        "timeouts_sum": timeouts == stats.get("disconnect_timeout", 0),
        "parse_errors_sum": parse == stats.get("parse_errors", 0),
        "ok": bool(connections and timeouts == stats.get("disconnect_timeout", 0)
                   and parse == stats.get("parse_errors", 0)),
    }


def accounting_verdict(final_stats, open_at_reset):
    """Unreadable counters fail the accounting check."""
    if final_stats is None:
        return {"ok": False, "reason": "no /stats reading after the run"}
    if open_at_reset is None:
        return {"ok": False, "reason": "the reset reply carried no open count"}
    return counters_consistent(final_stats, open_at_reset)


def slots_released(cfg, deadline_s=SLOT_RELEASE_DEADLINE_S):
    """Return the last stats and whether only the measuring connection remains.

    The deadline bounds the wait for released slots (RR-4).
    """
    start = time.time()
    stats = None
    while time.time() - start < deadline_s:
        stats = _safe_stats(cfg)
        if stats and stats.get("active_connections") == 1:
            return True, stats
        time.sleep(1.0)
    return False, stats


def _safe_stats(cfg):
    try:
        return get_stats(cfg.host, cfg.port, cfg.timeout)
    except (OSError, HttpError, ValueError):
        return None

# These set the abuse-run size, recovery deadline in seconds and number of
# held slowloris heads.
# Holding fewer heads than slots measures recovery rather than refusals (RR-2).
ABUSE_EVENTS = 1000
ABUSE_RECOVERY_S = 1.0
ABUSE_SLOWLORIS_HELD = 2


def _rst(sock):
    """Close with RST using SO_LINGER(0)."""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.close()


def t21_abuse(cfg):
    """Mixed abuse checks recovery (RR-2) and accounting (RR-4).

    A fresh client must receive ping within ABUSE_RECOVERY_S while slowloris
    heads remain. The device must reclaim them at header timeout (TR-1). Every
    slot must then be released, heap must stay bounded (AR-1), and counters
    must account for all connections. --count cannot reduce the event count.
    """
    n = ABUSE_EVENTS
    limits = getattr(cfg, "limits", {}) or {}
    over_line = limits.get("max_header_line", 322) + 64
    host = cfg.host.encode()
    open_at_reset = reset_stats(cfg.host, cfg.port, cfg.timeout).get(
        "active_connections")
    kinds = ("rst", "malformed", "oversize", "slowloris")
    expect = {"malformed": 400, "oversize": 431}
    counts = dict.fromkeys(kinds, 0)
    wrong_status = []
    failures = 0
    held = []
    started = time.time()
    for i in range(n):
        kind = kinds[i % len(kinds)]
        try:
            if kind in expect:
                conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
                if kind == "malformed":
                    conn.sock.sendall(b"NOT-HTTP\r\n\r\n")
                else:
                    conn.sock.sendall(b"GET / HTTP/1.1\r\nHost: " + host +
                                      b"\r\nX: " + b"a" * over_line +
                                      b"\r\n\r\n")
                resp = conn.read_response()
                conn.close()
                if resp.status != expect[kind]:
                    wrong_status.append({"kind": kind, "status": resp.status})
            else:
                sock = connect_with_retry(cfg.host, cfg.port, cfg.timeout)
                sock.sendall(b"GET /ping HTTP/1.1\r\nHost: " + host + b"\r\n")
                if kind == "rst":
                    _rst(sock)
                else:
                    held.append(sock)
                    if len(held) > ABUSE_SLOWLORIS_HELD:
                        _rst(held.pop(0))
            counts[kind] += 1
        except (OSError, HttpError):
            failures += 1
    for sock in held:
        with contextlib.suppress(OSError):
            _rst(sock)
    duration_s = time.time() - started

    # Held slowloris heads remain during the recovery check until their header
    # timeout.
    lingering = []
    for _ in range(ABUSE_SLOWLORIS_HELD):
        try:
            sock = connect_with_retry(cfg.host, cfg.port, cfg.timeout)
            sock.sendall(b"GET /ping HTTP/1.1\r\nHost: " + host + b"\r\n")
            lingering.append(sock)
        except OSError:
            failures += 1

    # Recovery time includes connect and the full ping response (RR-2).
    t0 = time.time()
    try:
        resp = one_shot(cfg.host, cfg.port, cfg.timeout, "GET", "/ping")
        ping_ok = resp.status == 200 and resp.body == b"pong"
    except (OSError, HttpError):
        ping_ok = False
    ping_s = time.time() - t0
    recovered = ping_ok and ping_s <= ABUSE_RECOVERY_S

    released, final_stats = slots_released(cfg)
    for sock in lingering:
        with contextlib.suppress(OSError):
            sock.close()
    consistency = accounting_verdict(final_stats, open_at_reset)
    stats = final_stats or {}
    reclaimed = stats.get("disconnect_timeout_header", 0) >= len(lingering) > 0
    growth = stats.get("post_init_heap_growth_bytes")
    bound = copied_body_heap_bound(cfg)
    heap_ok = growth is not None and growth <= bound
    return {
        "events": n,
        "duration_s": round(duration_s, 1),
        "per_kind": counts,
        "client_failures": failures,
        "wrong_status": wrong_status[:10],
        "wrong_status_count": len(wrong_status),
        "fresh_ping_s": round(ping_s, 3),
        "recovery_deadline_s": ABUSE_RECOVERY_S,
        "recovered": recovered,
        "slowloris_held": len(lingering),
        "slowloris_reclaimed": reclaimed,
        "post_init_heap_growth_bytes": growth,
        "copied_body_heap_bound": bound,
        "heap_within_bound": heap_ok,
        "slots_released": released,
        "open_at_reset": open_at_reset,
        "counters": consistency,
        "pass": bool(failures == 0 and not wrong_status and recovered and
                     reclaimed and heap_ok and released and consistency["ok"]),
    }

# The pipeline exceeds either profile receive window, including 5840 B on
# P8-RP2350 (RR-6).
POOL_PIPELINED_BYTES = 8192
POOL_DOWNLOAD_BYTES = 65536
POOL_HOLD_S = 1.0
# The padding is in bytes, under the default 256 B header-value cap.
POOL_PAD_VALUE = 200


def t22_pool_pressure(cfg):
    """Parallel full-window downloads test pooled receive storage (RR-6).

    Pool peaks are measured since boot.
    """
    maxc = cfg.info.get("max_connections", 8)
    # The fixed size exercises the receive-pool requirement (RR-6).
    size = POOL_DOWNLOAD_BYTES
    host = cfg.host.encode()
    max_per_conn = max(2, cfg.info.get("keep_alive_max_requests", 16) - 1)
    pings = max_per_conn - 1
    bare = b"GET /ping HTTP/1.1\r\nHost: %s\r\n\r\n" % host
    pad_lines = []
    pad = max(0, -(-POOL_PIPELINED_BYTES // pings) - len(bare))
    while pad > 0:
        # Distinct names avoid repeated-field combination (NR-10).
        name = b"X-Pad-%d: " % len(pad_lines)
        value = b"p" * max(1, min(POOL_PAD_VALUE, pad - len(name) - 2))
        line = name + value + b"\r\n"
        pad_lines.append(line)
        pad -= len(line)
    ping = bare[:-2] + b"".join(pad_lines) + b"\r\n"
    request = (b"GET /payload/%d HTTP/1.1\r\nHost: %s\r\n\r\n" % (size, host) +
               ping * pings)
    before = _safe_lwip(cfg)
    reset_stats(cfg.host, cfg.port, cfg.timeout)
    conns = []
    errors = []
    try:
        for _ in range(maxc):
            conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
            conn.sock.sendall(request)
            conns.append(conn)
        time.sleep(POOL_HOLD_S)
        expected = expected_pattern(size)
        for conn in conns:
            first = _read_response_paced(conn, pace_s=0.0)
            if first.status != 200 or first.body != expected:
                errors.append("download")
            for _ in range(pings):
                resp = conn.read_response(keep_alive=True)
                if resp.status != 200 or resp.body != b"pong":
                    errors.append("ping")
    except (OSError, HttpError) as exc:
        errors.append(str(exc))
    finally:
        for conn in conns:
            with contextlib.suppress(OSError):
                conn.close()
    stats = _safe_stats(cfg) or {}
    after = _safe_lwip(cfg)
    pool = after.get("pbuf_pool", {})
    pool_errors = pool.get("err", 0) - before.get("pbuf_pool", {}).get("err", 0)
    lwip_failures = lwip_alloc_failures(before, after)
    drops = stats.get("rx_overflow_drops")
    throttles = stats.get("rx_window_throttle_events", 0)
    responsive = settle(cfg, quiet_s=0.0, deadline_s=10)
    return {
        "connections": maxc,
        "download_bytes": size,
        "pipelined_pings": pings,
        "pipelined_bytes": len(ping) * pings,
        "errors": errors[:10],
        "error_count": len(errors),
        "rx_overflow_drops": drops,
        "rx_window_throttle_events": throttles,
        "pbuf_pool_max_since_boot": pool.get("max"),
        "pbuf_pool_avail": pool.get("avail"),
        "pbuf_pool_errors": pool_errors,
        "lwip_alloc_failures": lwip_failures,
        "responsive": responsive,
        # The count shows at least one throttle per held slot, without
        # identifying windows.
        "pass": bool(not errors and drops == 0 and throttles >= maxc and
                     responsive and lwip_alloc_clean(lwip_failures)),
    }


def _safe_lwip(cfg):
    try:
        return get_json(cfg.host, cfg.port, cfg.timeout, "/lwip_stats")
    except (OSError, HttpError, ValueError):
        return {}

# Allocation failures are counted separately from tcp.memerr, which also
# includes full send queues. The transport retries ERR_MEM writes on poll, so
# requests alone cannot expose allocation failure.
LWIP_ALLOC_FAILURES = (("mem", "err"), ("pbuf_pool", "err"),
                       ("pbuf_ref", "err"), ("tcp_seg", "err"))
LWIP_RECORDED = (("tcp", "memerr"),)


def lwip_alloc_failures(before, after):
    """Return allocation-failure deltas, or None if unread.

    Missing counters fail the caller.
    """
    deltas = {}
    for group, key in LWIP_ALLOC_FAILURES + LWIP_RECORDED:
        if group not in before or group not in after:
            return None
        deltas[f"{group}.{key}"] = (after[group].get(key, 0) -
                                    before[group].get(key, 0))
    return deltas


def lwip_alloc_clean(failures):
    """Check allocation-failure deltas. LWIP_RECORDED counts are reported for review."""
    if failures is None:
        return False
    return not any(failures[f"{group}.{key}"]
                   for group, key in LWIP_ALLOC_FAILURES)

TESTS = {
    "t1": ("Latency (keep-alive GET /ping)", t1_latency),
    "t2": ("Connection churn: device accept rate + latency (PR-4)",
           t2_churn),
    "t3": ("Download throughput vs size", t3_download),
    "t4": ("Upload throughput + integrity", t4_upload),
    "t5": ("Concurrency scaling (PR-3, RR-3; AR-3 debug-only)",
           t5_concurrency),
    "t6": ("Keep-alive semantics and reuse rate (RR-5)", t6_keepalive),
    "t7": ("Soak", t7_soak),
    "t8": ("RST robustness", t8_rst_robustness),
    "t9": ("Slowloris defense (TR-1)", t9_slowloris),
    "t10": ("Boundary matrix (BR-1..9)", t10_boundary_matrix),
    "t11": ("Backpressure download 256 KB (PR-8)", t11_backpressure_download),
    "t12": ("Slow-reader isolation + TR-4 (AR-5)", t12_slow_reader),
    "t13": ("RX flow control under backpressure (AR-6)", t13_rx_flow_control),
    "t14": ("Timeout precision (TR-1/TR-3)", t14_timeout_precision),
    "t15": ("Static footprint / zero heap growth (AR-1)", t15_footprint),
    "t16": ("Chunked download (NR-6)", t16_chunked_download),
    "t17": ("Body-mode comparison BUFFERED vs STATIC_VIEW (AR-2, AR-11)",
            t17_body_mode_comparison),
    "t18": ("Copied-body heap mechanism (AR-1, opt-in)",
            t18_heap_mechanism),
    "t19": ("Deferred handler vs synchronous under load (AR-9)",
            t19_deferred_handler),
    "t20": ("Aggregate download, max_connections (PR-6, PR-9)",
            t20_aggregate_download),
    "t21": ("Mixed abuse, recovery and accounting (RR-2, RR-4, opt-in)",
            t21_abuse),
    "t22": ("Every slot against the pbuf pool (RR-6)", t22_pool_pressure),
}


class Config:
    pass


def print_table(results):
    print("\n" + "=" * 60)
    print("BENCHMARK SUMMARY")
    print("=" * 60)
    for key, data in results.items():
        title = TESTS.get(key, (key, None))[0]
        print(f"\n[{key}] {title}")
        if isinstance(data, dict) and "error" in data:
            print("  ERROR: {}".format(data["error"]))
            continue
        if key == "t1":
            print("  p50={p50_ms}ms p90={p90_ms}ms p99={p99_ms}ms "
                  "rps={rps}".format(**data))
            if data.get("ttfb"):
                print("  ttfb p50={p50_ms}ms p99={p99_ms}ms".format(
                    **data["ttfb"]))
            if data.get("internal"):
                print("  internal: {receive_within_1ms}/{receive_samples} "
                      "receive callbacks processed within 1 ms "
                      "(>= {target_share}), max {receive_max_us}us; with the "
                      "radio handoff {receive_total_within_1ms}, max "
                      "{receive_total_max_us}us".format(**data["internal"]))
                print("  --> pass={}".format(data.get("pass")))
        elif key == "t2":
            print("  device accepts {device_accepts_per_s}/s "
                  "(expectation {target_accepts_per_s}) over {workers} workers, "
                  "{connections} connections in {interval_s}s".format(**data))
            print("  client completes {client_completions_per_s}/s  "
                  "stalls={client_stalls} costing {client_stall_s}s  "
                  "slot_refusals={slot_refusals}".format(**data))
            print("  serial latency p50={p50_ms}ms (expectation {target_p50_ms}) "
                  "p99={p99_ms}ms  failures={failures}  "
                  "pass={pass}".format(**data))
        elif key == "t3":
            for e in data["per_size"]:
                print("  {size:>6}B  complete {complete}/{reps}  "
                      "min_recv={received_min:>6}  p10 {kbps_p10} / "
                      "median {kbps_median} / min {kbps_min} KB/s  "
                      "truncated={truncated}".format(**e))
            print("  --> max reliable download: {} bytes".format(
                data["max_reliable_size"]))
            print("  --> PR-5 expectation (p{judged_percentile} "
                  "{target_kbps} KB/s from "
                  "{judged_from_size} B); completion pass={}".format(data.get("pass"),
                                                         **data))
        elif key == "t4":
            for e in data["per_size"]:
                print("  {size:>6}B  status={status}  "
                      "checksum_ok={checksum_ok} KB/s={kbps}".format(
                          **{**e, "kbps": e.get("kbps")}))
            print("  --> max reliable upload: {} bytes".format(
                data["max_reliable_upload"]))
        elif key == "t5":
            for e in data["per_n"]:
                print("  n={n}  rps={rps}  worker_failures={worker_failures}  "
                      "refusals_slot_full={refusals_slot_full_delta}".format(**e))
            print("  PR-3 rps at cap: {rps_at_cap} (expectation {target_rps_at_cap}); "
                  "workload_ok={workload_ok}".format(**data))
            print("  RR-3 refusals over cap: {refusals_over_cap} "
                  "pass={rr3_pass}".format(**data))
            if data.get("ar3_measured"):
                print("  AR-3 lock hold: max={lock_hold_max_us}us "
                      "p99<=500us={lock_hold_p99_within_500us} "
                      "(samples={lock_hold_samples}) pass={ar3_pass}".format(
                          **data))
            else:
                # Release builds have no sampler verdict. The overall result
                # still checks workload correctness and extra-connection
                # rejection (RR-3).
                print("  AR-3 lock hold: not measured (release build; "
                      "debug-only instrumentation)")
            print("  pass={pass}".format(**data))
        elif key == "t6":
            print("  observed={max_requests_observed}  "
                  "keep_alive_seen={keep_alive_header_seen}  "
                  "close_on_last={close_header_on_last}".format(**data))
            print("  RR-5 reuse rate: {reuse_rate} "
                  "({reuses_honoured}/{reuse_attempts}, "
                  "premature_closes={premature_closes}, "
                  ">= {target_reuse_rate})  pass={pass}".format(**data))
        elif key == "t7":
            print("  requests={requests} errors={errors} "
                  "heap {heap_start}->{heap_end} "
                  "growth={post_init_heap_growth_bytes}B "
                  "plateau_spread={heap_plateau_spread_bytes}B "
                  "plateau_ok={heap_plateau_ok} "
                  "released={slots_released} "
                  "counters={counters} "
                  "lwip_alloc_failures={lwip_alloc_failures} "
                  "pass={pass}".format(**data))
        elif key == "t22":
            print("  {connections} connections x ({download_bytes}B + "
                  "{pipelined_pings} pings) errors={error_count} "
                  "drops={rx_overflow_drops} "
                  "throttles={rx_window_throttle_events}".format(**data))
            print("  pbuf_pool max since boot={pbuf_pool_max_since_boot}/"
                  "{pbuf_pool_avail} "
                  "alloc_errors={pbuf_pool_errors} "
                  "responsive={responsive}".format(**data))
            print("  lwip_alloc_failures={lwip_alloc_failures}".format(**data))
            print("  --> pass={}".format(data["pass"]))
        elif key == "t21":
            print("  events={events} per_kind={per_kind} "
                  "client_failures={client_failures} "
                  "wrong_status={wrong_status_count}".format(**data))
            print("  {duration_s}s; fresh /ping {fresh_ping_s}s "
                  "(<= {recovery_deadline_s}s) with {slowloris_held} heads held "
                  "recovered={recovered} reclaimed={slowloris_reclaimed}".format(
                      **data))
            print("  heap growth={post_init_heap_growth_bytes}B "
                  "(<= {copied_body_heap_bound}B) released={slots_released} "
                  "counters={counters}".format(**data))
            print("  --> pass={}".format(data["pass"]))
        elif key == "t8":
            print("  n={n} rst_failures={rst_failures} "
                  "resets={disconnect_peer_reset}/{expected_resets} "
                  "active_after_burst={active_connections_after_burst} "
                  "fresh_ok={fresh_connections_ok}/{max_connections} "
                  "pass={pass}".format(**data))
        elif key == "t9":
            print("  closed_at={closed_at_s}s (target {header_timeout_s}s) "
                  "served_during={served_during} pass={pass}".format(**data))
        elif key == "t10":
            for c in data["cases"]:
                print("  {case:<22} status={status} expect={expect} "
                      "ok={ok}".format(**c))
            print("  --> pass={}".format(data["pass"]))
        elif key == "t11":
            print("  size={size} received={received} bit_exact={bit_exact} "
                  "backpressure={backpressure_events} pass={pass}".format(**data))
        elif key == "t12":
            print("  probes_served={probes_served} (p50={probe_p50_ms}ms "
                  "p99={probe_p99_ms}ms fail={probe_failures})  "
                  "disconnect_timeout_write={disconnect_timeout_write}  "
                  "pass={pass}".format(**data))
        elif key == "t13":
            print("  first={first_status} bit_exact={first_bit_exact}  "
                  "second={second_status} body_ok={second_body_ok}  "
                  "throttle_events={rx_window_throttle_events}  "
                  "overflow_drops={rx_overflow_drops}  "
                  "pass={pass}".format(**data))
        elif key == "t14":
            print("  idle {idle_actual_s}s/{idle_configured_s}s "
                  "(raw {idle_raw_s}s, rtt {idle_rtt_ms}ms, "
                  "closes={idle_timeout_closes})".format(**data))
            print("  header {header_actual_s}s/{header_configured_s}s "
                  "(raw {header_raw_s}s, rtt {header_rtt_ms}ms, "
                  "closes={header_timeout_closes})".format(**data))
            print("  (+/-{precision_s}s)  pass={pass}".format(**data))
        elif key == "t15":
            print("  post_init_heap_growth={post_init_heap_growth_bytes}B "
                  "pass={pass}".format(**data))
        elif key == "t16":
            for e in data["per_size"]:
                print("  {size:>6}B  status={status} chunked={te} "
                      "no_content_length={cl}  received={received:>6}  "
                      "bit_exact={bx}  backpressure={bp}".format(
                          size=e["size"], status=e["status"],
                          te=e["transfer_encoding_chunked"],
                          cl=e["content_length_absent"],
                          received=e["received"], bx=e["bit_exact"],
                          bp=e["backpressure_events"]))
            print("  keep_alive_after_chunked={keep_alive_after_chunked}  "
                  "pass={pass}".format(**data))
        elif key == "t17":
            for e in data["per_size"]:
                if e.get("buffered_skipped_413"):
                    print("  {size:>6}B  static {s} KB/s  buffered SKIPPED "
                          "(413: exceeds max_buffered_body)".format(
                              size=e["size"], s=e["static"]["kbps_avg"]))
                else:
                    print("  {size:>6}B  static {s} KB/s  buffered {b} KB/s  "
                          "deficit={d}%  justifies_opt={j}".format(
                              size=e["size"], s=e["static"]["kbps_avg"],
                              b=e["buffered"]["kbps_avg"],
                              d=e.get("buffered_throughput_deficit_pct"),
                              j=e.get("justifies_optimization")))
            print("  --> pass={}".format(data["pass"]))
        elif key == "t18":
            print("  bound={copied_body_heap_bound}B "
                  "(max_connections={max_connections} x "
                  "max_buffered_body={max_buffered_body}) probe={probe_body_bytes}B"
                  .format(**data))
            print("  control(static+gen 4500 req)={control_growth_bytes}B "
                  "-> zero={control_zero}".format(**data))
            print("  saturation bursts={saturation_growth_bytes} "
                  "reuse={reuse_growth_bytes}B "
                  "smaller({smaller_body_bytes}B)={smaller_growth_bytes}B".format(
                      **data))
            print("  released={copied_released}".format(**data))
            print("  --> pass={}".format(data.get("pass")))
        elif key == "t19":
            print("  burn={burn_ms}ms  heavy sync={sync_heavy_requests} "
                  "deferred={deferred_heavy_requests} "
                  "ratio={offered_load_ratio} matched={load_matched}"
                  .format(**data))
            print("  ping p99 sync={s}ms deferred={d}ms improvement={i}% "
                  "(expectation {c}ms)".format(
                      s=(data["synchronous"]["ping"] or {}).get("p99_ms"),
                      d=(data["deferred"]["ping"] or {}).get("p99_ms"),
                      i=data.get("ping_p99_improvement_pct"),
                      c=data.get("pr1_p99_ceiling_ms")))
            print("  dispatched={pending_dispatches} refused={deferrals_refused} "
                  "stranded={pending_responses_stranded} "
                  "handler_timeouts={disconnect_timeout_handler}".format(**data))
            print("  --> pass={}".format(data.get("pass")))
        elif key == "t20":
            print("  conns={connections} size={size}B complete={complete} "
                  "wall={wall_s}s".format(**data))
            print("  aggregate={aggregate_kbps} KB/s  "
                  "stall_free={stall_free_kbps} KB/s "
                  "(expectation {target_kbps} KB/s)".format(**data))
            print("  stalled_connections={stalled_connections} "
                  "worst_gap={worst_gap_s}s "
                  "(PR-9 expectation {gap_ceiling_s}s = 2 x {tcp_rto_ms}ms RTO "
                  "+ {segment_delivery_s}s delivery)".format(**data))
            print("  lwip_alloc_failures={}".format(
                data.get("lwip_alloc_failures")))
            print("  --> pass={}".format(data.get("pass")))
    print()


def rf_health(cfg):
    """Fifteen keep-alive pings bracket the sweep as review evidence (PR-1)."""
    samples = []
    errors = 0
    conn = None
    try:
        conn = HttpConn(cfg.host, cfg.port, cfg.timeout)
        for _ in range(15):
            response = conn.request("GET", "/ping", keep_alive=True)
            if response.status == 200:
                samples.append(response.elapsed_s * 1000.0)
            else:
                errors += 1
    except (OSError, HttpError):
        errors += 1
    finally:
        if conn is not None:
            conn.close()
    summary = percentiles(samples)
    return {"samples": len(samples), "errors": errors,
            **{key: summary.get(key) for key in ("min_ms", "p50_ms", "max_ms")}}


def history_comparison(results, info):
    """Record numeric comparisons without a performance verdict (PR-1/PR-9)."""
    profile = info.get("profile", "default")
    suffix = "-debug-t5" if info.get("build_type") == "Debug" else ""
    filename = "pico2w-" + profile.replace("-", "") + suffix + ".json"
    path = Path(__file__).parent / "baselines" / filename
    try:
        document = json.loads(path.read_text())
        conditions = document["conditions"]
        if any(conditions.get(key) != info.get(key)
               for key in ("profile", "build_type", "board")):
            return {"file": path.name, "unavailable": "reference conditions differ"}
        reference = document["results"]
    except (OSError, ValueError, KeyError) as error:
        return {"file": path.name, "unavailable": str(error)}
    names = {"p50_ms", "p90_ms", "p99_ms", "rps", "rps_at_cap", "kbps",
             "kbps_avg", "kbps_median", "kbps_p10", "kbps_min", "aggregate_kbps",
             "stall_free_kbps", "worst_gap_s", "device_accepts_per_s",
             "client_completions_per_s", "ping_p99_improvement_pct"}

    def figures(value, prefix=""):
        found = {}
        if isinstance(value, dict):
            for key, item in value.items():
                label = prefix + "/" + key
                if key in names and type(item) in (int, float):
                    found[label] = item
                elif isinstance(item, (dict, list)):
                    found.update(figures(item, label))
        elif isinstance(value, list):
            for index, item in enumerate(value):
                label = (item.get("size", item.get("n", index))
                         if isinstance(item, dict) else index)
                found.update(figures(item, prefix + "/" + str(label)))
        return found

    current, previous = figures(results), figures(reference)
    return {"file": path.name, "figures": {
        key: {"measured": value, "reference": previous[key],
              "change_percent": round((value / previous[key] - 1) * 100, 2)
              if previous[key] else None}
        for key, value in current.items() if key in previous}}


def read_config(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="device IP or hostname")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--tests", default=",".join(DEFAULT_TESTS),
                        help="comma-separated test ids ({})".format(
                            ",".join(ALL_TESTS)))
    parser.add_argument("--sizes", default="",
                        help="override T3 download sizes (comma-separated)")
    parser.add_argument("--upload-sizes", default="",
                        help="override T4 upload sizes (comma-separated)")
    parser.add_argument("--count", type=int, default=0,
                        help="override request count (t1, t2, t7, t17, t19)")
    parser.add_argument("--body-bytes", type=int, default=0,
                        help="override body size in bytes (t11, t13, t20)")
    parser.add_argument("--aggregate-kbps", type=float, default=600.0,
                        help="PR-6 recorded throughput expectation, KB/s (t20)")
    parser.add_argument("--warmup", type=int, default=WARMUP_SAMPLES,
                        help="requests issued but not sampled before t1 and t2 "
                             "(0 reproduces a measurement taken without one)")
    parser.add_argument("--duration", type=int, default=60,
                        help="T7 soak duration in seconds")
    parser.add_argument("--burn-ms", type=int, default=0,
                        help="t19 handler CPU burn per request (default 10)")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--output-dir", default="results")
    parser.add_argument("--wait", action="store_true",
                        help="poll /ping until the device is up before starting")
    args = parser.parse_args(argv)

    cfg = Config()
    cfg.host = args.host
    cfg.port = args.port
    cfg.timeout = args.timeout
    cfg.count = args.count
    cfg.body_bytes = args.body_bytes
    cfg.aggregate_kbps = args.aggregate_kbps
    cfg.duration = args.duration
    cfg.sizes = [int(x) for x in args.sizes.split(",") if x] or None
    cfg.burn_ms = args.burn_ms or None
    cfg.upload_sizes = [int(x) for x in args.upload_sizes.split(",") if x] or None

    return args, cfg


def main(argv=None):
    args, cfg = read_config(argv)

    if args.wait:
        print(f"Waiting for {cfg.host}:{cfg.port} ...")
        if not wait_until_up(cfg.host, cfg.port, cfg.timeout):
            print("error: device did not come up", file=sys.stderr)
            return 1

    try:
        cfg.info = get_info(cfg.host, cfg.port, cfg.timeout)
    except (OSError, HttpError, ValueError) as exc:
        print(f"error: cannot reach /info on {cfg.host}:{cfg.port}: {exc}",
              file=sys.stderr)
        return 1
    print("Device info:")
    print(json.dumps(cfg.info, indent=2))

    # Device-reported limits drive boundary and timeout tests (OR-3).
    try:
        cfg.limits = get_json(cfg.host, cfg.port, cfg.timeout, "/limits")
    except (OSError, HttpError, ValueError):
        cfg.limits = {}

    selected = [t.strip() for t in args.tests.split(",") if t.strip()]

    for _ in range(5):
        _recover(cfg)

    health = {"before": rf_health(cfg)}
    results = {}
    for key in selected:
        if key not in TESTS:
            print(f"skipping unknown test '{key}'", file=sys.stderr)
            continue
        title, func = TESTS[key]
        print(f"\n>>> Running [{key}] {title} ...")
        # A drained PCB pool prevents preceding churn from affecting this
        # test.
        settle(cfg)
        reset_stats(cfg.host, cfg.port, cfg.timeout)
        try:
            results[key] = func(cfg)
        # A failed case is recorded before the run continues.
        except Exception as exc:  # noqa: BLE001
            results[key] = {"error": str(exc)}
            print(f"  test errored: {exc}", file=sys.stderr)

    health["after"] = rf_health(cfg)
    print_table(results)
    print("RF health before: " + json.dumps(health["before"]))
    print("RF health after: " + json.dumps(health["after"]))
    comparison = history_comparison(results, cfg.info)
    print("History comparison (review evidence): " + json.dumps(comparison))

    # UTC timestamps order runs consistently across client timezones.
    now = datetime.datetime.now(datetime.UTC)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    out_path = output_dir / f"bench-{now:%Y%m%d-%H%M%S}.json"
    document = {
        "timestamp": now.isoformat(timespec="seconds"),
        "host": cfg.host,
        "port": cfg.port,
        # Build type and profile come from device identity, independently of
        # lock-hold sampling (AR-3).
        "info": cfg.info,
        "conditions": {
            "build_type": cfg.info.get("build_type"),
            "profile": cfg.info.get("profile"),
            "board": cfg.info.get("board"),
        },
        "results": results,
        "rf_health": health,
        "history_comparison": comparison,
        # Post-suite heap and pool peaks check the reference budgets against
        # load.
        "lwip_stats_end": _safe_lwip(cfg),
    }
    with out_path.open("w") as fh:
        json.dump(document, fh, indent=2)
    print(f"Results written to {out_path}")

    # Functional failures, internal timing and case errors decide exit status.
    # Network and history figures are review evidence.
    failures = sorted(
        key for key, data in results.items()
        if "error" in data or data.get("pass") is False)
    if failures:
        print(f"FAILED: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
