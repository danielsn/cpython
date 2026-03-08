#!/usr/bin/env python3
"""
Test script for heap profile sampling.

Run with:
    PYTHON_HEAP_PROFILE_SAMPLE_BYTES=5000 ./python test_heap_profile.py

  PYTHON_HEAP_PROFILE_SAMPLE_BYTES=N: ~1 sample per N bytes allocated (byte-weighted Poisson).

Or run the built-in verification (spawns subprocess):
    PYTHON_HEAP_PROFILE_SAMPLE_BYTES=5000 ./python test_heap_profile.py --check

Or validate pprof/OTel protobuf export:
    ./python test_heap_profile.py --check-export

This exercises the heap profiler by allocating and freeing small and large
objects. Small allocations use pool metadata; large allocations use an extra
pointer before the object.
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
        env = os.environ.copy()
        env["PYTHON_HEAP_PROFILE_SAMPLE_BYTES"] = "5000"  # ~1 sample per 5KB
        check_code = """
import gc
import _testinternalcapi
PYMALLOC_THRESHOLD = 512

# Run profiling allocations (small + large)
for i in range(200):
    x = [1, 2, 3, 4, 5]
    y = (10, 20, 30)
    z = {"a": 1, "b": 2}
    del x, y, z
for i in range(50):
    large = bytearray(PYMALLOC_THRESHOLD + 1)
    del large
gc.collect()

# Verify via API: samples exist and include large allocations
samples = _testinternalcapi.heap_profile_iterate_allocation_samples()
has_large = False
for ptr, size, bytes_w, allocs_w, tb in samples:
    if size > PYMALLOC_THRESHOLD:
        has_large = True
        break
if not has_large:
    print("TEST_FAIL: no large allocation")
    exit(1)

# Traceback test: allocations from Python code should have tracebacks
def level3():
    return [1, 2, 3] * 100
def level2():
    return level3()
def level1():
    return level2()
live = []
for _ in range(30):
    live.append(level1())
samples = _testinternalcapi.heap_profile_iterate_allocation_samples()
expected = ["level3", "level2", "level1"]
alloc_ok = False
for ptr, size, bytes_w, allocs_w, tb in samples:
    if tb is not None and len(tb) >= 3:
        names = [f[2] for f in tb]
        if all(want in names for want in expected):
            alloc_ok = True
            break
if not alloc_ok:
    print("TEST_FAIL: traceback missing level3/level2/level1")
    exit(1)
print("OK")
"""
        result = subprocess.run(
            [sys.executable, "-c", check_code],
            env=env,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"Test failed: exit code {result.returncode}")
            print("stderr:", result.stderr)
            return 1
        if "OK" not in result.stdout:
            print("Test failed: expected OK in output")
            print("stdout:", result.stdout)
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

    if "PYTHON_HEAP_PROFILE_SAMPLE_BYTES" not in os.environ:
        print("Run with: PYTHON_HEAP_PROFILE_SAMPLE_BYTES=5000 ./python test_heap_profile.py")
        print("  (PYTHON_HEAP_PROFILE_SAMPLE_BYTES=N means ~1 sample per N bytes allocated)")
        print("Or: PYTHON_HEAP_PROFILE_SAMPLE_BYTES=5000 ./python test_heap_profile.py --check")
        print("Or: ./python test_heap_profile.py --check-export  # validate pprof/OTel protobuf")
        return 1

    if "--traceback" in sys.argv:
        run_with_traceback_test()
    else:
        run_with_profiling()
    return 0


if __name__ == "__main__":
    sys.exit(main())
