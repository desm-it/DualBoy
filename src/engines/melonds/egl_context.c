/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "engines/melonds/egl_context.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <dlfcn.h>
#include <pthread.h>

#include "frontend/glad/glad.h"

#define DUALBOY_EGL_SLOT_COUNT 2U

typedef unsigned int dualboy_egl_boolean;
typedef unsigned int dualboy_egl_enum;
typedef int dualboy_egl_int;
typedef void *dualboy_egl_config;
typedef void *dualboy_egl_context_handle;
typedef void *dualboy_egl_display;
typedef void *dualboy_egl_surface;
typedef void (*dualboy_egl_proc)(void);

typedef dualboy_egl_display (*dualboy_egl_get_platform_display_fn)(
    dualboy_egl_enum platform,
    void *native,
    const intptr_t *attributes);
typedef dualboy_egl_display (*dualboy_egl_get_platform_display_ext_fn)(
    dualboy_egl_enum platform,
    void *native,
    const dualboy_egl_int *attributes);
typedef dualboy_egl_boolean (*dualboy_egl_initialize_fn)(
    dualboy_egl_display display,
    dualboy_egl_int *major,
    dualboy_egl_int *minor);
typedef dualboy_egl_boolean (*dualboy_egl_bind_api_fn)(dualboy_egl_enum api);
typedef dualboy_egl_boolean (*dualboy_egl_choose_config_fn)(
    dualboy_egl_display display,
    const dualboy_egl_int *attributes,
    dualboy_egl_config *configs,
    dualboy_egl_int capacity,
    dualboy_egl_int *count);
typedef dualboy_egl_surface (*dualboy_egl_create_pbuffer_surface_fn)(
    dualboy_egl_display display,
    dualboy_egl_config config,
    const dualboy_egl_int *attributes);
typedef dualboy_egl_boolean (*dualboy_egl_destroy_surface_fn)(
    dualboy_egl_display display,
    dualboy_egl_surface surface);
typedef dualboy_egl_context_handle (*dualboy_egl_create_context_fn)(
    dualboy_egl_display display,
    dualboy_egl_config config,
    dualboy_egl_context_handle shared,
    const dualboy_egl_int *attributes);
typedef dualboy_egl_boolean (*dualboy_egl_destroy_context_fn)(
    dualboy_egl_display display,
    dualboy_egl_context_handle context);
typedef dualboy_egl_boolean (*dualboy_egl_make_current_fn)(
    dualboy_egl_display display,
    dualboy_egl_surface draw,
    dualboy_egl_surface read,
    dualboy_egl_context_handle context);
typedef dualboy_egl_int (*dualboy_egl_get_error_fn)(void);
typedef dualboy_egl_proc (*dualboy_egl_get_proc_address_fn)(const char *name);
typedef dualboy_egl_boolean (*dualboy_egl_release_thread_fn)(void);

enum {
    DUALBOY_EGL_FALSE = 0,
    DUALBOY_EGL_NONE = 0x3038,
    DUALBOY_EGL_RED_SIZE = 0x3024,
    DUALBOY_EGL_GREEN_SIZE = 0x3023,
    DUALBOY_EGL_BLUE_SIZE = 0x3022,
    DUALBOY_EGL_ALPHA_SIZE = 0x3021,
    DUALBOY_EGL_SURFACE_TYPE = 0x3033,
    DUALBOY_EGL_PBUFFER_BIT = 0x0001,
    DUALBOY_EGL_RENDERABLE_TYPE = 0x3040,
    DUALBOY_EGL_OPENGL_BIT = 0x0008,
    DUALBOY_EGL_WIDTH = 0x3057,
    DUALBOY_EGL_HEIGHT = 0x3056,
    DUALBOY_EGL_OPENGL_API = 0x30a2,
    DUALBOY_EGL_CONTEXT_MAJOR_VERSION = 0x3098,
    DUALBOY_EGL_CONTEXT_MINOR_VERSION = 0x30fb,
    DUALBOY_EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30fd,
    DUALBOY_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x0001,
    DUALBOY_EGL_PLATFORM_SURFACELESS_MESA = 0x31dd,
};

struct dualboy_egl_implementation {
    void *library;
    dualboy_egl_display display;
    dualboy_egl_config config;
    dualboy_egl_surface surfaces[DUALBOY_EGL_SLOT_COUNT];
    dualboy_egl_context_handle contexts[DUALBOY_EGL_SLOT_COUNT];
    dualboy_egl_initialize_fn initialize;
    dualboy_egl_bind_api_fn bind_api;
    dualboy_egl_choose_config_fn choose_config;
    dualboy_egl_create_pbuffer_surface_fn create_pbuffer_surface;
    dualboy_egl_destroy_surface_fn destroy_surface;
    dualboy_egl_create_context_fn create_context;
    dualboy_egl_destroy_context_fn destroy_context;
    dualboy_egl_make_current_fn make_current;
    dualboy_egl_get_error_fn get_error;
    dualboy_egl_get_proc_address_fn get_proc_address;
    dualboy_egl_release_thread_fn release_thread;
    int current_slot;
};

struct dualboy_egl_worker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct dualboy_egl_implementation *egl;
    dualboy_egl_task_fn task;
    void *task_context;
    char *task_error;
    size_t task_error_size;
    char startup_error[512];
    unsigned task_slot;
    bool thread_started;
    bool startup_complete;
    bool startup_success;
    bool task_pending;
    bool task_complete;
    bool task_success;
    bool stopping;
};

static _Thread_local dualboy_egl_get_proc_address_fn
    loading_get_proc_address;

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) return;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool load_symbol(void *library,
                        const char *name,
                        void *destination,
                        size_t destination_size,
                        char *error,
                        size_t error_size)
{
    void *symbol;

    if (destination_size != sizeof(symbol)) {
        set_error(error, error_size,
                  "EGL function-pointer ABI is unsupported");
        return false;
    }
    (void)dlerror();
    symbol = dlsym(library, name);
    if (symbol == NULL) {
        const char *detail = dlerror();
        set_error(error, error_size, "libEGL is missing %s: %s", name,
                  detail != NULL ? detail : "unknown loader error");
        return false;
    }
    memcpy(destination, &symbol, destination_size);
    return true;
}

static dualboy_egl_display get_surfaceless_display(
    struct dualboy_egl_implementation *implementation)
{
    dualboy_egl_get_platform_display_fn core_function = NULL;
    dualboy_egl_get_platform_display_ext_fn extension_function = NULL;
    dualboy_egl_display display = NULL;
    void *symbol = dlsym(implementation->library, "eglGetPlatformDisplay");

    if (symbol != NULL && sizeof(core_function) == sizeof(symbol)) {
        memcpy(&core_function, &symbol, sizeof(core_function));
        display = core_function(DUALBOY_EGL_PLATFORM_SURFACELESS_MESA, NULL,
                                NULL);
        if (display != NULL) return display;
    }
    if (sizeof(extension_function) == sizeof(dualboy_egl_proc)) {
        const dualboy_egl_proc procedure =
            implementation->get_proc_address("eglGetPlatformDisplayEXT");
        memcpy(&extension_function, &procedure, sizeof(extension_function));
        if (extension_function != NULL) {
            return extension_function(
                DUALBOY_EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
        }
    }
    return NULL;
}

static void *load_gl_procedure(const char *name)
{
    dualboy_egl_proc procedure;
    void *address = NULL;

    if (loading_get_proc_address == NULL || name == NULL ||
        sizeof(address) != sizeof(procedure)) {
        return NULL;
    }
    procedure = loading_get_proc_address(name);
    memcpy(&address, &procedure, sizeof(address));
    return address;
}

static bool make_slot_current(struct dualboy_egl_implementation *implementation,
                              unsigned slot,
                              char *error,
                              size_t error_size)
{
    if (implementation == NULL || slot >= DUALBOY_EGL_SLOT_COUNT ||
        implementation->contexts[slot] == NULL ||
        implementation->surfaces[slot] == NULL) {
        set_error(error, error_size, "invalid offscreen OpenGL context slot");
        return false;
    }
    if (implementation->make_current(
            implementation->display, implementation->surfaces[slot],
            implementation->surfaces[slot], implementation->contexts[slot]) ==
        DUALBOY_EGL_FALSE) {
        set_error(error, error_size,
                  "could not select offscreen OpenGL context %u (0x%04x)",
                  slot + 1U, (unsigned)implementation->get_error());
        return false;
    }
    implementation->current_slot = (int)slot;
    return true;
}

static bool release_current(struct dualboy_egl_implementation *implementation,
                            char *error,
                            size_t error_size)
{
    if (implementation == NULL || implementation->display == NULL ||
        implementation->make_current == NULL) {
        set_error(error, error_size, "invalid offscreen OpenGL context release");
        return false;
    }
    if (implementation->make_current(implementation->display, NULL, NULL,
                                     NULL) == DUALBOY_EGL_FALSE) {
        set_error(error, error_size,
                  "could not release offscreen OpenGL context (0x%04x)",
                  (unsigned)implementation->get_error());
        return false;
    }
    implementation->current_slot = -1;
    return true;
}

static void release_implementation(
    struct dualboy_egl_implementation *implementation)
{
    unsigned slot;

    if (implementation == NULL) return;
    if (implementation->display != NULL && implementation->make_current != NULL) {
        (void)implementation->make_current(implementation->display, NULL, NULL,
                                           NULL);
        implementation->current_slot = -1;
    }
    for (slot = 0U; slot < DUALBOY_EGL_SLOT_COUNT; ++slot) {
        if (implementation->display != NULL &&
            implementation->contexts[slot] != NULL &&
            implementation->destroy_context != NULL) {
            (void)implementation->destroy_context(
                implementation->display, implementation->contexts[slot]);
            implementation->contexts[slot] = NULL;
        }
        if (implementation->display != NULL &&
            implementation->surfaces[slot] != NULL &&
            implementation->destroy_surface != NULL) {
            (void)implementation->destroy_surface(
                implementation->display, implementation->surfaces[slot]);
            implementation->surfaces[slot] = NULL;
        }
    }
    /* EGL 1.5 returns the same EGLDisplay for identical platform, native
     * display, and attribute triples. Treat this surfaceless display as a
     * process-borrowed handle: terminating it could invalidate contexts owned
     * by a frontend or another module. All resources created above are still
     * explicitly destroyed. */
    if (implementation->release_thread != NULL) {
        (void)implementation->release_thread();
    }
    if (implementation->library != NULL) {
        (void)dlclose(implementation->library);
        implementation->library = NULL;
    }
    free(implementation);
}

static struct dualboy_egl_implementation *create_implementation(
    char *error,
    size_t error_size)
{
    static const char *const library_names[] = {"libEGL.so.1", "libEGL.so"};
    static const dualboy_egl_int config_attributes[] = {
        DUALBOY_EGL_SURFACE_TYPE, DUALBOY_EGL_PBUFFER_BIT,
        DUALBOY_EGL_RENDERABLE_TYPE, DUALBOY_EGL_OPENGL_BIT,
        DUALBOY_EGL_RED_SIZE, 8, DUALBOY_EGL_GREEN_SIZE, 8,
        DUALBOY_EGL_BLUE_SIZE, 8, DUALBOY_EGL_ALPHA_SIZE, 8,
        DUALBOY_EGL_NONE,
    };
    static const dualboy_egl_int fallback_config_attributes[] = {
        DUALBOY_EGL_SURFACE_TYPE, DUALBOY_EGL_PBUFFER_BIT,
        DUALBOY_EGL_RENDERABLE_TYPE, DUALBOY_EGL_OPENGL_BIT,
        DUALBOY_EGL_NONE,
    };
    static const dualboy_egl_int surface_attributes[] = {
        DUALBOY_EGL_WIDTH, 1, DUALBOY_EGL_HEIGHT, 1, DUALBOY_EGL_NONE,
    };
    static const dualboy_egl_int context_attributes[] = {
        DUALBOY_EGL_CONTEXT_MAJOR_VERSION, 3,
        DUALBOY_EGL_CONTEXT_MINOR_VERSION, 2,
        DUALBOY_EGL_CONTEXT_OPENGL_PROFILE_MASK,
        DUALBOY_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        DUALBOY_EGL_NONE,
    };
    struct dualboy_egl_implementation *implementation;
    dualboy_egl_int config_count = 0;
    dualboy_egl_int egl_major = 0;
    dualboy_egl_int egl_minor = 0;
    PFNGLGETSTRINGPROC first_get_string = NULL;
    PFNGLGENBUFFERSPROC first_gen_buffers = NULL;
    PFNGLBINDFRAMEBUFFERPROC first_bind_framebuffer = NULL;
    size_t library_index;
    unsigned slot;

    implementation = (struct dualboy_egl_implementation *)calloc(
        1U, sizeof(*implementation));
    if (implementation == NULL) {
        set_error(error, error_size, "could not allocate EGL context state");
        return NULL;
    }
    implementation->current_slot = -1;
    for (library_index = 0U;
         library_index < sizeof(library_names) / sizeof(library_names[0]);
         ++library_index) {
        /* A borrowed, initialized EGLDisplay can outlive this context pair.
         * Keep libEGL's process-wide display registry resident after dlclose;
         * unloading it would invalidate the borrowed handle just as surely as
         * eglTerminate would. */
        implementation->library = dlopen(
            library_names[library_index],
            RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (implementation->library != NULL) break;
    }
    if (implementation->library == NULL) {
        set_error(error, error_size,
                  "OpenGL renderer requires the libEGL.so.1 runtime");
        release_implementation(implementation);
        return NULL;
    }

#define DUALBOY_LOAD_EGL(member, symbol_name)                              \
    if (!load_symbol(implementation->library, symbol_name,                  \
                     &implementation->member, sizeof(implementation->member), \
                     error, error_size)) {                                 \
        release_implementation(implementation);                            \
        return NULL;                                                        \
    }
    DUALBOY_LOAD_EGL(initialize, "eglInitialize")
    DUALBOY_LOAD_EGL(bind_api, "eglBindAPI")
    DUALBOY_LOAD_EGL(choose_config, "eglChooseConfig")
    DUALBOY_LOAD_EGL(create_pbuffer_surface, "eglCreatePbufferSurface")
    DUALBOY_LOAD_EGL(destroy_surface, "eglDestroySurface")
    DUALBOY_LOAD_EGL(create_context, "eglCreateContext")
    DUALBOY_LOAD_EGL(destroy_context, "eglDestroyContext")
    DUALBOY_LOAD_EGL(make_current, "eglMakeCurrent")
    DUALBOY_LOAD_EGL(get_error, "eglGetError")
    DUALBOY_LOAD_EGL(get_proc_address, "eglGetProcAddress")
    DUALBOY_LOAD_EGL(release_thread, "eglReleaseThread")
#undef DUALBOY_LOAD_EGL

    implementation->display = get_surfaceless_display(implementation);
    if (implementation->display == NULL ||
        implementation->initialize(implementation->display, &egl_major,
                                   &egl_minor) == DUALBOY_EGL_FALSE) {
        set_error(error, error_size,
                  "EGL surfaceless platform is unavailable (0x%04x)",
                  (unsigned)implementation->get_error());
        release_implementation(implementation);
        return NULL;
    }
    if (implementation->bind_api(DUALBOY_EGL_OPENGL_API) ==
        DUALBOY_EGL_FALSE) {
        set_error(error, error_size, "EGL cannot bind desktop OpenGL (0x%04x)",
                  (unsigned)implementation->get_error());
        release_implementation(implementation);
        return NULL;
    }
    if (implementation->choose_config(
            implementation->display, config_attributes,
            &implementation->config, 1, &config_count) == DUALBOY_EGL_FALSE ||
        config_count != 1) {
        /* The renderer uses its own FBOs, so a headless driver need not
         * provide every window-system color channel on the tiny pbuffer. */
        config_count = 0;
        implementation->config = NULL;
        if (implementation->choose_config(
                implementation->display, fallback_config_attributes,
                &implementation->config, 1, &config_count) ==
                DUALBOY_EGL_FALSE ||
            config_count != 1) {
            set_error(error, error_size,
                      "EGL has no OpenGL pbuffer configuration (0x%04x)",
                      (unsigned)implementation->get_error());
            release_implementation(implementation);
            return NULL;
        }
    }

    /* The two contexts intentionally pass EGL_NO_CONTEXT as their share
     * argument. They therefore have disjoint object-name namespaces even
     * though one adapter worker serializes all access to their EGLDisplay. */
    for (slot = 0U; slot < DUALBOY_EGL_SLOT_COUNT; ++slot) {
        implementation->surfaces[slot] =
            implementation->create_pbuffer_surface(
                implementation->display, implementation->config,
                surface_attributes);
        if (implementation->surfaces[slot] == NULL) {
            set_error(error, error_size,
                      "could not create EGL pbuffer %u (0x%04x)", slot + 1U,
                      (unsigned)implementation->get_error());
            release_implementation(implementation);
            return NULL;
        }
        implementation->contexts[slot] = implementation->create_context(
            implementation->display, implementation->config, NULL,
            context_attributes);
        if (implementation->contexts[slot] == NULL ||
            !make_slot_current(implementation, slot, error, error_size)) {
            if (implementation->contexts[slot] == NULL) {
                set_error(error, error_size,
                          "could not create OpenGL 3.2 context %u (0x%04x)",
                          slot + 1U,
                          (unsigned)implementation->get_error());
            }
            release_implementation(implementation);
            return NULL;
        }
        loading_get_proc_address = implementation->get_proc_address;
        if (gladLoadGLLoader(load_gl_procedure) == 0 ||
            !GLAD_GL_VERSION_3_2) {
            loading_get_proc_address = NULL;
            set_error(error, error_size,
                      "offscreen context %u does not provide OpenGL 3.2",
                      slot + 1U);
            release_implementation(implementation);
            return NULL;
        }
        loading_get_proc_address = NULL;
        if (slot == 0U) {
            first_get_string = glad_glGetString;
            first_gen_buffers = glad_glGenBuffers;
            first_bind_framebuffer = glad_glBindFramebuffer;
        } else if (glad_glGetString != first_get_string ||
                   glad_glGenBuffers != first_gen_buffers ||
                   glad_glBindFramebuffer != first_bind_framebuffer) {
            /* GLAD has process-global dispatch variables. Both contexts are
             * created serially on this one worker and must resolve through the
             * same driver before the adapter may publish either renderer. */
            set_error(error, error_size,
                      "offscreen OpenGL contexts use incompatible dispatch tables");
            release_implementation(implementation);
            return NULL;
        }
        if (!release_current(implementation, error, error_size)) {
            release_implementation(implementation);
            return NULL;
        }
    }
    return implementation;
}

static void *egl_worker_main(void *opaque_worker)
{
    struct dualboy_egl_worker *worker =
        (struct dualboy_egl_worker *)opaque_worker;

    worker->egl = create_implementation(worker->startup_error,
                                        sizeof(worker->startup_error));
    (void)pthread_mutex_lock(&worker->mutex);
    worker->startup_success = worker->egl != NULL;
    worker->startup_complete = true;
    (void)pthread_cond_broadcast(&worker->condition);
    if (worker->egl == NULL) {
        (void)pthread_mutex_unlock(&worker->mutex);
        return NULL;
    }

    while (!worker->stopping) {
        dualboy_egl_task_fn task;
        void *task_context;
        char *task_error;
        size_t task_error_size;
        unsigned task_slot;
        bool success;

        while (!worker->task_pending && !worker->stopping) {
            (void)pthread_cond_wait(&worker->condition, &worker->mutex);
        }
        if (worker->stopping) break;
        task = worker->task;
        task_context = worker->task_context;
        task_error = worker->task_error;
        task_error_size = worker->task_error_size;
        task_slot = worker->task_slot;
        (void)pthread_mutex_unlock(&worker->mutex);

        success = make_slot_current(worker->egl, task_slot, task_error,
                                    task_error_size) &&
                  task != NULL &&
                  task(task_context, task_error, task_error_size);
        /* Leave no context current between task boundaries. Every operation
         * therefore proves its slot affinity with a fresh eglMakeCurrent,
         * including reset and teardown after an earlier failure. */
        if (!release_current(worker->egl, task_error, task_error_size)) {
            success = false;
        }

        (void)pthread_mutex_lock(&worker->mutex);
        worker->task_success = success;
        worker->task_pending = false;
        worker->task_complete = true;
        (void)pthread_cond_broadcast(&worker->condition);
    }
    (void)pthread_mutex_unlock(&worker->mutex);
    release_implementation(worker->egl);
    worker->egl = NULL;
    return NULL;
}

bool dualboy_egl_context_create(struct dualboy_egl_context *context,
                                char *error,
                                size_t error_size)
{
    struct dualboy_egl_worker *worker;
    int result;

    if (context == NULL || context->implementation != NULL) {
        set_error(error, error_size, "invalid EGL context lifecycle");
        return false;
    }
    worker = (struct dualboy_egl_worker *)calloc(1U, sizeof(*worker));
    if (worker == NULL) {
        set_error(error, error_size, "could not allocate EGL worker state");
        return false;
    }
    result = pthread_mutex_init(&worker->mutex, NULL);
    if (result != 0) {
        set_error(error, error_size, "could not initialize EGL worker mutex");
        free(worker);
        return false;
    }
    result = pthread_cond_init(&worker->condition, NULL);
    if (result != 0) {
        set_error(error, error_size,
                  "could not initialize EGL worker condition");
        (void)pthread_mutex_destroy(&worker->mutex);
        free(worker);
        return false;
    }
    result = pthread_create(&worker->thread, NULL, egl_worker_main, worker);
    if (result != 0) {
        set_error(error, error_size, "could not start the EGL worker thread");
        (void)pthread_cond_destroy(&worker->condition);
        (void)pthread_mutex_destroy(&worker->mutex);
        free(worker);
        return false;
    }
    worker->thread_started = true;
    (void)pthread_mutex_lock(&worker->mutex);
    while (!worker->startup_complete) {
        (void)pthread_cond_wait(&worker->condition, &worker->mutex);
    }
    (void)pthread_mutex_unlock(&worker->mutex);
    if (!worker->startup_success) {
        (void)pthread_join(worker->thread, NULL);
        set_error(error, error_size, "%s",
                  worker->startup_error[0] != '\0'
                      ? worker->startup_error
                      : "offscreen EGL initialization failed");
        (void)pthread_cond_destroy(&worker->condition);
        (void)pthread_mutex_destroy(&worker->mutex);
        free(worker);
        return false;
    }
    context->implementation = worker;
    return true;
}

bool dualboy_egl_context_execute(struct dualboy_egl_context *context,
                                 unsigned slot,
                                 dualboy_egl_task_fn task,
                                 void *task_context,
                                 char *error,
                                 size_t error_size)
{
    struct dualboy_egl_worker *worker;
    bool success;

    if (context == NULL || context->implementation == NULL || task == NULL ||
        slot >= DUALBOY_EGL_SLOT_COUNT) {
        set_error(error, error_size, "offscreen OpenGL worker is not ready");
        return false;
    }
    worker = (struct dualboy_egl_worker *)context->implementation;
    (void)pthread_mutex_lock(&worker->mutex);
    if (!worker->startup_success || worker->stopping ||
        worker->task_pending) {
        (void)pthread_mutex_unlock(&worker->mutex);
        set_error(error, error_size, "offscreen OpenGL worker is unavailable");
        return false;
    }
    worker->task = task;
    worker->task_context = task_context;
    worker->task_error = error;
    worker->task_error_size = error_size;
    worker->task_slot = slot;
    worker->task_complete = false;
    worker->task_pending = true;
    (void)pthread_cond_broadcast(&worker->condition);
    while (!worker->task_complete) {
        (void)pthread_cond_wait(&worker->condition, &worker->mutex);
    }
    success = worker->task_success;
    worker->task = NULL;
    worker->task_context = NULL;
    worker->task_error = NULL;
    worker->task_error_size = 0U;
    (void)pthread_mutex_unlock(&worker->mutex);
    return success;
}

void dualboy_egl_context_destroy(struct dualboy_egl_context *context)
{
    struct dualboy_egl_worker *worker;

    if (context == NULL) return;
    worker = (struct dualboy_egl_worker *)context->implementation;
    context->implementation = NULL;
    if (worker == NULL) return;
    (void)pthread_mutex_lock(&worker->mutex);
    worker->stopping = true;
    (void)pthread_cond_broadcast(&worker->condition);
    (void)pthread_mutex_unlock(&worker->mutex);
    if (worker->thread_started) {
        (void)pthread_join(worker->thread, NULL);
    }
    (void)pthread_cond_destroy(&worker->condition);
    (void)pthread_mutex_destroy(&worker->mutex);
    free(worker);
}

#else

static void unsupported(char *error, size_t error_size)
{
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size,
                       "offscreen EGL rendering is unavailable on this platform");
    }
}

bool dualboy_egl_context_create(struct dualboy_egl_context *context,
                                char *error,
                                size_t error_size)
{
    (void)context;
    unsupported(error, error_size);
    return false;
}

bool dualboy_egl_context_execute(struct dualboy_egl_context *context,
                                 unsigned slot,
                                 dualboy_egl_task_fn task,
                                 void *task_context,
                                 char *error,
                                 size_t error_size)
{
    (void)context;
    (void)slot;
    (void)task;
    (void)task_context;
    unsupported(error, error_size);
    return false;
}

void dualboy_egl_context_destroy(struct dualboy_egl_context *context)
{
    if (context != NULL) context->implementation = NULL;
}

#endif
