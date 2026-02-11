"""Performance benchmarks for the Godot MCP server.

All tests are marked @pytest.mark.p2 and @pytest.mark.slow.
Run with: pytest test_performance.py -m "p2 and slow" -v -s
"""

import time

import pytest

from mcp_test_client import MCPClient


def _percentile(sorted_values: list[float], p: float) -> float:
    """Return the p-th percentile from a pre-sorted list (0 <= p <= 100)."""
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1
    if c >= len(sorted_values):
        return sorted_values[f]
    d = k - f
    return sorted_values[f] + d * (sorted_values[c] - sorted_values[f])


# ---------------------------------------------------------------------------
# PERF-01: Ping Latency
# ---------------------------------------------------------------------------

class TestPingLatency:
    """PERF-01 -- Measure round-trip ping latency over 1000 iterations."""

    @pytest.mark.p2
    @pytest.mark.slow
    def test_ping_latency(self, module_client: MCPClient):
        """PERF-01: Send 1000 pings, assert p50 < 5ms and p99 < 20ms."""
        iterations = 1000
        latencies_ns: list[int] = []

        for _ in range(iterations):
            start = time.perf_counter_ns()
            resp = module_client.ping()
            end = time.perf_counter_ns()
            assert "result" in resp
            latencies_ns.append(end - start)

        latencies_ms = sorted(ns / 1_000_000 for ns in latencies_ns)

        p50 = _percentile(latencies_ms, 50)
        p95 = _percentile(latencies_ms, 95)
        p99 = _percentile(latencies_ms, 99)
        minimum = latencies_ms[0]
        maximum = latencies_ms[-1]

        print(f"\n  PERF-01: Ping latency ({iterations} iterations)")
        print(f"    min:  {minimum:.2f} ms")
        print(f"    p50:  {p50:.2f} ms")
        print(f"    p95:  {p95:.2f} ms")
        print(f"    p99:  {p99:.2f} ms")
        print(f"    max:  {maximum:.2f} ms")

        assert p50 < 5.0, f"p50 ping latency {p50:.2f}ms exceeds 5ms threshold"
        assert p99 < 20.0, f"p99 ping latency {p99:.2f}ms exceeds 20ms threshold"


# ---------------------------------------------------------------------------
# PERF-02: tools/list Latency
# ---------------------------------------------------------------------------

class TestToolsListLatency:
    """PERF-02 -- Measure tools/list round-trip latency."""

    @pytest.mark.p2
    @pytest.mark.slow
    def test_tools_list_latency(self, module_client: MCPClient):
        """PERF-02: Send 100 tools/list calls, assert p50 < 10ms."""
        iterations = 100
        latencies_ns: list[int] = []

        for _ in range(iterations):
            start = time.perf_counter_ns()
            resp = module_client.list_tools()
            end = time.perf_counter_ns()
            assert "result" in resp
            latencies_ns.append(end - start)

        latencies_ms = sorted(ns / 1_000_000 for ns in latencies_ns)

        p50 = _percentile(latencies_ms, 50)
        p95 = _percentile(latencies_ms, 95)
        p99 = _percentile(latencies_ms, 99)
        minimum = latencies_ms[0]
        maximum = latencies_ms[-1]

        print(f"\n  PERF-02: tools/list latency ({iterations} iterations)")
        print(f"    min:  {minimum:.2f} ms")
        print(f"    p50:  {p50:.2f} ms")
        print(f"    p95:  {p95:.2f} ms")
        print(f"    p99:  {p99:.2f} ms")
        print(f"    max:  {maximum:.2f} ms")

        assert p50 < 10.0, f"p50 tools/list latency {p50:.2f}ms exceeds 10ms threshold"


# ---------------------------------------------------------------------------
# PERF-03: testing/check_script Latency
# ---------------------------------------------------------------------------

class TestCheckErrorsLatency:
    """PERF-03 -- Measure testing/check_script latency on a valid script."""

    @pytest.mark.p2
    @pytest.mark.slow
    def test_check_errors_latency(self, module_client: MCPClient):
        """PERF-03: Call testing/check_script on valid.gd 100 times, assert p50 < 50ms."""
        iterations = 100
        latencies_ns: list[int] = []

        for _ in range(iterations):
            start = time.perf_counter_ns()
            resp = module_client.call_tool(
                "testing/check_script",
                {"path": "res://scripts/valid.gd"},
            )
            end = time.perf_counter_ns()
            assert "result" in resp
            latencies_ns.append(end - start)

        latencies_ms = sorted(ns / 1_000_000 for ns in latencies_ns)

        p50 = _percentile(latencies_ms, 50)
        p95 = _percentile(latencies_ms, 95)
        p99 = _percentile(latencies_ms, 99)
        minimum = latencies_ms[0]
        maximum = latencies_ms[-1]

        print(f"\n  PERF-03: testing/check_script latency ({iterations} iterations)")
        print(f"    min:  {minimum:.2f} ms")
        print(f"    p50:  {p50:.2f} ms")
        print(f"    p95:  {p95:.2f} ms")
        print(f"    p99:  {p99:.2f} ms")
        print(f"    max:  {maximum:.2f} ms")

        assert p50 < 50.0, f"p50 check_script latency {p50:.2f}ms exceeds 50ms threshold"


# ---------------------------------------------------------------------------
# PERF-04: Throughput (pings/sec)
# ---------------------------------------------------------------------------

class TestPingThroughput:
    """PERF-04 -- Measure sustained ping throughput over 5 seconds."""

    @pytest.mark.p2
    @pytest.mark.slow
    def test_ping_throughput(self, module_client: MCPClient):
        """PERF-04: Tight-loop pings for 5 seconds, assert > 200 pings/sec."""
        duration_sec = 5.0
        count = 0

        deadline = time.perf_counter_ns() + int(duration_sec * 1_000_000_000)
        start = time.perf_counter_ns()

        while time.perf_counter_ns() < deadline:
            resp = module_client.ping()
            assert "result" in resp
            count += 1

        elapsed_ns = time.perf_counter_ns() - start
        elapsed_sec = elapsed_ns / 1_000_000_000
        throughput = count / elapsed_sec

        print(f"\n  PERF-04: Ping throughput")
        print(f"    duration: {elapsed_sec:.2f} s")
        print(f"    count:    {count}")
        print(f"    rate:     {throughput:.1f} pings/sec")

        assert throughput > 200.0, (
            f"Ping throughput {throughput:.1f}/sec is below 200/sec threshold"
        )


# ---------------------------------------------------------------------------
# PERF-05: Memory Growth (simplified)
# ---------------------------------------------------------------------------

class TestMemoryGrowth:
    """PERF-05 -- Long-running soak test for memory growth."""

    @pytest.mark.p2
    @pytest.mark.slow
    @pytest.mark.skip(reason="Long-running soak test")
    def test_memory_growth(self, module_client: MCPClient):
        """PERF-05: Soak test -- send requests for extended period and check memory.

        This test is skipped by default because it requires a long run time
        and external memory monitoring.

        To run manually:
            1. Start the Godot editor with the test project.
            2. Record initial RSS of the Godot process:
                   ps -o rss= -p $(pidof godot)
            3. Run a sustained workload for 5+ minutes:
                   pytest test_performance.py::TestMemoryGrowth -k memory --no-header -rN -s
            4. Record final RSS and compare.
            5. Acceptable: < 10 MB growth over 10,000 request cycles.

        A future version of this test could automate RSS sampling via
        /proc/<pid>/status on Linux.
        """
        pass


# ---------------------------------------------------------------------------
# PERF-06: Editor FPS Impact
# ---------------------------------------------------------------------------

class TestEditorFPSImpact:
    """PERF-06 -- Measure editor FPS impact under MCP load."""

    @pytest.mark.p2
    @pytest.mark.slow
    @pytest.mark.skip(reason="Requires manual FPS measurement")
    def test_editor_fps_impact(self, module_client: MCPClient):
        """PERF-06: Verify MCP server activity does not degrade editor FPS.

        This test is skipped by default because automated FPS measurement
        requires access to the editor's rendering metrics, which are not
        exposed over the MCP protocol.

        To run manually:
            1. Start the Godot editor with the test project.
            2. Enable the FPS counter in Editor -> Editor Settings -> Debug.
            3. Record baseline FPS with no MCP activity for 30 seconds.
            4. Run a sustained MCP workload (e.g., PERF-04 throughput test).
            5. Record FPS under load for 30 seconds.
            6. Acceptable: < 5% FPS drop compared to baseline.

        A future version could use editor/get_performance_monitors if that
        tool is added to the MCP server.
        """
        pass
