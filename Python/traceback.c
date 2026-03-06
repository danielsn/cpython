
/* Traceback implementation */

#include "Python.h"
#include "pycore_call.h"          // _PyObject_CallMethodFormat()
#include "pycore_fileutils.h"     // _Py_BEGIN_SUPPRESS_IPH
#include "pycore_frame.h"         // PyFrameObject
#include "pycore_interp.h"        // PyInterpreterState.gc
#include "pycore_interpframe.h"   // _PyFrame_GetCode()
#include "pycore_pyerrors.h"      // _PyErr_GetRaisedException()
#include "pycore_pystate.h"       // _PyThreadState_GET()
#include "pycore_traceback.h"     // EXCEPTION_TB_HEADER
#include "pycore_hashtable.h"     // _Py_hashtable_*

#include "frameobject.h"          // PyFrame_New()

#include "osdefs.h"               // SEP
#ifdef HAVE_UNISTD_H
#  include <unistd.h>             // lseek()
#endif

#if (defined(HAVE_EXECINFO_H) && defined(HAVE_DLFCN_H) && defined(HAVE_LINK_H))
#  define _PY_HAS_BACKTRACE_HEADERS 1
#endif

#if (defined(__APPLE__) && defined(HAVE_EXECINFO_H) && defined(HAVE_DLFCN_H))
#  define _PY_HAS_BACKTRACE_HEADERS 1
#endif

#ifdef _PY_HAS_BACKTRACE_HEADERS
#  include <execinfo.h>           // backtrace(), backtrace_symbols()
#  include <dlfcn.h>              // dladdr1()
#ifdef HAVE_LINK_H
#    include <link.h>               // struct DL_info
#endif
#  if defined(__APPLE__) && defined(HAVE_BACKTRACE) && defined(HAVE_DLADDR)
#    define CAN_C_BACKTRACE
#  elif defined(HAVE_BACKTRACE) && defined(HAVE_DLADDR1)
#    define CAN_C_BACKTRACE
#  endif
#endif

#if defined(__STDC_NO_VLA__) && (__STDC_NO_VLA__ == 1)
/* Use alloca() for VLAs. */
#  define VLA(type, name, size) type *name = alloca(size)
#elif !defined(__STDC_NO_VLA__) || (__STDC_NO_VLA__ == 0)
/* Use actual C VLAs.*/
#  define VLA(type, name, size) type name[size]
#elif defined(CAN_C_BACKTRACE)
/* VLAs are not possible. Disable C stack trace functions. */
#  undef CAN_C_BACKTRACE
#endif

#define OFF(x) offsetof(PyTracebackObject, x)
#define PUTS(fd, str) (void)_Py_write_noraise(fd, str, strlen(str))

#define MAX_STRING_LENGTH 500
#define MAX_FRAME_DEPTH 100
#define MAX_NTHREADS 100

/* Function from Parser/tokenizer/file_tokenizer.c */
extern char* _PyTokenizer_FindEncodingFilename(int, PyObject *);

/*[clinic input]
class traceback "PyTracebackObject *" "&PyTraceback_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=cf96294b2bebc811]*/

#define _PyTracebackObject_CAST(op)   ((PyTracebackObject *)(op))

#include "clinic/traceback.c.h"


#ifdef MS_WINDOWS
typedef HRESULT (WINAPI *PF_GET_THREAD_DESCRIPTION)(HANDLE, PCWSTR*);
static PF_GET_THREAD_DESCRIPTION pGetThreadDescription = NULL;
#endif


static PyObject *
tb_create_raw(PyTracebackObject *next, PyFrameObject *frame, int lasti,
              int lineno)
{
    PyTracebackObject *tb;
    if ((next != NULL && !PyTraceBack_Check(next)) ||
                    frame == NULL || !PyFrame_Check(frame)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    tb = PyObject_GC_New(PyTracebackObject, &PyTraceBack_Type);
    if (tb != NULL) {
        tb->tb_next = (PyTracebackObject*)Py_XNewRef(next);
        tb->tb_frame = (PyFrameObject*)Py_XNewRef(frame);
        tb->tb_lasti = lasti;
        tb->tb_lineno = lineno;
        PyObject_GC_Track(tb);
    }
    return (PyObject *)tb;
}

/*[clinic input]
@classmethod
traceback.__new__ as tb_new

  tb_next: object
  tb_frame: object(type='PyFrameObject *', subclass_of='&PyFrame_Type')
  tb_lasti: int
  tb_lineno: int

Create a new traceback object.
[clinic start generated code]*/

static PyObject *
tb_new_impl(PyTypeObject *type, PyObject *tb_next, PyFrameObject *tb_frame,
            int tb_lasti, int tb_lineno)
/*[clinic end generated code: output=fa077debd72d861a input=b88143145454cb59]*/
{
    if (tb_next == Py_None) {
        tb_next = NULL;
    } else if (!PyTraceBack_Check(tb_next)) {
        return PyErr_Format(PyExc_TypeError,
                            "expected traceback object or None, got '%s'",
                            Py_TYPE(tb_next)->tp_name);
    }

    return tb_create_raw((PyTracebackObject *)tb_next, tb_frame, tb_lasti,
                         tb_lineno);
}

static PyObject *
tb_dir(PyObject *Py_UNUSED(self), PyObject *Py_UNUSED(ignored))
{
    return Py_BuildValue("[ssss]", "tb_frame", "tb_next",
                                   "tb_lasti", "tb_lineno");
}

/*[clinic input]
@critical_section
@getter
traceback.tb_next
[clinic start generated code]*/

static PyObject *
traceback_tb_next_get_impl(PyTracebackObject *self)
/*[clinic end generated code: output=963634df7d5fc837 input=8f6345f2b73cb965]*/
{
    PyObject* ret = (PyObject*)self->tb_next;
    if (!ret) {
        ret = Py_None;
    }
    return Py_NewRef(ret);
}

static int
tb_get_lineno(PyObject *op)
{
    PyTracebackObject *tb = _PyTracebackObject_CAST(op);
    _PyInterpreterFrame* frame = tb->tb_frame->f_frame;
    assert(frame != NULL);
    return PyCode_Addr2Line(_PyFrame_GetCode(frame), tb->tb_lasti);
}

static PyObject *
tb_lineno_get(PyObject *op, void *Py_UNUSED(_))
{
    PyTracebackObject *self = _PyTracebackObject_CAST(op);
    int lineno = self->tb_lineno;
    if (lineno == -1) {
        lineno = tb_get_lineno(op);
        if (lineno < 0) {
            Py_RETURN_NONE;
        }
    }
    return PyLong_FromLong(lineno);
}

/*[clinic input]
@critical_section
@setter
traceback.tb_next
[clinic start generated code]*/

static int
traceback_tb_next_set_impl(PyTracebackObject *self, PyObject *value)
/*[clinic end generated code: output=d4868cbc48f2adac input=ce66367f85e3c443]*/
{
    if (!value) {
        PyErr_Format(PyExc_TypeError, "can't delete tb_next attribute");
        return -1;
    }

    /* We accept None or a traceback object, and map None -> NULL (inverse of
       tb_next_get) */
    if (value == Py_None) {
        value = NULL;
    } else if (!PyTraceBack_Check(value)) {
        PyErr_Format(PyExc_TypeError,
                     "expected traceback object, got '%s'",
                     Py_TYPE(value)->tp_name);
        return -1;
    }

    /* Check for loops */
    PyTracebackObject *cursor = (PyTracebackObject *)value;
    Py_XINCREF(cursor);
    while (cursor) {
        if (cursor == self) {
            PyErr_Format(PyExc_ValueError, "traceback loop detected");
            Py_DECREF(cursor);
            return -1;
        }
        Py_BEGIN_CRITICAL_SECTION(cursor);
        Py_XINCREF(cursor->tb_next);
        Py_SETREF(cursor, cursor->tb_next);
        Py_END_CRITICAL_SECTION();
    }

    Py_XSETREF(self->tb_next, (PyTracebackObject *)Py_XNewRef(value));

    return 0;
}


static PyMethodDef tb_methods[] = {
   {"__dir__", tb_dir, METH_NOARGS, NULL},
   {NULL, NULL, 0, NULL},
};

static PyMemberDef tb_memberlist[] = {
    {"tb_frame",        _Py_T_OBJECT,       OFF(tb_frame),  Py_READONLY|Py_AUDIT_READ},
    {"tb_lasti",        Py_T_INT,          OFF(tb_lasti),  Py_READONLY},
    {NULL}      /* Sentinel */
};

static PyGetSetDef tb_getsetters[] = {
    TRACEBACK_TB_NEXT_GETSETDEF
    {"tb_lineno", tb_lineno_get, NULL, NULL, NULL},
    {NULL}      /* Sentinel */
};

static void
tb_dealloc(PyObject *op)
{
    PyTracebackObject *tb = _PyTracebackObject_CAST(op);
    PyObject_GC_UnTrack(tb);
    Py_XDECREF(tb->tb_next);
    Py_XDECREF(tb->tb_frame);
    PyObject_GC_Del(tb);
}

static int
tb_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyTracebackObject *tb = _PyTracebackObject_CAST(op);
    Py_VISIT(tb->tb_next);
    Py_VISIT(tb->tb_frame);
    return 0;
}

static int
tb_clear(PyObject *op)
{
    PyTracebackObject *tb = _PyTracebackObject_CAST(op);
    Py_CLEAR(tb->tb_next);
    Py_CLEAR(tb->tb_frame);
    return 0;
}

PyTypeObject PyTraceBack_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "traceback",
    sizeof(PyTracebackObject),
    0,
    tb_dealloc,         /*tp_dealloc*/
    0,                  /*tp_vectorcall_offset*/
    0,    /*tp_getattr*/
    0,                  /*tp_setattr*/
    0,                  /*tp_as_async*/
    0,                  /*tp_repr*/
    0,                  /*tp_as_number*/
    0,                  /*tp_as_sequence*/
    0,                  /*tp_as_mapping*/
    0,                  /* tp_hash */
    0,                  /* tp_call */
    0,                  /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                  /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    tb_new__doc__,                              /* tp_doc */
    tb_traverse,                                /* tp_traverse */
    tb_clear,                                   /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    tb_methods,         /* tp_methods */
    tb_memberlist,      /* tp_members */
    tb_getsetters,                              /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    tb_new,                                     /* tp_new */
};


PyObject*
_PyTraceBack_FromFrame(PyObject *tb_next, PyFrameObject *frame)
{
    assert(tb_next == NULL || PyTraceBack_Check(tb_next));
    assert(frame != NULL);
    int addr = _PyInterpreterFrame_LASTI(frame->f_frame) * sizeof(_Py_CODEUNIT);
    return tb_create_raw((PyTracebackObject *)tb_next, frame, addr, -1);
}


int
PyTraceBack_Here(PyFrameObject *frame)
{
    PyObject *exc = PyErr_GetRaisedException();
    assert(PyExceptionInstance_Check(exc));
    PyObject *tb = PyException_GetTraceback(exc);
    PyObject *newtb = _PyTraceBack_FromFrame(tb, frame);
    Py_XDECREF(tb);
    if (newtb == NULL) {
        _PyErr_ChainExceptions1(exc);
        return -1;
    }
    PyException_SetTraceback(exc, newtb);
    Py_XDECREF(newtb);
    PyErr_SetRaisedException(exc);
    return 0;
}

/* Insert a frame into the traceback for (funcname, filename, lineno). */
void _PyTraceback_Add(const char *funcname, const char *filename, int lineno)
{
    PyObject *globals;
    PyCodeObject *code;
    PyFrameObject *frame;
    PyThreadState *tstate = _PyThreadState_GET();

    /* Save and clear the current exception. Python functions must not be
       called with an exception set. Calling Python functions happens when
       the codec of the filesystem encoding is implemented in pure Python. */
    PyObject *exc = _PyErr_GetRaisedException(tstate);

    globals = PyDict_New();
    if (!globals)
        goto error;
    code = PyCode_NewEmpty(filename, funcname, lineno);
    if (!code) {
        Py_DECREF(globals);
        goto error;
    }
    frame = PyFrame_New(tstate, code, globals, NULL);
    Py_DECREF(globals);
    Py_DECREF(code);
    if (!frame)
        goto error;
    frame->f_lineno = lineno;

    _PyErr_SetRaisedException(tstate, exc);
    PyTraceBack_Here(frame);
    Py_DECREF(frame);
    return;

error:
    _PyErr_ChainExceptions1(exc);
}

static PyObject *
_Py_FindSourceFile(PyObject *filename, char* namebuf, size_t namelen, PyObject *io)
{
    Py_ssize_t i;
    PyObject *binary;
    PyObject *v;
    Py_ssize_t npath;
    size_t taillen;
    PyObject *syspath;
    PyObject *path;
    const char* tail;
    PyObject *filebytes;
    const char* filepath;
    Py_ssize_t len;
    PyObject* result;
    PyObject *open = NULL;

    filebytes = PyUnicode_EncodeFSDefault(filename);
    if (filebytes == NULL) {
        PyErr_Clear();
        return NULL;
    }
    filepath = PyBytes_AS_STRING(filebytes);

    /* Search tail of filename in sys.path before giving up */
    tail = strrchr(filepath, SEP);
    if (tail == NULL)
        tail = filepath;
    else
        tail++;
    taillen = strlen(tail);

    PyThreadState *tstate = _PyThreadState_GET();
    if (PySys_GetOptionalAttr(&_Py_ID(path), &syspath) < 0) {
        PyErr_Clear();
        goto error;
    }
    if (syspath == NULL || !PyList_Check(syspath)) {
        goto error;
    }
    npath = PyList_Size(syspath);

    open = PyObject_GetAttr(io, &_Py_ID(open));
    if (open == NULL) {
        goto error;
    }
    for (i = 0; i < npath; i++) {
        v = PyList_GetItem(syspath, i);
        if (v == NULL) {
            PyErr_Clear();
            break;
        }
        if (!PyUnicode_Check(v))
            continue;
        path = PyUnicode_EncodeFSDefault(v);
        if (path == NULL) {
            PyErr_Clear();
            continue;
        }
        len = PyBytes_GET_SIZE(path);
        if (len + 1 + (Py_ssize_t)taillen >= (Py_ssize_t)namelen - 1) {
            Py_DECREF(path);
            continue; /* Too long */
        }
        strcpy(namebuf, PyBytes_AS_STRING(path));
        Py_DECREF(path);
        if (strlen(namebuf) != (size_t)len)
            continue; /* v contains '\0' */
        if (len > 0 && namebuf[len-1] != SEP)
            namebuf[len++] = SEP;
        strcpy(namebuf+len, tail);

        binary = _PyObject_CallMethodFormat(tstate, open, "ss", namebuf, "rb");
        if (binary != NULL) {
            result = binary;
            goto finally;
        }
        PyErr_Clear();
    }
    goto error;

error:
    result = NULL;
finally:
    Py_XDECREF(open);
    Py_XDECREF(syspath);
    Py_DECREF(filebytes);
    return result;
}

/* Writes indent spaces. Returns 0 on success and non-zero on failure.
 */
int
_Py_WriteIndent(int indent, PyObject *f)
{
    char buf[11] = "          ";
    assert(strlen(buf) == 10);
    while (indent > 0) {
        if (indent < 10) {
            buf[indent] = '\0';
        }
        if (PyFile_WriteString(buf, f) < 0) {
            return -1;
        }
        indent -= 10;
    }
    return 0;
}

static int
display_source_line(PyObject *f, PyObject *filename, int lineno, int indent,
                    int *truncation, PyObject **line)
{
    int fd;
    int i;
    char *found_encoding;
    const char *encoding;
    PyObject *io;
    PyObject *binary;
    PyObject *fob = NULL;
    PyObject *lineobj = NULL;
    PyObject *res;
    char buf[MAXPATHLEN+1];
    int kind;
    const void *data;

    /* open the file */
    if (filename == NULL)
        return 0;

    /* Do not attempt to open things like <string> or <stdin> */
    assert(PyUnicode_Check(filename));
    if (PyUnicode_READ_CHAR(filename, 0) == '<') {
        Py_ssize_t len = PyUnicode_GET_LENGTH(filename);
        if (len > 0 && PyUnicode_READ_CHAR(filename, len - 1) == '>') {
            return 0;
        }
    }

    io = PyImport_ImportModule("io");
    if (io == NULL) {
        return -1;
    }

    binary = _PyObject_CallMethod(io, &_Py_ID(open), "Os", filename, "rb");
    if (binary == NULL) {
        PyErr_Clear();

        binary = _Py_FindSourceFile(filename, buf, sizeof(buf), io);
        if (binary == NULL) {
            Py_DECREF(io);
            return -1;
        }
    }

    /* use the right encoding to decode the file as unicode */
    fd = PyObject_AsFileDescriptor(binary);
    if (fd < 0) {
        Py_DECREF(io);
        Py_DECREF(binary);
        return 0;
    }
    found_encoding = _PyTokenizer_FindEncodingFilename(fd, filename);
    if (found_encoding == NULL)
        PyErr_Clear();
    encoding = (found_encoding != NULL) ? found_encoding : "utf-8";
    /* Reset position */
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        Py_DECREF(io);
        Py_DECREF(binary);
        PyMem_Free(found_encoding);
        return 0;
    }
    fob = _PyObject_CallMethod(io, &_Py_ID(TextIOWrapper),
                               "Os", binary, encoding);
    Py_DECREF(io);
    PyMem_Free(found_encoding);

    if (fob == NULL) {
        PyErr_Clear();

        res = PyObject_CallMethodNoArgs(binary, &_Py_ID(close));
        Py_DECREF(binary);
        if (res)
            Py_DECREF(res);
        else
            PyErr_Clear();
        return 0;
    }
    Py_DECREF(binary);

    /* get the line number lineno */
    for (i = 0; i < lineno; i++) {
        Py_XDECREF(lineobj);
        lineobj = PyFile_GetLine(fob, -1);
        if (!lineobj) {
            PyErr_Clear();
            break;
        }
    }
    res = PyObject_CallMethodNoArgs(fob, &_Py_ID(close));
    if (res) {
        Py_DECREF(res);
    }
    else {
        PyErr_Clear();
    }
    Py_DECREF(fob);
    if (!lineobj || !PyUnicode_Check(lineobj)) {
        Py_XDECREF(lineobj);
        return -1;
    }

    if (line) {
        *line = Py_NewRef(lineobj);
    }

    /* remove the indentation of the line */
    kind = PyUnicode_KIND(lineobj);
    data = PyUnicode_DATA(lineobj);
    for (i=0; i < PyUnicode_GET_LENGTH(lineobj); i++) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, i);
        if (ch != ' ' && ch != '\t' && ch != '\014')
            break;
    }
    if (i) {
        PyObject *truncated;
        truncated = PyUnicode_Substring(lineobj, i, PyUnicode_GET_LENGTH(lineobj));
        if (truncated) {
            Py_SETREF(lineobj, truncated);
        } else {
            PyErr_Clear();
        }
    }

    if (truncation != NULL) {
        *truncation = i - indent;
    }

    /* Write some spaces before the line */
    if (_Py_WriteIndent(indent, f) < 0) {
        goto error;
    }

    /* finally display the line */
    if (PyFile_WriteObject(lineobj, f, Py_PRINT_RAW) < 0) {
        goto error;
    }

    if (PyFile_WriteString("\n", f) < 0) {
        goto error;
    }

    Py_DECREF(lineobj);
    return 0;
error:
    Py_DECREF(lineobj);
    return -1;
}

int
_Py_DisplaySourceLine(PyObject *f, PyObject *filename, int lineno, int indent,
                      int *truncation, PyObject **line)
{
    return display_source_line(f, filename, lineno, indent, truncation, line);
}


#define IS_WHITESPACE(c) (((c) == ' ') || ((c) == '\t') || ((c) == '\f'))
#define _TRACEBACK_SOURCE_LINE_INDENT 4

static inline int
ignore_source_errors(void) {
    if (PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
            return -1;
        }
        PyErr_Clear();
    }
    return 0;
}

static int
tb_displayline(PyTracebackObject* tb, PyObject *f, PyObject *filename, int lineno,
               PyFrameObject *frame, PyObject *name)
{
    if (filename == NULL || name == NULL) {
        return -1;
    }

    PyObject *line = PyUnicode_FromFormat("  File \"%U\", line %d, in %U\n",
                                          filename, lineno, name);
    if (line == NULL) {
        return -1;
    }

    int res = PyFile_WriteObject(line, f, Py_PRINT_RAW);
    Py_DECREF(line);
    if (res < 0) {
        return -1;
    }

    int err = 0;

    int truncation = _TRACEBACK_SOURCE_LINE_INDENT;
    PyObject* source_line = NULL;
    int rc = display_source_line(
            f, filename, lineno, _TRACEBACK_SOURCE_LINE_INDENT,
            &truncation, &source_line);
    if (rc != 0 || !source_line) {
        /* ignore errors since we can't report them, can we? */
        err = ignore_source_errors();
    }
    Py_XDECREF(source_line);
    return err;
}

static const int TB_RECURSIVE_CUTOFF = 3; // Also hardcoded in traceback.py.

static int
tb_print_line_repeated(PyObject *f, long cnt)
{
    cnt -= TB_RECURSIVE_CUTOFF;
    PyObject *line = PyUnicode_FromFormat(
        (cnt > 1)
          ? "  [Previous line repeated %ld more times]\n"
          : "  [Previous line repeated %ld more time]\n",
        cnt);
    if (line == NULL) {
        return -1;
    }
    int err = PyFile_WriteObject(line, f, Py_PRINT_RAW);
    Py_DECREF(line);
    return err;
}

static int
tb_printinternal(PyTracebackObject *tb, PyObject *f, long limit)
{
    PyCodeObject *code = NULL;
    Py_ssize_t depth = 0;
    PyObject *last_file = NULL;
    int last_line = -1;
    PyObject *last_name = NULL;
    long cnt = 0;
    PyTracebackObject *tb1 = tb;
    while (tb1 != NULL) {
        depth++;
        tb1 = tb1->tb_next;
    }
    while (tb != NULL && depth > limit) {
        depth--;
        tb = tb->tb_next;
    }
    while (tb != NULL) {
        code = PyFrame_GetCode(tb->tb_frame);
        int tb_lineno = tb->tb_lineno;
        if (tb_lineno == -1) {
            tb_lineno = tb_get_lineno((PyObject *)tb);
        }
        if (last_file == NULL ||
            code->co_filename != last_file ||
            last_line == -1 || tb_lineno != last_line ||
            last_name == NULL || code->co_name != last_name) {
            if (cnt > TB_RECURSIVE_CUTOFF) {
                if (tb_print_line_repeated(f, cnt) < 0) {
                    goto error;
                }
            }
            last_file = code->co_filename;
            last_line = tb_lineno;
            last_name = code->co_name;
            cnt = 0;
        }
        cnt++;
        if (cnt <= TB_RECURSIVE_CUTOFF) {
            if (tb_displayline(tb, f, code->co_filename, tb_lineno,
                               tb->tb_frame, code->co_name) < 0) {
                goto error;
            }

            if (PyErr_CheckSignals() < 0) {
                goto error;
            }
        }
        Py_CLEAR(code);
        tb = tb->tb_next;
    }
    if (cnt > TB_RECURSIVE_CUTOFF) {
        if (tb_print_line_repeated(f, cnt) < 0) {
            goto error;
        }
    }
    return 0;
error:
    Py_XDECREF(code);
    return -1;
}

#define PyTraceBack_LIMIT 1000

int
_PyTraceBack_Print(PyObject *v, const char *header, PyObject *f)
{
    PyObject *limitv;
    long limit = PyTraceBack_LIMIT;

    if (v == NULL) {
        return 0;
    }
    if (!PyTraceBack_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (PySys_GetOptionalAttrString("tracebacklimit", &limitv) < 0) {
        return -1;
    }
    else if (limitv != NULL && PyLong_Check(limitv)) {
        int overflow;
        limit = PyLong_AsLongAndOverflow(limitv, &overflow);
        if (overflow > 0) {
            limit = LONG_MAX;
        }
        else if (limit <= 0) {
            Py_DECREF(limitv);
            return 0;
        }
    }
    Py_XDECREF(limitv);

    if (PyFile_WriteString(header, f) < 0) {
        return -1;
    }

    if (tb_printinternal((PyTracebackObject *)v, f, limit) < 0) {
        return -1;
    }

    return 0;
}

int
PyTraceBack_Print(PyObject *v, PyObject *f)
{
    const char *header = EXCEPTION_TB_HEADER;
    return _PyTraceBack_Print(v, header, f);
}

/* Format an integer in range [0; 0xffffffff] to decimal and write it
   into the file fd.

   This function is signal safe. */

void
_Py_DumpDecimal(int fd, size_t value)
{
    /* maximum number of characters required for output of %lld or %p.
       We need at most ceil(log10(256)*SIZEOF_LONG_LONG) digits,
       plus 1 for the null byte.  53/22 is an upper bound for log10(256). */
    char buffer[1 + (sizeof(size_t)*53-1) / 22 + 1];
    char *ptr, *end;

    end = &buffer[Py_ARRAY_LENGTH(buffer) - 1];
    ptr = end;
    *ptr = '\0';
    do {
        --ptr;
        assert(ptr >= buffer);
        *ptr = '0' + (value % 10);
        value /= 10;
    } while (value);

    (void)_Py_write_noraise(fd, ptr, end - ptr);
}

/* Format an integer as hexadecimal with width digits into fd file descriptor.
   The function is signal safe. */
static void
dump_hexadecimal(int fd, uintptr_t value, Py_ssize_t width, int strip_zeros)
{
    char buffer[sizeof(uintptr_t) * 2 + 1], *ptr, *end;
    Py_ssize_t size = Py_ARRAY_LENGTH(buffer) - 1;

    if (width > size)
        width = size;
    /* it's ok if width is negative */

    end = &buffer[size];
    ptr = end;
    *ptr = '\0';
    do {
        --ptr;
        assert(ptr >= buffer);
        *ptr = Py_hexdigits[value & 15];
        value >>= 4;
    } while ((end - ptr) < width || value);

    size = end - ptr;
    if (strip_zeros) {
        while (*ptr == '0' && size >= 2) {
            ptr++;
            size--;
        }
    }

    (void)_Py_write_noraise(fd, ptr, size);
}

void
_Py_DumpHexadecimal(int fd, uintptr_t value, Py_ssize_t width)
{
    dump_hexadecimal(fd, value, width, 0);
}

#ifdef CAN_C_BACKTRACE
static void
dump_pointer(int fd, void *ptr)
{
    PUTS(fd, "0x");
    dump_hexadecimal(fd, (uintptr_t)ptr, sizeof(void*), 1);
}
#endif

static void
dump_char(int fd, char ch)
{
    char buf[1] = {ch};
    (void)_Py_write_noraise(fd, buf, 1);
}

void
_Py_DumpASCII(int fd, PyObject *text)
{
    PyASCIIObject *ascii = _PyASCIIObject_CAST(text);
    Py_ssize_t i, size;
    int truncated;
    int kind;
    void *data = NULL;
    Py_UCS4 ch;

    if (!PyUnicode_Check(text))
        return;

    size = ascii->length;
    kind = ascii->state.kind;
    if (ascii->state.compact) {
        if (ascii->state.ascii)
            data = ascii + 1;
        else
            data = _PyCompactUnicodeObject_CAST(text) + 1;
    }
    else {
        data = _PyUnicodeObject_CAST(text)->data.any;
        if (data == NULL)
            return;
    }

    if (MAX_STRING_LENGTH < size) {
        size = MAX_STRING_LENGTH;
        truncated = 1;
    }
    else {
        truncated = 0;
    }

    // Is an ASCII string?
    if (ascii->state.ascii) {
        assert(kind == PyUnicode_1BYTE_KIND);
        char *str = data;

        int need_escape = 0;
        for (i=0; i < size; i++) {
            ch = str[i];
            if (!(' ' <= ch && ch <= 126)) {
                need_escape = 1;
                break;
            }
        }
        if (!need_escape) {
            // The string can be written with a single write() syscall
            (void)_Py_write_noraise(fd, str, size);
            goto done;
        }
    }

    for (i=0; i < size; i++) {
        ch = PyUnicode_READ(kind, data, i);
        if (' ' <= ch && ch <= 126) {
            /* printable ASCII character */
            dump_char(fd, (char)ch);
        }
        else if (ch <= 0xff) {
            PUTS(fd, "\\x");
            _Py_DumpHexadecimal(fd, ch, 2);
        }
        else if (ch <= 0xffff) {
            PUTS(fd, "\\u");
            _Py_DumpHexadecimal(fd, ch, 4);
        }
        else {
            PUTS(fd, "\\U");
            _Py_DumpHexadecimal(fd, ch, 8);
        }
    }

done:
    if (truncated) {
        PUTS(fd, "...");
    }
}

/* Write a Unicode object to a buffer as ASCII (backslashreplace). Signal safe.
   Returns the number of chars written (excluding null). Always null-terminates. */
static Py_ssize_t _Py_NO_SANITIZE_THREAD
dump_ascii_to_buffer(char *buf, size_t buf_size, PyObject *text)
{
    if (buf_size == 0)
        return 0;
    buf[0] = '\0';

    if (!PyUnicode_Check(text))
        goto invalid;

    PyASCIIObject *ascii = _PyASCIIObject_CAST(text);
    Py_ssize_t i, size;
    int truncated;
    int kind;
    void *data = NULL;
    Py_UCS4 ch;
    char *out = buf;
    char *end = buf + buf_size - 1;  /* leave room for null */

    size = ascii->length;
    kind = ascii->state.kind;
    if (ascii->state.compact) {
        if (ascii->state.ascii)
            data = ascii + 1;
        else
            data = _PyCompactUnicodeObject_CAST(text) + 1;
    }
    else {
        data = _PyUnicodeObject_CAST(text)->data.any;
        if (data == NULL)
            goto invalid;
    }

    if (MAX_STRING_LENGTH < size) {
        size = MAX_STRING_LENGTH;
        truncated = 1;
    }
    else {
        truncated = 0;
    }

    /* Fast path: pure ASCII, no escaping needed */
    if (ascii->state.ascii) {
        assert(kind == PyUnicode_1BYTE_KIND);
        char *str = data;
        for (i = 0; i < size && out < end; i++) {
            ch = str[i];
            if (!(' ' <= ch && ch <= 126))
                break;
            *out++ = (char)ch;
        }
        if (i == size) {
            if (truncated && out + 3 <= end) {
                *out++ = '.';
                *out++ = '.';
                *out++ = '.';
            }
            *out = '\0';
            return out - buf;
        }
        /* Need to escape; fall through to slow path from start */
        out = buf;
    }

    /* Slow path: escape non-printable chars */
    for (i = 0; i < size && out < end; i++) {
        ch = PyUnicode_READ(kind, data, i);
        if (' ' <= ch && ch <= 126) {
            *out++ = (char)ch;
        }
        else if (ch <= 0xff) {
            if (out + 4 <= end) {
                *out++ = '\\';
                *out++ = 'x';
                *out++ = Py_hexdigits[(ch >> 4) & 15];
                *out++ = Py_hexdigits[ch & 15];
            }
        }
        else if (ch <= 0xffff) {
            if (out + 6 <= end) {
                *out++ = '\\';
                *out++ = 'u';
                *out++ = Py_hexdigits[(ch >> 12) & 15];
                *out++ = Py_hexdigits[(ch >> 8) & 15];
                *out++ = Py_hexdigits[(ch >> 4) & 15];
                *out++ = Py_hexdigits[ch & 15];
            }
        }
        else {
            if (out + 10 <= end) {
                *out++ = '\\';
                *out++ = 'U';
                *out++ = Py_hexdigits[(ch >> 28) & 15];
                *out++ = Py_hexdigits[(ch >> 24) & 15];
                *out++ = Py_hexdigits[(ch >> 20) & 15];
                *out++ = Py_hexdigits[(ch >> 16) & 15];
                *out++ = Py_hexdigits[(ch >> 12) & 15];
                *out++ = Py_hexdigits[(ch >> 8) & 15];
                *out++ = Py_hexdigits[(ch >> 4) & 15];
                *out++ = Py_hexdigits[ch & 15];
            }
        }
    }
    if (truncated && out + 3 <= end) {
        *out++ = '.';
        *out++ = '.';
        *out++ = '.';
    }
    *out = '\0';
    return out - buf;

invalid:
    if (buf_size >= 4) {
        buf[0] = '?';
        buf[1] = '?';
        buf[2] = '?';
        buf[3] = '\0';
        return 3;
    }
    return 0;
}


#ifdef MS_WINDOWS
static void
_Py_DumpWideString(int fd, wchar_t *str)
{
    Py_ssize_t size = wcslen(str);
    int truncated;
    if (MAX_STRING_LENGTH < size) {
        size = MAX_STRING_LENGTH;
        truncated = 1;
    }
    else {
        truncated = 0;
    }

    for (Py_ssize_t i=0; i < size; i++) {
        Py_UCS4 ch = str[i];
        if (' ' <= ch && ch <= 126) {
            /* printable ASCII character */
            dump_char(fd, (char)ch);
        }
        else if (ch <= 0xff) {
            PUTS(fd, "\\x");
            _Py_DumpHexadecimal(fd, ch, 2);
        }
        else if (Py_UNICODE_IS_HIGH_SURROGATE(ch)
                 && Py_UNICODE_IS_LOW_SURROGATE(str[i+1])) {
            ch = Py_UNICODE_JOIN_SURROGATES(ch, str[i+1]);
            i++;  // Skip the low surrogate character
            PUTS(fd, "\\U");
            _Py_DumpHexadecimal(fd, ch, 8);
        }
        else {
            Py_BUILD_ASSERT(sizeof(wchar_t) == 2);
            PUTS(fd, "\\u");
            _Py_DumpHexadecimal(fd, ch, 4);
        }
    }

    if (truncated) {
        PUTS(fd, "...");
    }
}
#endif


/* Write a frame into the file fd: "File "xxx", line xxx in xxx".

   This function is signal safe.

   Return 0 on success. Return -1 if the frame is invalid. */

static int _Py_NO_SANITIZE_THREAD
dump_frame(int fd, _PyInterpreterFrame *frame)
{
    if (frame->owner == FRAME_OWNED_BY_INTERPRETER) {
        /* Ignore trampoline frames and base frame sentinel */
        return 0;
    }

    PyCodeObject *code = _PyFrame_SafeGetCode(frame);
    if (code == NULL) {
        return -1;
    }

    int res = 0;
    PUTS(fd, "  File ");
    if (code->co_filename != NULL
        && PyUnicode_Check(code->co_filename))
    {
        PUTS(fd, "\"");
        _Py_DumpASCII(fd, code->co_filename);
        PUTS(fd, "\"");
    }
    else {
        PUTS(fd, "???");
        res = -1;
    }

    PUTS(fd, ", line ");
    int lasti = _PyFrame_SafeGetLasti(frame);
    int lineno = -1;
    if (lasti >= 0) {
        lineno = _PyCode_SafeAddr2Line(code, lasti);
    }
    if (lineno >= 0) {
        _Py_DumpDecimal(fd, (size_t)lineno);
    }
    else {
        PUTS(fd, "???");
        res = -1;
    }

    PUTS(fd, " in ");
    if (code->co_name != NULL && PyUnicode_Check(code->co_name)) {
        _Py_DumpASCII(fd, code->co_name);
    }
    else {
        PUTS(fd, "???");
        res = -1;
    }
    PUTS(fd, "\n");
    return res;
}

static int _Py_NO_SANITIZE_THREAD
tstate_is_freed(PyThreadState *tstate)
{
    if (_PyMem_IsPtrFreed(tstate)) {
        return 1;
    }
    if (_PyMem_IsPtrFreed(tstate->interp)) {
        return 1;
    }
    if (_PyMem_IsULongFreed(tstate->thread_id)) {
        return 1;
    }
    return 0;
}


static int _Py_NO_SANITIZE_THREAD
interp_is_freed(PyInterpreterState *interp)
{
    return _PyMem_IsPtrFreed(interp);
}


static void _Py_NO_SANITIZE_THREAD
dump_traceback(int fd, PyThreadState *tstate, int write_header)
{
    if (write_header) {
        PUTS(fd, "Stack (most recent call first):\n");
    }

    if (tstate_is_freed(tstate)) {
        PUTS(fd, "  <freed thread state>\n");
        return;
    }

    _PyInterpreterFrame *frame = tstate->current_frame;
    if (frame == NULL) {
        PUTS(fd, "  <no Python frame>\n");
        return;
    }

    unsigned int depth = 0;
    while (1) {
        if (MAX_FRAME_DEPTH <= depth) {
            if (MAX_FRAME_DEPTH < depth) {
                PUTS(fd, "plus ");
                _Py_DumpDecimal(fd, depth);
                PUTS(fd, " frames\n");
            }
            break;
        }

        if (_PyMem_IsPtrFreed(frame)) {
            PUTS(fd, "  <freed frame>\n");
            break;
        }
        // Read frame->previous early since memory can be freed during
        // dump_frame()
        _PyInterpreterFrame *previous = frame->previous;

        if (dump_frame(fd, frame) < 0) {
            PUTS(fd, "  <invalid frame>\n");
            break;
        }

        frame = previous;
        if (frame == NULL) {
            break;
        }
        depth++;
    }
}

/* Dump the traceback of a Python thread into fd. Use write() to write the
   traceback and retry if write() is interrupted by a signal (failed with
   EINTR), but don't call the Python signal handler.

   The caller is responsible to call PyErr_CheckSignals() to call Python signal
   handlers if signals were received. */
void
_Py_DumpTraceback(int fd, PyThreadState *tstate)
{
    dump_traceback(fd, tstate, 1);
}

/* Fill a single frame struct from an interpreter frame. Signal safe.
   Return 0 on success, -1 if frame should be skipped, -2 if invalid. */
static int _Py_NO_SANITIZE_THREAD
fill_frame_struct(PyTracebackFrameInfo *out, _PyInterpreterFrame *frame)
{
    if (frame->owner == FRAME_OWNED_BY_INTERPRETER) {
        return -1;  /* skip trampoline/sentinel frames */
    }

    PyCodeObject *code = _PyFrame_SafeGetCode(frame);
    if (code == NULL) {
        return -2;
    }

    if (code->co_filename != NULL && PyUnicode_Check(code->co_filename)) {
        dump_ascii_to_buffer(out->filename, Py_TRACEBACK_FRAME_FILENAME_MAX,
                            code->co_filename);
    }
    else {
        if (Py_TRACEBACK_FRAME_FILENAME_MAX >= 4) {
            out->filename[0] = '?';
            out->filename[1] = '?';
            out->filename[2] = '?';
            out->filename[3] = '\0';
        }
    }

    int lasti = _PyFrame_SafeGetLasti(frame);
    int lineno = -1;
    if (lasti >= 0) {
        lineno = _PyCode_SafeAddr2Line(code, lasti);
    }
    out->lineno = lineno >= 0 ? lineno : 0;

    if (code->co_name != NULL && PyUnicode_Check(code->co_name)) {
        dump_ascii_to_buffer(out->name, Py_TRACEBACK_FRAME_NAME_MAX,
                            code->co_name);
    }
    else {
        if (Py_TRACEBACK_FRAME_NAME_MAX >= 4) {
            out->name[0] = '?';
            out->name[1] = '?';
            out->name[2] = '?';
            out->name[3] = '\0';
        }
    }

    return 0;
}

int
_Py_GetTracebackFrames(PyThreadState *tstate, PyTracebackFrameInfo *frames,
                      int max_frames)
{
    if (max_frames <= 0 || frames == NULL) {
        return 0;
    }
    if (tstate_is_freed(tstate)) {
        return -1;
    }

    _PyInterpreterFrame *frame = tstate->current_frame;
    if (frame == NULL) {
        return 0;
    }

    int count = 0;
    while (count < max_frames) {
        if (_PyMem_IsPtrFreed(frame)) {
            break;
        }
        _PyInterpreterFrame *previous = frame->previous;

        int res = fill_frame_struct(&frames[count], frame);
        if (res == 0) {
            count++;
        }
        else if (res == -2) {
            /* invalid frame, stop */
            break;
        }
        /* res == -1: skip this frame, continue */

        frame = previous;
        if (frame == NULL) {
            break;
        }
    }

    return count;
}

#if defined(HAVE_PTHREAD_GETNAME_NP) || defined(HAVE_PTHREAD_GET_NAME_NP)
# if defined(__OpenBSD__)
    /* pthread_*_np functions, especially pthread_{get,set}_name_np().
       pthread_np.h exists on both OpenBSD and FreeBSD but the latter declares
       pthread_getname_np() and pthread_setname_np() in pthread.h as long as
       __BSD_VISIBLE remains set.
     */
#   include <pthread_np.h>
# endif
#endif


// Write the thread name
static void _Py_NO_SANITIZE_THREAD
write_thread_name(int fd, PyThreadState *tstate)
{
#ifndef MS_WINDOWS
#if defined(HAVE_PTHREAD_GETNAME_NP) || defined(HAVE_PTHREAD_GET_NAME_NP)
    char name[100];
    pthread_t thread = (pthread_t)tstate->thread_id;
#ifdef HAVE_PTHREAD_GETNAME_NP
    int rc = pthread_getname_np(thread, name, Py_ARRAY_LENGTH(name));
#else /* defined(HAVE_PTHREAD_GET_NAME_NP) */
    int rc = 0; /* pthread_get_name_np() returns void */
    pthread_get_name_np(thread, name, Py_ARRAY_LENGTH(name));
#endif
    if (!rc) {
        size_t len = strlen(name);
        if (len) {
            PUTS(fd, " [");
            (void)_Py_write_noraise(fd, name, len);
            PUTS(fd, "]");
        }
    }
#endif
#else
    // Windows implementation
    if (pGetThreadDescription == NULL) {
        return;
    }

    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tstate->thread_id);
    if (thread == NULL) {
        return;
    }

    wchar_t *name;
    HRESULT hr = pGetThreadDescription(thread, &name);
    if (!FAILED(hr)) {
        if (name[0] != 0) {
            PUTS(fd, " [");
            _Py_DumpWideString(fd, name);
            PUTS(fd, "]");
        }
        LocalFree(name);
    }
    CloseHandle(thread);
#endif
}


/* Write the thread identifier into the file 'fd': "Current thread 0xHHHH:\" if
   is_current is true, "Thread 0xHHHH:\n" otherwise.

   This function is signal safe (except on Windows). */

static void _Py_NO_SANITIZE_THREAD
write_thread_id(int fd, PyThreadState *tstate, int is_current)
{
    if (is_current)
        PUTS(fd, "Current thread 0x");
    else
        PUTS(fd, "Thread 0x");
    _Py_DumpHexadecimal(fd,
                        tstate->thread_id,
                        sizeof(unsigned long) * 2);

    if (!_PyMem_IsULongFreed(tstate->thread_id)) {
        write_thread_name(fd, tstate);
    }

    PUTS(fd, " (most recent call first):\n");
}

/* Dump the traceback of all Python threads into fd. Use write() to write the
   traceback and retry if write() is interrupted by a signal (failed with
   EINTR), but don't call the Python signal handler.

   The caller is responsible to call PyErr_CheckSignals() to call Python signal
   handlers if signals were received. */
const char* _Py_NO_SANITIZE_THREAD
_Py_DumpTracebackThreads(int fd, PyInterpreterState *interp,
                         PyThreadState *current_tstate)
{
    if (current_tstate == NULL) {
        /* _Py_DumpTracebackThreads() is called from signal handlers by
           faulthandler.

           SIGSEGV, SIGFPE, SIGABRT, SIGBUS and SIGILL are synchronous signals
           and are thus delivered to the thread that caused the fault. Get the
           Python thread state of the current thread.

           PyThreadState_Get() doesn't give the state of the thread that caused
           the fault if the thread released the GIL, and so
           _PyThreadState_GET() cannot be used. Read the thread specific
           storage (TSS) instead: call PyGILState_GetThisThreadState(). */
        current_tstate = PyGILState_GetThisThreadState();
    }

    if (current_tstate != NULL && tstate_is_freed(current_tstate)) {
        return "tstate is freed";
    }

    if (interp == NULL) {
        if (current_tstate == NULL) {
            interp = _PyGILState_GetInterpreterStateUnsafe();
            if (interp == NULL) {
                /* We need the interpreter state to get Python threads */
                return "unable to get the interpreter state";
            }
        }
        else {
            interp = current_tstate->interp;
        }
    }
    assert(interp != NULL);

    if (interp_is_freed(interp)) {
        return "interp is freed";
    }

    /* Get the current interpreter from the current thread */
    PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
    if (tstate == NULL)
        return "unable to get the thread head state";

    /* Dump the traceback of each thread */
    unsigned int nthreads = 0;
    _Py_BEGIN_SUPPRESS_IPH
    do
    {
        if (nthreads != 0)
            PUTS(fd, "\n");
        if (nthreads >= MAX_NTHREADS) {
            PUTS(fd, "...\n");
            break;
        }

        if (tstate_is_freed(tstate)) {
            PUTS(fd, "<freed thread state>\n");
            break;
        }

        write_thread_id(fd, tstate, tstate == current_tstate);
        if (tstate == current_tstate && tstate->interp->gc.collecting) {
            PUTS(fd, "  Garbage-collecting\n");
        }
        dump_traceback(fd, tstate, 0);

        tstate = tstate->next;
        nthreads++;
    } while (tstate != NULL);
    _Py_END_SUPPRESS_IPH

    return NULL;
}

#ifdef CAN_C_BACKTRACE
/* Based on glibc's implementation of backtrace_symbols(), but only uses stack memory. */
void
_Py_backtrace_symbols_fd(int fd, void *const *array, Py_ssize_t size)
{
    VLA(Dl_info, info, size);
    VLA(int, status, size);
    /* Fill in the information we can get from dladdr() */
    for (Py_ssize_t i = 0; i < size; ++i) {
#ifdef __APPLE__
        status[i] = dladdr(array[i], &info[i]);
#else
        struct link_map *map;
        status[i] = dladdr1(array[i], &info[i], (void **)&map, RTLD_DL_LINKMAP);
        if (status[i] != 0
            && info[i].dli_fname != NULL
            && info[i].dli_fname[0] != '\0') {
            /* The load bias is more useful to the user than the load
               address. The use of these addresses is to calculate an
               address in the ELF file, so its prelinked bias is not
               something we want to subtract out */
            info[i].dli_fbase = (void *) map->l_addr;
        }
#endif
    }
    for (Py_ssize_t i = 0; i < size; ++i) {
        if (status[i] == 0
            || info[i].dli_fname == NULL
            || info[i].dli_fname[0] == '\0'
        ) {
            PUTS(fd, "  Binary file '<unknown>' [");
            dump_pointer(fd, array[i]);
            PUTS(fd, "]\n");
            continue;
        }

        if (info[i].dli_sname == NULL) {
            /* We found no symbol name to use, so describe it as
               relative to the file. */
            info[i].dli_saddr = info[i].dli_fbase;
        }

        if (info[i].dli_sname == NULL && info[i].dli_saddr == 0) {
            PUTS(fd, "  Binary file \"");
            PUTS(fd, info[i].dli_fname);
            PUTS(fd, "\" [");
            dump_pointer(fd, array[i]);
            PUTS(fd, "]\n");
        }
        else {
            char sign;
            ptrdiff_t offset;
            if (array[i] >= (void *) info[i].dli_saddr) {
                sign = '+';
                offset = array[i] - info[i].dli_saddr;
            }
            else {
                sign = '-';
                offset = info[i].dli_saddr - array[i];
            }
            const char *symbol_name = info[i].dli_sname != NULL ? info[i].dli_sname : "";
            PUTS(fd, "  Binary file \"");
            PUTS(fd, info[i].dli_fname);
            PUTS(fd, "\", at ");
            PUTS(fd, symbol_name);
            dump_char(fd, sign);
            PUTS(fd, "0x");
            dump_hexadecimal(fd, offset, sizeof(offset), 1);
            PUTS(fd, " [");
            dump_pointer(fd, array[i]);
            PUTS(fd, "]\n");
        }
    }
}

void
_Py_DumpStack(int fd)
{
#define BACKTRACE_SIZE 32
    PUTS(fd, "Current thread's C stack trace (most recent call first):\n");
    VLA(void *, callstack, BACKTRACE_SIZE);
    int frames = backtrace(callstack, BACKTRACE_SIZE);
    if (frames == 0) {
        // Some systems won't return anything for the stack trace
        PUTS(fd, "  <system returned no stack trace>\n");
        return;
    }

    _Py_backtrace_symbols_fd(fd, callstack, frames);
    if (frames == BACKTRACE_SIZE) {
        PUTS(fd, "  <truncated rest of calls>\n");
    }

#undef BACKTRACE_SIZE
}

/* Fill buffer with up to size return addresses. Returns number captured. */
int
_Py_GetBacktrace(void **buffer, int size)
{
    if (buffer == NULL || size <= 0) {
        return 0;
    }
    int frames = backtrace(buffer, size);
    return (frames > 0) ? frames : 0;
}

/* Dump a previously captured backtrace array to fd. */
void
_Py_DumpBacktraceFromArray(int fd, void *const *array, int size)
{
    if (array == NULL || size <= 0) {
        return;
    }
    PUTS(fd, "  C stack (most recent call first):\n");
    _Py_backtrace_symbols_fd(fd, array, size);
}
#else
void
_Py_DumpStack(int fd)
{
    PUTS(fd, "Current thread's C stack trace (most recent call first):\n");
    PUTS(fd, "  <cannot get C stack on this system>\n");
}

int
_Py_GetBacktrace(void **buffer, int size)
{
    (void)buffer;
    (void)size;
    return 0;
}

void
_Py_DumpBacktraceFromArray(int fd, void *const *array, int size)
{
    (void)fd;
    (void)array;
    (void)size;
}
#endif

void
_Py_InitDumpStack(void)
{
#ifdef CAN_C_BACKTRACE
    // gh-137185: Call backtrace() once to force libgcc to be loaded early.
    void *callstack[1];
    (void)backtrace(callstack, 1);
#endif
}


void
_Py_DumpTraceback_Init(void)
{
#ifdef MS_WINDOWS
    if (pGetThreadDescription != NULL) {
        return;
    }

    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase != NULL) {
        pGetThreadDescription = (PF_GET_THREAD_DESCRIPTION)GetProcAddress(
                                    kernelbase, "GetThreadDescription");
    }
#endif
}


/* Traceback interning table implementation
   ========================================
   Three-level interning: strings -> frames -> tracebacks.
   Uses _Py_hashtable. Each level is refcounted. */

/* Key structs for hash/compare - first fields must match interned structs */
typedef struct {
    const char *str;
    size_t len;
} tb_string_key_t;

typedef struct {
    Py_traceback_string_id_t filename_id;
    int lineno;
    Py_traceback_string_id_t name_id;
} tb_frame_key_t;

typedef struct {
    Py_traceback_frame_id_t *frame_ids;
    int frame_count;
} tb_traceback_key_t;

struct _Py_traceback_interned_string {
    char *str;
    size_t len;
    int refcount;
    int64_t export_index;  /* scratch: pprof/otel string table index, -1 when unset */
};
typedef struct _Py_traceback_interned_string interned_string_t;

#define TB_EXPORT_INDEX_UNSET ((uint32_t)-1)

struct _Py_traceback_interned_frame {
    Py_traceback_string_id_t filename_id;
    int lineno;
    Py_traceback_string_id_t name_id;
    int refcount;
    uint32_t export_function_index;  /* scratch: OTel function table index */
    uint32_t export_location_index;  /* scratch: OTel location table index */
};
typedef struct _Py_traceback_interned_frame interned_frame_t;

struct _Py_traceback_interned_traceback {
    Py_traceback_frame_id_t *frame_ids;
    int frame_count;
    int refcount;
};
typedef struct _Py_traceback_interned_traceback interned_traceback_t;

struct _Py_traceback_interning_table {
    _Py_hashtable_t *strings;
    _Py_hashtable_t *frames;
    _Py_hashtable_t *tracebacks;
    void *(*malloc)(size_t size);
    void (*free)(void *ptr);
};

/* FNV-1a hash - returns Py_uhash_t for _Py_hashtable */
static Py_uhash_t
tb_string_hash(const void *key)
{
    const tb_string_key_t *k = (const tb_string_key_t *)key;
    Py_uhash_t h = 2166136261u;
    for (size_t i = 0; i < k->len; i++) {
        h ^= (unsigned char)k->str[i];
        h *= 16777619u;
    }
    return h;
}

static int
tb_string_compare(const void *k1, const void *k2)
{
    const tb_string_key_t *a = (const tb_string_key_t *)k1;
    const tb_string_key_t *b = (const tb_string_key_t *)k2;
    return a->len == b->len && memcmp(a->str, b->str, a->len) == 0;
}

static Py_uhash_t
tb_frame_hash(const void *key)
{
    const tb_frame_key_t *k = (const tb_frame_key_t *)key;
    Py_uhash_t h = 2166136261u;
    h ^= (Py_uhash_t)(uintptr_t)k->filename_id;
    h *= 16777619u;
    h ^= (Py_uhash_t)(unsigned)k->lineno;
    h *= 16777619u;
    h ^= (Py_uhash_t)(uintptr_t)k->name_id;
    h *= 16777619u;
    return h;
}

static int
tb_frame_compare(const void *k1, const void *k2)
{
    const tb_frame_key_t *a = (const tb_frame_key_t *)k1;
    const tb_frame_key_t *b = (const tb_frame_key_t *)k2;
    return a->filename_id == b->filename_id
        && a->lineno == b->lineno
        && a->name_id == b->name_id;
}

static Py_uhash_t
tb_traceback_hash(const void *key)
{
    const tb_traceback_key_t *k = (const tb_traceback_key_t *)key;
    Py_uhash_t h = 2166136261u;
    for (int i = 0; i < k->frame_count; i++) {
        h ^= (Py_uhash_t)(uintptr_t)k->frame_ids[i];
        h *= 16777619u;
    }
    return h;
}

static int
tb_traceback_compare(const void *k1, const void *k2)
{
    const tb_traceback_key_t *a = (const tb_traceback_key_t *)k1;
    const tb_traceback_key_t *b = (const tb_traceback_key_t *)k2;
    if (a->frame_count != b->frame_count) {
        return 0;
    }
    return memcmp(a->frame_ids, b->frame_ids,
                  (size_t)a->frame_count * sizeof(Py_traceback_frame_id_t)) == 0;
}

static void
tb_intern_string_release(interned_string_t *ent,
                         Py_traceback_interning_table_t *table);

static void
tb_intern_frame_release(interned_frame_t *ent,
                        Py_traceback_interning_table_t *table);

static void
tb_intern_traceback_release(interned_traceback_t *ent,
                            Py_traceback_interning_table_t *table);

Py_traceback_interning_table_t *
_Py_traceback_interning_table_new(const Py_traceback_interning_allocator_t *allocator)
{
    void *(*malloc_fn)(size_t) = allocator && allocator->malloc
        ? allocator->malloc : (void *(*)(size_t))PyMem_Malloc;
    void (*free_fn)(void *) = allocator && allocator->free
        ? allocator->free : (void (*)(void *))PyMem_Free;

    Py_traceback_interning_table_t *table = malloc_fn(sizeof(*table));
    if (table == NULL) {
        return NULL;
    }
    table->malloc = malloc_fn;
    table->free = free_fn;

    _Py_hashtable_allocator_t ht_alloc = { malloc_fn, free_fn };
    table->strings = _Py_hashtable_new_full(tb_string_hash, tb_string_compare,
                                            NULL, NULL, &ht_alloc);
    table->frames = _Py_hashtable_new_full(tb_frame_hash, tb_frame_compare,
                                            NULL, NULL, &ht_alloc);
    table->tracebacks = _Py_hashtable_new_full(tb_traceback_hash, tb_traceback_compare,
                                               NULL, NULL, &ht_alloc);
    if (table->strings == NULL || table->frames == NULL || table->tracebacks == NULL) {
        if (table->strings) {
            _Py_hashtable_destroy(table->strings);
        }
        if (table->frames) {
            _Py_hashtable_destroy(table->frames);
        }
        if (table->tracebacks) {
            _Py_hashtable_destroy(table->tracebacks);
        }
        free_fn(table);
        return NULL;
    }
    return table;
}

static void
tb_intern_string_release(interned_string_t *ent,
                         Py_traceback_interning_table_t *table)
{
    ent->refcount--;
    if (ent->refcount == 0) {
        tb_string_key_t key = { .str = ent->str, .len = ent->len };
        (void)_Py_hashtable_steal(table->strings, &key);
        table->free(ent->str);
        table->free(ent);
    }
}

static Py_traceback_string_id_t
tb_intern_string(const char *str, size_t len,
                 Py_traceback_interning_table_t *table)
{
    if (str == NULL || len == 0) {
        return NULL;
    }
    tb_string_key_t lookup_key = { .str = str, .len = len };
    interned_string_t *ent = (interned_string_t *)_Py_hashtable_get(table->strings,
                                                                    &lookup_key);
    if (ent != NULL) {
        ent->refcount++;
        return (Py_traceback_string_id_t)ent;
    }

    ent = table->malloc(sizeof(*ent));
    if (ent == NULL) {
        return NULL;
    }
    ent->str = table->malloc(len + 1);
    if (ent->str == NULL) {
        table->free(ent);
        return NULL;
    }
    memcpy(ent->str, str, len);
    ent->str[len] = '\0';
    ent->len = len;
    ent->refcount = 1;
    ent->export_index = -1;

    if (_Py_hashtable_set(table->strings, ent, ent) < 0) {
        table->free(ent->str);
        table->free(ent);
        return NULL;
    }
    return (Py_traceback_string_id_t)ent;
}

static Py_traceback_frame_id_t
tb_intern_frame(Py_traceback_string_id_t filename_id,
                int lineno,
                Py_traceback_string_id_t name_id,
                Py_traceback_interning_table_t *table)
{
    tb_frame_key_t lookup_key = {
        .filename_id = filename_id,
        .lineno = lineno,
        .name_id = name_id,
    };
    interned_frame_t *ent = (interned_frame_t *)_Py_hashtable_get(table->frames,
                                                                  &lookup_key);
    if (ent != NULL) {
        ent->refcount++;
        return (Py_traceback_frame_id_t)ent;
    }

    ent = table->malloc(sizeof(*ent));
    if (ent == NULL) {
        return NULL;
    }
    ent->filename_id = filename_id;
    ent->lineno = lineno;
    ent->name_id = name_id;
    ent->refcount = 1;
    ent->export_function_index = TB_EXPORT_INDEX_UNSET;
    ent->export_location_index = TB_EXPORT_INDEX_UNSET;

    if (filename_id) {
        ((interned_string_t *)filename_id)->refcount++;
    }
    if (name_id) {
        ((interned_string_t *)name_id)->refcount++;
    }

    if (_Py_hashtable_set(table->frames, ent, ent) < 0) {
        if (filename_id) {
            tb_intern_string_release((interned_string_t *)filename_id, table);
        }
        if (name_id) {
            tb_intern_string_release((interned_string_t *)name_id, table);
        }
        table->free(ent);
        return NULL;
    }
    return (Py_traceback_frame_id_t)ent;
}

static void
tb_intern_frame_release(interned_frame_t *ent,
                        Py_traceback_interning_table_t *table)
{
    ent->refcount--;
    if (ent->refcount == 0) {
        tb_frame_key_t key = {
            .filename_id = ent->filename_id,
            .lineno = ent->lineno,
            .name_id = ent->name_id,
        };
        (void)_Py_hashtable_steal(table->frames, &key);
        if (ent->filename_id) {
            tb_intern_string_release((interned_string_t *)ent->filename_id, table);
        }
        if (ent->name_id) {
            tb_intern_string_release((interned_string_t *)ent->name_id, table);
        }
        table->free(ent);
    }
}

static Py_traceback_id_t
tb_intern_traceback(Py_traceback_frame_id_t *frame_ids, int count,
                    Py_traceback_interning_table_t *table)
{
    if (frame_ids == NULL || count <= 0) {
        return NULL;
    }
    tb_traceback_key_t lookup_key = {
        .frame_ids = frame_ids,
        .frame_count = count,
    };
    interned_traceback_t *ent = (interned_traceback_t *)_Py_hashtable_get(
        table->tracebacks, &lookup_key);
    if (ent != NULL) {
        ent->refcount++;
        return (Py_traceback_id_t)ent;
    }

    ent = table->malloc(sizeof(*ent));
    if (ent == NULL) {
        return NULL;
    }
    ent->frame_ids = table->malloc((size_t)count * sizeof(Py_traceback_frame_id_t));
    if (ent->frame_ids == NULL) {
        table->free(ent);
        return NULL;
    }
    memcpy(ent->frame_ids, frame_ids, (size_t)count * sizeof(frame_ids[0]));
    ent->frame_count = count;
    ent->refcount = 1;

    for (int i = 0; i < count; i++) {
        ((interned_frame_t *)frame_ids[i])->refcount++;
    }

    if (_Py_hashtable_set(table->tracebacks, ent, ent) < 0) {
        for (int i = 0; i < count; i++) {
            tb_intern_frame_release((interned_frame_t *)frame_ids[i], table);
        }
        table->free(ent->frame_ids);
        table->free(ent);
        return NULL;
    }
    return (Py_traceback_id_t)ent;
}

static void
tb_intern_traceback_release(interned_traceback_t *ent,
                             Py_traceback_interning_table_t *table)
{
    ent->refcount--;
    if (ent->refcount == 0) {
        tb_traceback_key_t key = {
            .frame_ids = ent->frame_ids,
            .frame_count = ent->frame_count,
        };
        (void)_Py_hashtable_steal(table->tracebacks, &key);
        for (int i = 0; i < ent->frame_count; i++) {
            tb_intern_frame_release((interned_frame_t *)ent->frame_ids[i], table);
        }
        table->free(ent->frame_ids);
        table->free(ent);
    }
}

Py_traceback_id_t
_Py_traceback_intern(const PyTracebackFrameInfo *frames,
                     int count,
                     Py_traceback_interning_table_t *table)
{
    if (frames == NULL || count <= 0 || table == NULL) {
        return NULL;
    }

    Py_traceback_frame_id_t *frame_ids = table->malloc((size_t)count * sizeof(Py_traceback_frame_id_t));
    if (frame_ids == NULL) {
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        const PyTracebackFrameInfo *f = &frames[i];
        size_t filename_len = strlen(f->filename);
        size_t name_len = strlen(f->name);

        Py_traceback_string_id_t filename_id = tb_intern_string(
            f->filename, filename_len, table);
        Py_traceback_string_id_t name_id = tb_intern_string(
            f->name, name_len, table);

        if (filename_id == NULL && filename_len > 0) {
            goto fail;
        }
        if (name_id == NULL && name_len > 0) {
            if (filename_id) {
                tb_intern_string_release((interned_string_t *)filename_id, table);
            }
            goto fail;
        }

        frame_ids[i] = tb_intern_frame(filename_id, f->lineno, name_id, table);
        if (filename_id) {
            tb_intern_string_release((interned_string_t *)filename_id, table);
        }
        if (name_id) {
            tb_intern_string_release((interned_string_t *)name_id, table);
        }
        if (frame_ids[i] == NULL) {
            for (int j = 0; j < i; j++) {
                tb_intern_frame_release((interned_frame_t *)frame_ids[j], table);
            }
            goto fail;
        }
    }

    Py_traceback_id_t result = tb_intern_traceback(frame_ids, count, table);
    for (int i = 0; i < count; i++) {
        tb_intern_frame_release((interned_frame_t *)frame_ids[i], table);
    }
    table->free(frame_ids);
    return result;

fail:
    table->free(frame_ids);
    return NULL;
}

void
_Py_traceback_release(Py_traceback_id_t traceback_id,
                      Py_traceback_interning_table_t *table)
{
    if (traceback_id != NULL && table != NULL) {
        tb_intern_traceback_release((interned_traceback_t *)traceback_id, table);
    }
}

void
_Py_traceback_dump_id(Py_traceback_id_t traceback_id,
                      Py_traceback_interning_table_t *table,
                      int fd)
{
    if (traceback_id == NULL || table == NULL) {
        return;
    }
    interned_traceback_t *tb = (interned_traceback_t *)traceback_id;
    if (tb->frame_count <= 0) {
        return;
    }
    PUTS(fd, "  Allocation traceback (most recent first):\n");
    for (int i = 0; i < tb->frame_count; i++) {
        interned_frame_t *f = (interned_frame_t *)tb->frame_ids[i];
        const char *filename = f->filename_id
            ? ((interned_string_t *)f->filename_id)->str : "???";
        const char *name = f->name_id
            ? ((interned_string_t *)f->name_id)->str : "???";
        PUTS(fd, "    File \"");
        PUTS(fd, filename);
        PUTS(fd, "\", line ");
        _Py_DumpDecimal(fd, (size_t)f->lineno);
        PUTS(fd, " in ");
        PUTS(fd, name);
        PUTS(fd, "\n");
    }
}

int
_Py_traceback_fill_frames(Py_traceback_id_t traceback_id,
                         Py_traceback_interning_table_t *table,
                         PyTracebackFrameInfo *frames,
                         int max_frames)
{
    if (traceback_id == NULL || table == NULL || frames == NULL || max_frames <= 0) {
        return 0;
    }
    interned_traceback_t *tb = (interned_traceback_t *)traceback_id;
    int count = tb->frame_count;
    if (count <= 0) return 0;
    if (count > max_frames) count = max_frames;
    for (int i = 0; i < count; i++) {
        interned_frame_t *f = (interned_frame_t *)tb->frame_ids[i];
        const char *filename = f->filename_id
            ? ((interned_string_t *)f->filename_id)->str : "???";
        const char *name = f->name_id
            ? ((interned_string_t *)f->name_id)->str : "???";

        strncpy(frames[i].filename, filename, Py_TRACEBACK_FRAME_FILENAME_MAX - 1);
        frames[i].filename[Py_TRACEBACK_FRAME_FILENAME_MAX - 1] = '\0';

        frames[i].lineno = f->lineno;

        strncpy(frames[i].name, name, Py_TRACEBACK_FRAME_NAME_MAX - 1);
        frames[i].name[Py_TRACEBACK_FRAME_NAME_MAX - 1] = '\0';
    }
    return count;
}

int
_Py_traceback_fill_frames_with_string_ids(Py_traceback_id_t traceback_id,
                                          Py_traceback_interning_table_t *table,
                                          PyTracebackFrameInfoWithIds *frames,
                                          int max_frames)
{
    if (traceback_id == NULL || table == NULL || frames == NULL || max_frames <= 0) {
        return 0;
    }
    interned_traceback_t *tb = (interned_traceback_t *)traceback_id;
    int count = tb->frame_count;
    if (count <= 0) return 0;
    if (count > max_frames) count = max_frames;
    for (int i = 0; i < count; i++) {
        interned_frame_t *f = (interned_frame_t *)tb->frame_ids[i];
        frames[i].filename_id = f->filename_id;
        frames[i].name_id = f->name_id;
        frames[i].lineno = f->lineno;
    }
    return count;
}

int
_Py_traceback_fill_frame_ids(Py_traceback_id_t traceback_id,
                             Py_traceback_interning_table_t *table,
                             Py_traceback_frame_id_t *frame_ids,
                             int max_frames)
{
    if (traceback_id == NULL || table == NULL || frame_ids == NULL || max_frames <= 0) {
        return 0;
    }
    interned_traceback_t *tb = (interned_traceback_t *)traceback_id;
    int count = tb->frame_count;
    if (count <= 0) return 0;
    if (count > max_frames) count = max_frames;
    memcpy(frame_ids, tb->frame_ids, (size_t)count * sizeof(Py_traceback_frame_id_t));
    return count;
}

const char *
_Py_traceback_string_id_get_str(Py_traceback_string_id_t string_id)
{
    assert(string_id != NULL);
    return ((interned_string_t *)string_id)->str;
}

struct build_export_ctx {
    void *ctx;
    _Py_traceback_export_add_string_fn add_fn;
};

static int
tb_build_export_string_cb(_Py_hashtable_t *ht, const void *key, const void *value,
                          void *user_data)
{
    struct build_export_ctx *bc = (struct build_export_ctx *)user_data;
    interned_string_t *ent = (interned_string_t *)value;
    int64_t idx = bc->add_fn(bc->ctx, ent->str);
    if (idx < 0) {
        return -1;
    }
    ent->export_index = idx;
    return 0;
}

int
_Py_traceback_build_export_string_table(Py_traceback_interning_table_t *table,
                                        void *ctx,
                                        _Py_traceback_export_add_string_fn add_fn)
{
    struct build_export_ctx bc = { .ctx = ctx, .add_fn = add_fn };
    return _Py_hashtable_foreach(table->strings, tb_build_export_string_cb, &bc);
}

int64_t
_Py_traceback_string_id_get_export_index(Py_traceback_string_id_t string_id,
                                        int64_t null_index)
{
    if (string_id == NULL) {
        return null_index;
    }
    int64_t idx = ((interned_string_t *)string_id)->export_index;
    assert(idx >= 0);
    return idx;
}

static int
tb_clear_export_index_cb(_Py_hashtable_t *ht, const void *key, const void *value,
                         void *user_data)
{
    (void)ht;
    (void)key;
    (void)user_data;
    ((interned_string_t *)value)->export_index = -1;
    return 0;
}

void
_Py_traceback_clear_export_indices(Py_traceback_interning_table_t *table)
{
    _Py_hashtable_foreach(table->strings, tb_clear_export_index_cb, NULL);
}

struct build_export_frame_ctx {
    void *ctx;
    _Py_traceback_export_add_frame_fn add_fn;
    int64_t null_idx;
};

static int
tb_build_export_frame_cb(_Py_hashtable_t *ht, const void *key, const void *value,
                         void *user_data)
{
    struct build_export_frame_ctx *bc = (struct build_export_frame_ctx *)user_data;
    interned_frame_t *ent = (interned_frame_t *)value;
    int64_t filename_idx = _Py_traceback_string_id_get_export_index(ent->filename_id, bc->null_idx);
    int64_t name_idx = _Py_traceback_string_id_get_export_index(ent->name_id, bc->null_idx);
    int64_t result = bc->add_fn(bc->ctx, filename_idx, name_idx, ent->lineno);
    if (result < 0) {
        return -1;
    }
    ent->export_function_index = (uint32_t)(result >> 32);
    ent->export_location_index = (uint32_t)(result & 0xFFFFFFFF);
    return 0;
}

int
_Py_traceback_build_export_frame_table(Py_traceback_interning_table_t *table,
                                       void *ctx,
                                       _Py_traceback_export_add_frame_fn add_fn,
                                       int64_t null_idx)
{
    struct build_export_frame_ctx bc = {
        .ctx = ctx,
        .add_fn = add_fn,
        .null_idx = null_idx,
    };
    return _Py_hashtable_foreach(table->frames, tb_build_export_frame_cb, &bc);
}

void
_Py_traceback_frame_id_get_export_indices(Py_traceback_frame_id_t frame_id,
                                          uint32_t *out_function_index,
                                          uint32_t *out_location_index)
{
    if (frame_id == NULL) {
        if (out_function_index) *out_function_index = 0;
        if (out_location_index) *out_location_index = 0;
        return;
    }
    interned_frame_t *f = (interned_frame_t *)frame_id;
    assert(f->export_function_index != TB_EXPORT_INDEX_UNSET);
    assert(f->export_location_index != TB_EXPORT_INDEX_UNSET);
    if (out_function_index) *out_function_index = f->export_function_index;
    if (out_location_index) *out_location_index = f->export_location_index;
}

static int
tb_clear_export_frame_cb(_Py_hashtable_t *ht, const void *key, const void *value,
                         void *user_data)
{
    (void)ht;
    (void)key;
    (void)user_data;
    interned_frame_t *ent = (interned_frame_t *)value;
    ent->export_function_index = TB_EXPORT_INDEX_UNSET;
    ent->export_location_index = TB_EXPORT_INDEX_UNSET;
    return 0;
}

void
_Py_traceback_clear_export_frame_indices(Py_traceback_interning_table_t *table)
{
    _Py_hashtable_foreach(table->frames, tb_clear_export_frame_cb, NULL);
}

/* Foreach callback to free table entries (used when destroying table) */
static int
tb_free_string_cb(_Py_hashtable_t *ht, const void *key, const void *value, void *ud)
{
    Py_traceback_interning_table_t *table = (Py_traceback_interning_table_t *)ud;
    (void)ht;
    (void)key;
    interned_string_t *ent = (interned_string_t *)value;
    table->free(ent->str);
    table->free(ent);
    return 0;
}

static int
tb_free_frame_cb(_Py_hashtable_t *ht, const void *key, const void *value, void *ud)
{
    Py_traceback_interning_table_t *table = (Py_traceback_interning_table_t *)ud;
    (void)ht;
    (void)key;
    interned_frame_t *ent = (interned_frame_t *)value;
    if (ent->filename_id) {
        tb_intern_string_release((interned_string_t *)ent->filename_id, table);
    }
    if (ent->name_id) {
        tb_intern_string_release((interned_string_t *)ent->name_id, table);
    }
    table->free(ent);
    return 0;
}

static int
tb_free_traceback_cb(_Py_hashtable_t *ht, const void *key, const void *value, void *ud)
{
    Py_traceback_interning_table_t *table = (Py_traceback_interning_table_t *)ud;
    (void)ht;
    (void)key;
    interned_traceback_t *ent = (interned_traceback_t *)value;
    table->free(ent->frame_ids);
    table->free(ent);
    return 0;
}

void
_Py_traceback_interning_table_free(Py_traceback_interning_table_t *table)
{
    if (table == NULL) {
        return;
    }
    /* Free tracebacks first (no cross-refs to frames in our free path),
       then frames (releases strings), then strings. */
    _Py_hashtable_foreach(table->tracebacks, tb_free_traceback_cb, table);
    _Py_hashtable_foreach(table->frames, tb_free_frame_cb, table);
    _Py_hashtable_foreach(table->strings, tb_free_string_cb, table);
    _Py_hashtable_destroy(table->tracebacks);
    _Py_hashtable_destroy(table->frames);
    _Py_hashtable_destroy(table->strings);
    table->free(table);
}
