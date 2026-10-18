#!/usr/bin/env python3
import http.client
import json
import socket
import statistics
import subprocess
import sys
import time


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: perf_smoke_http.py /path/to/cppmcp_http_server")

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    server = subprocess.Popen(
        [sys.argv[1], "--port", str(port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    latencies = []
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                if server.poll() is not None:
                    raise RuntimeError(f"HTTP server exited with status {server.returncode}")
                time.sleep(0.05)
        else:
            raise RuntimeError("HTTP server did not start within 10 seconds")

        body = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {
                    "name": "add",
                    "arguments": {"a": 1, "b": 2},
                    "_meta": {
                        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                        "io.modelcontextprotocol/clientInfo": {
                            "name": "perf-smoke",
                            "version": "1",
                        },
                        "io.modelcontextprotocol/clientCapabilities": {},
                    },
                },
            }
        )
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": "2026-07-28",
            "Mcp-Method": "tools/call",
            "Mcp-Name": "add",
        }

        for request_number in range(101):
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            started = time.perf_counter()
            connection.request("POST", "/mcp", body, headers)
            response = connection.getresponse()
            payload = json.loads(response.read())
            elapsed = time.perf_counter() - started
            connection.close()
            if response.status != 200 or "result" not in payload:
                raise RuntimeError(
                    f"request {request_number} failed: HTTP {response.status}: {payload}"
                )
            if request_number:
                latencies.append(elapsed * 1000)

        total_seconds = sum(latencies) / 1000
        print(
            "perf smoke: "
            f"requests={len(latencies)} "
            f"throughput={len(latencies) / total_seconds:.1f} req/s "
            f"median={statistics.median(latencies):.2f} ms "
            f"p95={sorted(latencies)[int(len(latencies) * 0.95) - 1]:.2f} ms"
        )
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


if __name__ == "__main__":
    main()
