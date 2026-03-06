#!/usr/bin/env python3
"""
Test script for heap profile sampling.

Run with:
    PYTHON_HEAP_PROFILE_SAMPLE=10 PYTHON_HEAP_PROFILE_PRINT=1 ./python.exe test_heap_profile.py

Or run the built-in verification (spawns subprocess):
    ./python.exe test_heap_profile.py --check

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
        env["PYTHON_HEAP_PROFILE_SAMPLE"] = "10"  # required: disabled by default
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
        # Verify our call chain appears (level3 -> level2 -> level1)
        if "level3" not in result2.stderr or "level2" not in result2.stderr:
            print("Test failed: traceback missing expected call chain (level3/level2)")
            return 1

        print("OK: heap profiling produced expected output")
        return 0

    if "PYTHON_HEAP_PROFILE_SAMPLE" not in os.environ or "PYTHON_HEAP_PROFILE_PRINT" not in os.environ:
        print("Run with: PYTHON_HEAP_PROFILE_SAMPLE=10 PYTHON_HEAP_PROFILE_PRINT=1 ./python.exe test_heap_profile.py")
        print("Or: ./python.exe test_heap_profile.py --check")
        return 1

    if "--traceback" in sys.argv:
        run_with_traceback_test()
    else:
        run_with_profiling()
    return 0


if __name__ == "__main__":
    sys.exit(main())
