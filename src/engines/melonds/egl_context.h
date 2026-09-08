/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_MELONDS_EGL_CONTEXT_H
#define DUALBOY_MELONDS_EGL_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The implementation deliberately keeps EGL types out of this public header.
 * EGL is loaded at runtime so software rendering keeps working on systems
 * without an EGL development package or runtime. */
struct dualboy_egl_context {
    void *implementation;
};

typedef bool (*dualboy_egl_task_fn)(void *task_context,
                                    char *error,
                                    size_t error_size);

/* Creates one dedicated worker, obtains a process-shared EGLDisplay, and owns
 * two mutually unshared OpenGL 3.2 core contexts. A context is current only on
 * that worker. */
bool dualboy_egl_context_create(struct dualboy_egl_context *context,
                                char *error,
                                size_t error_size);

/* Runs one synchronous task on the context-owning worker. */
bool dualboy_egl_context_execute(struct dualboy_egl_context *context,
                                 unsigned slot,
                                 dualboy_egl_task_fn task,
                                 void *task_context,
                                 char *error,
                                 size_t error_size);

/* The caller must execute melonDS GL-object teardown on the worker before
 * stopping it. Partial construction and software-only contexts are safe. */
void dualboy_egl_context_destroy(struct dualboy_egl_context *context);

#ifdef __cplusplus
}
#endif

#endif
