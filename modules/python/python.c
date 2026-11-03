/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * The python guest: an embedded CPython for wasm32-wasip1.
 *
 * The interpreter is initialized once per guest instance.  The host
 * preopens the module directory read-only at /python; the standard library
 * is the stored (uncompressed) python313.zip there, because CPython's WASI
 * build has no zlib.  A live Python heap cannot be exported, so this module
 * deliberately has no state_export/state_import: it lives for one turn.
 */

#include <Python.h>

#include "wd_abi.h"

#define HEAP_SIZE 8192u
#define REPLY_SIZE (64u * 1024u)

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];
static int ready;
static int failed;
static char failure[512];

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (heap_at + len > HEAP_SIZE) return 0;
  uint32_t at = (uint32_t)(uintptr_t)(heap + heap_at);
  heap_at += len;
  return at;
}

static int write_failure(const char *text, uint32_t *at, uint32_t *len) {
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 3);
  wd_w_textz(&w, "ok");
  wd_w_bool(&w, 0);
  wd_w_textz(&w, "status");
  wd_w_uint(&w, 1);
  wd_w_textz(&w, "text");
  wd_w_textz(&w, text != NULL ? text : "python failed");
  if (w.failed) return -1;
  *at = (uint32_t)(uintptr_t)reply;
  *len = w.at;
  return 0;
}

static void remember_status(PyStatus status) {
  if (PyStatus_Exception(status)) {
    failed = 1;
    snprintf(failure, sizeof(failure), "%s",
             status.err_msg != NULL ? status.err_msg : "python init failed");
  }
}

static int initialize(void) {
  if (ready) return failed ? -1 : 0;
  ready = 1;
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.site_import = 0;
  config.user_site_directory = 0;
  config.write_bytecode = 0;
  config.parse_argv = 0;
  config.install_signal_handlers = 0;
  config.use_environment = 0;
  config.use_frozen_modules = 1;
  PyStatus status = PyConfig_SetBytesString(&config, &config.program_name,
                                             "python.wasm");
  if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  remember_status(status);
  if (failed) {
    PyErr_Clear();
    return -1;
  }
  return 0;
}

static PyObject *redirect(const char *name, PyObject **saved) {
  PyObject *io = PyImport_ImportModule("io");
  if (io == NULL) return NULL;
  PyObject *sink = PyObject_CallMethod(io, "StringIO", NULL);
  Py_DECREF(io);
  if (sink == NULL) return NULL;
  *saved = PySys_GetObject(name);
  Py_XINCREF(*saved);
  if (PySys_SetObject(name, sink) < 0) {
    Py_DECREF(sink);
    Py_XDECREF(*saved);
    *saved = NULL;
    return NULL;
  }
  return sink;
}

static void restore(const char *name, PyObject *saved) {
  if (saved != NULL) {
    PySys_SetObject(name, saved);
    Py_DECREF(saved);
  }
}

static int run_program(const char *program, uint32_t *at, uint32_t *len) {
  if (initialize() != 0) return write_failure(failure, at, len);

  PyObject *old_out = NULL;
  PyObject *old_err = NULL;
  PyObject *out = redirect("stdout", &old_out);
  PyObject *err = redirect("stderr", &old_err);
  if (out == NULL || err == NULL) {
    PyErr_Clear();
    Py_XDECREF(out);
    Py_XDECREF(err);
    restore("stdout", old_out);
    restore("stderr", old_err);
    return write_failure("python: cannot capture output", at, len);
  }

  PyObject *main = PyImport_AddModule("__main__");
  PyObject *globals = main != NULL ? PyModule_GetDict(main) : NULL;
  PyObject *result = NULL;
  if (globals != NULL)
    result = PyRun_StringFlags(program, Py_file_input, globals, globals, NULL);
  int ok = result != NULL;
  if (!ok) PyErr_Print();
  Py_XDECREF(result);

  PyObject *out_text = PyObject_CallMethod(out, "getvalue", NULL);
  PyObject *err_text = PyObject_CallMethod(err, "getvalue", NULL);
  PyObject *both = NULL;
  if (out_text != NULL && err_text != NULL) {
    both = PyUnicode_Concat(out_text, err_text);
  }
  Py_XDECREF(out_text);
  Py_XDECREF(err_text);

  restore("stdout", old_out);
  restore("stderr", old_err);
  Py_DECREF(out);
  Py_DECREF(err);

  const char *text = "";
  Py_ssize_t text_len = 0;
  if (both != NULL) text = PyUnicode_AsUTF8AndSize(both, &text_len);
  if (text == NULL) {
    PyErr_Clear();
    text = "";
    text_len = 0;
  }
  if (text_len > (Py_ssize_t)(REPLY_SIZE - 128)) text_len = REPLY_SIZE - 128;

  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 3);
  wd_w_textz(&w, "ok");
  wd_w_bool(&w, ok);
  wd_w_textz(&w, "status");
  wd_w_uint(&w, ok ? 0 : 1);
  wd_w_textz(&w, "text");
  wd_w_text(&w, text, (uint32_t)text_len);
  Py_XDECREF(both);
  if (w.failed) return -1;
  *at = (uint32_t)(uintptr_t)reply;
  *len = w.at;
  return 0;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char program[4097];
  program[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "program")) {
      if (wd_r_text(&r, program, sizeof(program), NULL) != 0) return;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }
  uint32_t rat = 0;
  uint32_t rlen = 0;
  if (program[0] == '\0') {
    if (write_failure("python.run: the program is missing", &rat, &rlen) != 0)
      return;
  } else if (run_program(program, &rat, &rlen) != 0) {
    return;
  }
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, rat);
  wd_put32(o + 4, rlen);
}
