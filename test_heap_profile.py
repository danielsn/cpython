#!/usr/bin/env python3
"""
Test script for heap profile sampling.

Run with:
    PYTHON_HEAP_PROFILE_SAMPLE_BYTES=5000 PYTHON_HEAP_PROFILE_PRINT=1 ./python test_heap_profile.py

  PYTHON_HEAP_PROFILE_SAMPLE_BYTES=N: ~1 sample per N bytes allocated (byte-weighted Poisson).
  PYTHON_HEAP_PROFILE_DEBUG=1: also print native C stacks when no Python traceback.

Or run the built-in verification (spawns subprocess):
    ./python test_heap_profile.py --check

Or validate pprof/OTel protobuf export:
    ./python test_heap_profile.py --check-export

This exercises the heap profiler by allocating and freeing small and large
objects. Small allocations use pool metadata; large allocations use an extra
pointer before the object. When tracked objects are freed, they are printed.
"""
import sys
import subprocess
import gc
import os


# pymalloc threshold: allocations larger than this use the system allocator
# and are not tracked by the heap profiler (see SMALL_REQUEST_THRESHOLD in obmalloc.c)
PYMALLOC_THRESHOLD = 512


def run_with_profiling():
    """Allocate and free many small objects to exercise heap profiling."""
    for i in range(200):
        x = [1, 2, 3, 4, 5]
        y = (10, 20, 30)
        z = {"a": 1, "b": 2}
        del x, y, z

    # Allocate and free large objects (> pymalloc threshold) - tracked via
    # metadata prefix before the object
    for i in range(50):
        large = bytearray(PYMALLOC_THRESHOLD + 1)
        del large

    gc.collect()
    return 0


def run_with_traceback_test():
    """Allocate from a call chain so tracebacks show multiple frames."""

    def level3():
        return [1, 2, 3] * 100  # Allocation happens here

    def level2():
        return level3()

    def level1():
        return level2()

    for _ in range(30):
        obj = level1()
        del obj

    gc.collect()
    return 0


def main():
    if "--check" in sys.argv:
        # Run self in subprocess with profiling, verify we get output
        env = os.environ.copy()
        env["PYTHON_HEAP_PROFILE_SAMPLE_BYTES"] = "5000"  # ~1 sample per 5KB
        env["PYTHON_HEAP_PROFILE_PRINT"] = "1"
        result = subprocess.run(
            [sys.executable, __file__],
            env=env,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"Test failed: exit code {result.returncode}")
            return 1
        if "heap profile free:" not in result.stderr:
            print("Test failed: no 'heap profile free:' lines in stderr")
            print("stderr sample:", result.stderr[:500])
            return 1

        # Verify large allocations are tracked (size > 512)
        import re
        has_large = False
        for line in result.stderr.splitlines():
            if "heap profile free:" in line:
                m = re.search(r"size=(\d+)", line)
                if m:
                    size = int(m.group(1))
                    if size > PYMALLOC_THRESHOLD:
                        has_large = True
        if not has_large:
            print("Test failed: no large allocation tracking (size > 512)")
            return 1

        # Run traceback test: allocations from Python code should have tracebacks
        result2 = subprocess.run(
            [sys.executable, "-c",
             "from test_heap_profile import run_with_traceback_test; "
             "run_with_traceback_test()"],
            env=env,
            capture_output=True,
            text=True,
        )
        if result2.returncode != 0:
            print(f"Traceback test failed: exit code {result2.returncode}")
            return 1
        if "Allocation traceback" not in result2.stderr:
            print("Test failed: no 'Allocation traceback' in output")
            return 1
        if 'File "' not in result2.stderr or " in " not in result2.stderr:
            print("Test failed: traceback missing file/line info")
            return 1
        # Verify expected frames occur in correct order (most recent first: level3, level2, level1)
        # Format: "  Allocation traceback (most recent first):\n    File \"...\", line N in level3\n ..."
        expected = [" in level3", " in level2", " in level1"]
        for block in result2.stderr.split("Allocation traceback"):
            if " in level3" not in block:
                continue
            # Check all expected frames appear in this block in order
            pos = 0
            for want in expected:
                idx = block.find(want, pos)
                if idx < 0:
                    break
                pos = idx + len(want)
            else:
                # All found in order
                break
        else:
            print("Test failed: traceback missing expected call chain (level3 -> level2 -> level1)")
            print("stderr sample:", result2.stderr[-1500:] if len(result2.stderr) > 1500 else result2.stderr)
            return 1

        print("OK: heap profiling produced expected output")
        return 0

    if "--check-export" in sys.argv:
        # Validate pprof/OTel protobuf export: run with profiling, export, verify wire format
        # and that stack traces contain expected frame names (level3, level2, level1)
        env = os.environ.copy()
        env["PYTHON_HEAP_PROFILE_SAMPLE_BYTES"] = "5000"

        def read_varint(buf, pos):
            val, shift = 0, 0
            while pos < len(buf):
                b = buf[pos]
                pos += 1
                val |= (b & 0x7F) << shift
                if not (b & 0x80):
                    return val, pos
                shift += 7
                if shift >= 64:
                    return None, None
            return None, None

        def extract_string_table_strings(data, want_field):
            """Extract repeated string values for field want_field from protobuf.
            Recurses into length-delimited submessages to reach nested Profile.
            pprof Profile: string_table=6. OTel: Profile.string_table=13, nested in Request(1)->ResourceProfiles(2)->ScopeProfiles(2)."""
            strings = []
            i = 0
            while i < len(data):
                tag, i = read_varint(data, i)
                if i is None:
                    break
                field, wire = tag >> 3, tag & 7
                if field == want_field and wire == 2:
                    length, i = read_varint(data, i)
                    if i is None or length is None or i + length > len(data):
                        break
                    try:
                        s = data[i:i + length].decode('utf-8')
                        strings.append(s)
                    except UnicodeDecodeError:
                        pass
                    i += length
                elif wire == 0:
                    _, i = read_varint(data, i)
                    if i is None:
                        break
                elif wire == 1:
                    if i + 8 > len(data):
                        break
                    i += 8
                elif wire == 2:
                    length, i = read_varint(data, i)
                    if i is None or length is None or i + length > len(data):
                        break
                    strings.extend(extract_string_table_strings(data[i:i + length], want_field))
                    i += length
                elif wire == 5:
                    if i + 4 > len(data):
                        break
                    i += 4
            return strings

        def validate_proto(data, name):
            if len(data) == 0:
                return f"{name} export empty"
            tag, i = read_varint(data, 0)
            if i is None:
                return f"{name} too short"
            field, wire = tag >> 3, tag & 7
            if not (1 <= field < 256 and wire <= 5):
                return f"{name} invalid tag field={field} wire={wire}"
            return None

        def verify_stacktrace_strings(strings, name):
            expected = ["level3", "level2", "level1"]
            for want in expected:
                if want not in strings:
                    return f"{name} string_table missing expected frame '{want}'"
            return None

        # Code that runs traceback test (level1->level2->level3) then exports.
        # Keep allocations alive until after export (heap profile tracks live allocations).
        # Also verify allocation profile (iterate_allocation_samples) has expected frames.
        traceback_export_code = """
def level3():
    return [1, 2, 3] * 100
def level2():
    return level3()
def level1():
    return level2()
import _testinternalcapi
live = []
for _ in range(30):
    live.append(level1())
pprof = _testinternalcapi.heap_profile_export_pprof_bytes()
otel = _testinternalcapi.heap_profile_export_otel_bytes()
print(pprof.hex())
print("---")
print(otel.hex())
print("---")
# Verify allocation profile has expected stack trace
samples = _testinternalcapi.heap_profile_iterate_allocation_samples()
expected = ["level3", "level2", "level1"]
alloc_ok = False
for ptr, size, bytes_w, allocs_w, tb in samples:
    if tb is not None and len(tb) >= 3:
        names = [f[2] for f in tb]
        if all(want in names for want in expected):
            alloc_ok = True
            break
print("ALLOC_OK" if alloc_ok else "ALLOC_FAIL")
"""

        # Run traceback test + export in one subprocess
        result = subprocess.run(
            [sys.executable, "-c", traceback_export_code],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if result.returncode != 0:
            print(f"Export test failed: exit {result.returncode}")
            print("stderr:", result.stderr)
            return 1

        parts = result.stdout.strip().split("\n---\n")
        if len(parts) != 3:
            print("Export test failed: expected pprof, OTel, and allocation output")
            return 1
        pprof = bytes.fromhex(parts[0].replace("\n", ""))
        otel = bytes.fromhex(parts[1].replace("\n", ""))
        alloc_result = parts[2].strip()

        err = validate_proto(pprof, "pprof")
        if err:
            print(f"Export test failed: {err}")
            return 1
        err = validate_proto(otel, "OTel")
        if err:
            print(f"Export test failed: {err}")
            return 1
        print("OK: pprof and OTel export valid protobuf")

        # Verify stack traces in string tables (pprof field 6, OTel field 13)
        pprof_strings = extract_string_table_strings(pprof, 6)
        err = verify_stacktrace_strings(pprof_strings, "pprof")
        if err:
            print(f"Export test failed: {err}")
            return 1
        print("OK: pprof string_table contains expected stack trace (level3, level2, level1)")

        otel_strings = extract_string_table_strings(otel, 13)
        err = verify_stacktrace_strings(otel_strings, "OTel")
        if err:
            print(f"Export test failed: {err}")
            return 1
        print("OK: OTel string_table contains expected stack trace (level3, level2, level1)")

        if alloc_result != "ALLOC_OK":
            print("Export test failed: allocation profile missing expected frames (level3, level2, level1)")
            return 1
        print("OK: allocation profile contains expected stack trace (level3, level2, level1)")
        return 0

    if "PYTHON_HEAP_PROFILE_SAMPLE_BYTES" not in os.environ or "PYTHON_HEAP_PROFILE_PRINT" not in os.environ:
        print("Run with: PYTHON_HEAP_PROFILE_SAMPLE_BYTES=5000 PYTHON_HEAP_PROFILE_PRINT=1 ./python test_heap_profile.py")
        print("  (PYTHON_HEAP_PROFILE_SAMPLE_BYTES=N means ~1 sample per N bytes allocated)")
        print("Or: ./python test_heap_profile.py --check")
        print("Or: ./python test_heap_profile.py --check-export  # validate pprof/OTel protobuf")
        return 1

    if "--traceback" in sys.argv:
        run_with_traceback_test()
    else:
        run_with_profiling()
    return 0


if __name__ == "__main__":
    sys.exit(main())
