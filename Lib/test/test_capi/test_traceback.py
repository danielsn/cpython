"""Tests for PyUnstable_CollectTraceback and PyUnstable_PrintTraceback."""

import os
import sys
import unittest
from test.support import import_helper

_testcapi = import_helper.import_module('_testcapi')


def _read_pipe(fd):
    """Read all available bytes from a pipe file descriptor."""
    chunks = []
    while True:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        chunks.append(chunk)
    return b''.join(chunks).decode()


# Path to this source file as stored in code objects (.pyc -> .py).
_THIS_FILE = __file__.removesuffix('c')


class TestCollectTraceback(unittest.TestCase):

    def test_returns_list(self):
        frames = _testcapi.collect_traceback()
        self.assertIsInstance(frames, list)
        self.assertGreater(len(frames), 0)

    def test_frame_tuple_structure(self):
        # Each element is (filename: str, lineno: int | None, name: str).
        frames = _testcapi.collect_traceback()
        for filename, lineno, name in frames:
            self.assertIsInstance(filename, str)
            self.assertTrue(lineno is None or isinstance(lineno, int))
            self.assertIsInstance(name, str)

    def test_innermost_frame_name_and_caller(self):
        # frames[0] is the direct Python caller; frames[1] is its caller.
        def inner():
            return _testcapi.collect_traceback()

        frames = inner()
        self.assertEqual(frames[0][2], 'inner')
        self.assertEqual(frames[1][2], 'test_innermost_frame_name_and_caller')

    def test_call_stack_order(self):
        # Frames are most-recent-first.
        def level2():
            return _testcapi.collect_traceback()

        def level1():
            return level2()

        frames = level1()
        names = [f[2] for f in frames]
        self.assertEqual(names[0], 'level2')
        self.assertEqual(names[1], 'level1')
        self.assertEqual(names[2], 'test_call_stack_order')

    def test_filename_and_lineno_accuracy(self):
        # The innermost frame should reference this file at the call site line.
        def inner():
            call_line = sys._getframe().f_lineno + 1
            frames = _testcapi.collect_traceback()
            return call_line, frames

        call_line, frames = inner()
        filename0, lineno0, name0 = frames[0]
        self.assertEqual(name0, 'inner')
        self.assertEqual(filename0, _THIS_FILE)
        self.assertEqual(lineno0, call_line)

        filename1, lineno1, name1 = frames[1]
        self.assertEqual(name1, 'test_filename_and_lineno_accuracy')
        self.assertEqual(filename1, _THIS_FILE)

    def test_max_frames_limits_collection(self):
        def level2():
            def level1():
                return _testcapi.collect_traceback(2)
            return level1()

        frames = level2()
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0][2], 'level1')
        self.assertEqual(frames[1][2], 'level2')

    def test_frameinfo_strsize_constant(self):
        # 500 content bytes + "..." (3) + '\0' (1) = 504.
        self.assertEqual(_testcapi.FRAMEINFO_STRSIZE, 504)


class TestPrintTraceback(unittest.TestCase):

    def _print(self, frames, write_header=True):
        r, w = os.pipe()
        try:
            _testcapi.print_traceback(w, frames, write_header)
            os.close(w)
            w = -1
            return _read_pipe(r)
        finally:
            os.close(r)
            if w >= 0:
                os.close(w)

    def test_header_present(self):
        out = self._print([('/a.py', 1, 'f')], write_header=True)
        self.assertTrue(out.startswith('Stack (most recent call first):\n'))

    def test_header_absent(self):
        out = self._print([('/a.py', 1, 'f')], write_header=False)
        self.assertNotIn('Stack', out)

    def test_frame_format(self):
        out = self._print([('/some/module.py', 42, 'myfunc')], write_header=False)
        self.assertEqual(out, '  File "/some/module.py", line 42 in myfunc\n')

    def test_multiple_frames(self):
        frames = [('/a.py', 10, 'inner'), ('/b.py', 20, 'outer')]
        out = self._print(frames, write_header=False)
        lines = out.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertIn('inner', lines[0])
        self.assertIn('outer', lines[1])

    def test_unknown_filename_prints_question_marks(self):
        out = self._print([('', 1, 'f')], write_header=False)
        self.assertIn('???', out)
        self.assertNotIn('""', out)

    def test_unknown_name_prints_question_marks(self):
        out = self._print([('/a.py', 1, '')], write_header=False)
        self.assertIn('???', out)

    def test_unknown_lineno_prints_question_marks(self):
        out = self._print([('/a.py', -1, 'f')], write_header=False)
        self.assertIn('???', out)
        self.assertNotIn('line -1', out)

    def test_empty_frame_list(self):
        out = self._print([], write_header=True)
        self.assertEqual(out, 'Stack (most recent call first):\n')


class TestEndToEnd(unittest.TestCase):

    def test_output_contains_caller(self):
        def inner():
            r, w = os.pipe()
            try:
                _testcapi.collect_and_print_traceback(w)
                os.close(w)
                w = -1
                return _read_pipe(r)
            finally:
                os.close(r)
                if w >= 0:
                    os.close(w)

        output = inner()
        self.assertIn('Stack (most recent call first):', output)
        self.assertIn('inner', output)
        self.assertIn('test_output_contains_caller', output)

    def test_output_filename_and_lineno(self):
        # The innermost frame should reference this file and the correct line.
        def inner():
            r, w = os.pipe()
            try:
                call_line = sys._getframe().f_lineno + 1
                _testcapi.collect_and_print_traceback(w)
                os.close(w)
                w = -1
                return call_line, _read_pipe(r)
            finally:
                os.close(r)
                if w >= 0:
                    os.close(w)

        call_line, output = inner()
        file_lines = [l for l in output.splitlines() if l.startswith('  File')]
        self.assertTrue(file_lines)
        first = file_lines[0]
        self.assertIn(os.path.basename(_THIS_FILE), first)
        self.assertIn(f'line {call_line}', first)
        self.assertIn('inner', first)

    def test_max_frames_limits_output(self):
        def level2():
            def level1():
                r, w = os.pipe()
                try:
                    _testcapi.collect_and_print_traceback(w, 1)
                    os.close(w)
                    w = -1
                    return _read_pipe(r)
                finally:
                    os.close(r)
                    if w >= 0:
                        os.close(w)
            return level1()

        output = level2()
        file_lines = [l for l in output.splitlines() if l.startswith('  File')]
        self.assertEqual(len(file_lines), 1)
        self.assertIn('level1', file_lines[0])


if __name__ == '__main__':
    unittest.main()
