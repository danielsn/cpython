#!/usr/bin/env python3
"""
Test script for heap profile sampling.

Run with:
    PYTHON_HEAP_PROFILE_SAMPLE=10 PYTHON_HEAP_PROFILE_PRINT=1 ./python.exe test_heap_profile.py

Or run the built-in verification (spawns subprocess):
    ./python.exe test_heap_profile.py --check

This exercises the heap profiler by allocating and freeing many small objects.
The profiler tracks 1 in N allocations (default 10) via a linked list in each
pool's metadata. When tracked objects are freed, they are printed to stderr.
"""
import sys
import subprocess
import gc
import os


def run_with_profiling():
    """Allocate and free many small objects to exercise heap profiling."""
    for i in range(200):
        x = [1, 2, 3, 4, 5]
        y = (10, 20, 30)
        z = {"a": 1, "b": 2}
        del x, y, z

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
        print("OK: heap profiling produced expected output")
        return 0

    if "PYTHON_HEAP_PROFILE_SAMPLE" not in os.environ or "PYTHON_HEAP_PROFILE_PRINT" not in os.environ:
        print("Run with: PYTHON_HEAP_PROFILE_SAMPLE=10 PYTHON_HEAP_PROFILE_PRINT=1 ./python.exe test_heap_profile.py")
        print("Or: ./python.exe test_heap_profile.py --check")
        return 1

    run_with_profiling()
    return 0


if __name__ == "__main__":
    sys.exit(main())
