/*
 * entry.c - Library entry point: constructor, worker thread, shared state.
 *
 * The constructor fires on dlopen and hands off to a worker thread so the
 * injector isn't blocked inside dlopen while we talk to the JVM.
 */

#include "payload.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

JavaVM  *g_jvm = NULL;
JNIEnv  *g_env = NULL;
jobject  g_isolated_loader = NULL;
jclass   g_bootstrap_class = NULL;
void    *g_libjvm_handle = NULL;
volatile int g_initialized = 0;
volatile int g_should_unload = 0;

JNI_GetCreatedJavaVMs_t JNI_GetCreatedJavaVMs_func = NULL;

/*
 * Worker thread - does the actual initialization
 * We use a separate thread to avoid blocking the injector
 */
static void *worker_thread(void *arg) {
    (void)arg;

    printf("[payload] Worker thread started\n");

    /* Small delay to let injection complete */
    usleep(100000);  /* 100ms */

    if (find_jvm() != 0) {
        fprintf(stderr, "[payload] Failed to find JVM\n");
        return NULL;
    }

    if (attach_to_jvm() != 0) {
        fprintf(stderr, "[payload] Failed to attach to JVM\n");
        return NULL;
    }

    g_isolated_loader = create_isolated_classloader(g_env);
    if (!g_isolated_loader) {
        fprintf(stderr, "[payload] Failed to create isolated ClassLoader\n");
        return NULL;
    }

    if (bootstrap_java() != 0) {
        fprintf(stderr, "[payload] Failed to bootstrap Java\n");
        payload_unload();
        return NULL;
    }

    /* Promote g_bootstrap_class from local ref to global ref BEFORE
     * detaching.  DetachCurrentThread frees all local refs on this thread,
     * which would turn g_bootstrap_class into a dangling pointer. */
    g_bootstrap_class = (jclass)(*g_env)->NewGlobalRef(g_env, g_bootstrap_class);

    g_initialized = 1;
    printf("[payload] Initialization complete!\n");

    /* Detach this thread from the JVM.  We're done with JNI calls.
     * Without this, each inject/uninject cycle leaks a JVM thread
     * slot, eventually causing AttachCurrentThread to fail. */
    (*g_jvm)->DetachCurrentThread(g_jvm);
    g_env = NULL;

    return NULL;
}

/*
 * Constructor - called when .so is loaded
 */
__attribute__((constructor))
static void on_load(void) {
    /* Force line-buffered stdout so printf output is visible immediately,
     * even when stdout isn't a terminal (e.g. piped through launcher). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("[payload] Loaded into process %d\n", getpid());

    /* Start worker thread */
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker_thread, NULL) != 0) {
        fprintf(stderr, "[payload] Failed to create worker thread\n");
        return;
    }

    pthread_detach(thread);
}

/* No destructor - we bypass dlclose entirely and use direct munmap,
 * so the __attribute__((destructor)) would never fire anyway.
 * Having one is dangerous: if anything accidentally calls dlclose,
 * it would re-enter payload_unload and crash in _dl_catch_exception. */
