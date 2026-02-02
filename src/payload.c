/*
 * payload.c - Injected shared library that bootstraps Java code via JNI
 *
 * Compile: gcc -shared -fPIC -o payload.so payload.c -I$JAVA_HOME/include -I$JAVA_HOME/include/linux -ldl -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <jni.h>

/* Global state */
static JavaVM *g_jvm = NULL;
static JNIEnv *g_env = NULL;
static jobject g_isolated_loader = NULL;
static jclass g_bootstrap_class = NULL;
static void *g_libjvm_handle = NULL;
static volatile int g_initialized = 0;
static volatile int g_should_unload = 0;

/* JNI function pointers */
typedef jint (JNICALL *JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);
static JNI_GetCreatedJavaVMs_t JNI_GetCreatedJavaVMs_func = NULL;

/* Forward declarations */
static int find_jvm(void);
static int attach_to_jvm(void);
static jobject create_isolated_classloader(JNIEnv *env);
static jclass define_class_in_loader(JNIEnv *env, jobject loader, const char *name, const unsigned char *bytecode, size_t len);
static int bootstrap_java(void);
static void *worker_thread(void *arg);
static void schedule_self_unload(void);
JNIEXPORT void JNICALL Java_client_Bootstrap_nativeUnload(JNIEnv *env, jclass cls);

/*
 * Find libjvm.so and get JNI_GetCreatedJavaVMs
 */
static int find_jvm(void) {
    /* Try various libjvm paths */
    const char *jvm_paths[] = {
        NULL,  /* Already loaded in process */
        "libjvm.so",
        "/usr/lib/jvm/java-8-openjdk-amd64/jre/lib/amd64/server/libjvm.so",
        "/usr/lib/jvm/java-11-openjdk-amd64/lib/server/libjvm.so",
        "/usr/lib/jvm/java-17-openjdk-amd64/lib/server/libjvm.so",
        "/usr/lib/jvm/java-21-openjdk-amd64/lib/server/libjvm.so",
        NULL
    };

    /* First try to find it already loaded */
    g_libjvm_handle = dlopen(NULL, RTLD_NOW);
    JNI_GetCreatedJavaVMs_func = (JNI_GetCreatedJavaVMs_t)dlsym(g_libjvm_handle, "JNI_GetCreatedJavaVMs");

    if (JNI_GetCreatedJavaVMs_func) {
        printf("[payload] Found JNI_GetCreatedJavaVMs in main process\n");
        return 0;
    }

    /* Try explicit paths */
    for (int i = 1; jvm_paths[i]; i++) {
        g_libjvm_handle = dlopen(jvm_paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_libjvm_handle) {
            JNI_GetCreatedJavaVMs_func = (JNI_GetCreatedJavaVMs_t)dlsym(g_libjvm_handle, "JNI_GetCreatedJavaVMs");
            if (JNI_GetCreatedJavaVMs_func) {
                printf("[payload] Found JVM at %s\n", jvm_paths[i]);
                return 0;
            }
            dlclose(g_libjvm_handle);
        }
    }

    /* Search in /proc/self/maps for libjvm.so */
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libjvm.so")) {
                char *path = strchr(line, '/');
                if (path) {
                    char *nl = strchr(path, '\n');
                    if (nl) *nl = '\0';

                    g_libjvm_handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
                    if (g_libjvm_handle) {
                        JNI_GetCreatedJavaVMs_func = (JNI_GetCreatedJavaVMs_t)dlsym(g_libjvm_handle, "JNI_GetCreatedJavaVMs");
                        if (JNI_GetCreatedJavaVMs_func) {
                            printf("[payload] Found JVM at %s\n", path);
                            fclose(maps);
                            return 0;
                        }
                        dlclose(g_libjvm_handle);
                    }
                }
            }
        }
        fclose(maps);
    }

    fprintf(stderr, "[payload] Could not find JNI_GetCreatedJavaVMs\n");
    return -1;
}

/*
 * Attach to the running JVM
 */
static int attach_to_jvm(void) {
    jsize num_vms = 0;
    jint res;

    /* Get the JVM */
    res = JNI_GetCreatedJavaVMs_func(&g_jvm, 1, &num_vms);
    if (res != JNI_OK || num_vms == 0) {
        fprintf(stderr, "[payload] No JVM found (res=%d, num=%d)\n", res, num_vms);
        return -1;
    }

    printf("[payload] Found %d JVM(s)\n", num_vms);

    /* Attach current thread to JVM */
    res = (*g_jvm)->AttachCurrentThread(g_jvm, (void**)&g_env, NULL);
    if (res != JNI_OK) {
        fprintf(stderr, "[payload] Failed to attach to JVM: %d\n", res);
        return -1;
    }

    printf("[payload] Attached to JVM successfully\n");
    return 0;
}

/*
 * Create an isolated URLClassLoader that can be GC'd
 */
static jobject create_isolated_classloader(JNIEnv *env) {
    /* Get system class loader as parent */
    jclass cl_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    if (!cl_class) {
        fprintf(stderr, "[payload] Could not find ClassLoader class\n");
        return NULL;
    }

    jmethodID get_sys_cl = (*env)->GetStaticMethodID(env, cl_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (!get_sys_cl) {
        fprintf(stderr, "[payload] Could not find getSystemClassLoader\n");
        return NULL;
    }

    jobject sys_loader = (*env)->CallStaticObjectMethod(env, cl_class, get_sys_cl);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return NULL;
    }

    /* Create URLClassLoader with empty URL array */
    jclass url_cl_class = (*env)->FindClass(env, "java/net/URLClassLoader");
    if (!url_cl_class) {
        fprintf(stderr, "[payload] Could not find URLClassLoader class\n");
        return NULL;
    }

    jclass url_class = (*env)->FindClass(env, "java/net/URL");
    if (!url_class) {
        fprintf(stderr, "[payload] Could not find URL class\n");
        return NULL;
    }

    /* Create empty URL[] */
    jobjectArray urls = (*env)->NewObjectArray(env, 0, url_class, NULL);
    if (!urls) {
        fprintf(stderr, "[payload] Could not create URL array\n");
        return NULL;
    }

    /* URLClassLoader(URL[] urls, ClassLoader parent) */
    jmethodID url_cl_init = (*env)->GetMethodID(env, url_cl_class, "<init>", "([Ljava/net/URL;Ljava/lang/ClassLoader;)V");
    if (!url_cl_init) {
        fprintf(stderr, "[payload] Could not find URLClassLoader constructor\n");
        return NULL;
    }

    jobject loader = (*env)->NewObject(env, url_cl_class, url_cl_init, urls, sys_loader);
    if (!loader || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        fprintf(stderr, "[payload] Could not create URLClassLoader\n");
        return NULL;
    }

    /* Create global reference so it persists */
    jobject global_loader = (*env)->NewGlobalRef(env, loader);
    (*env)->DeleteLocalRef(env, loader);

    printf("[payload] Created isolated ClassLoader\n");
    return global_loader;
}

/*
 * Define a class in the isolated ClassLoader using Unsafe
 */
static jclass define_class_in_loader(JNIEnv *env, jobject loader, const char *name, const unsigned char *bytecode, size_t len) {
    jclass result = NULL;

    /* Method 1: Try using ClassLoader.defineClass via reflection */
    jclass cl_class = (*env)->GetObjectClass(env, loader);

    /* Get the protected defineClass method */
    jmethodID define_class = (*env)->GetMethodID(env, cl_class, "defineClass", "(Ljava/lang/String;[BII)Ljava/lang/Class;");

    if (!define_class) {
        /* Try getting it from superclass */
        jclass super = (*env)->GetSuperclass(env, cl_class);
        while (super && !define_class) {
            define_class = (*env)->GetMethodID(env, super, "defineClass", "(Ljava/lang/String;[BII)Ljava/lang/Class;");
            if (!define_class) {
                (*env)->ExceptionClear(env);
            }
            jclass next = (*env)->GetSuperclass(env, super);
            (*env)->DeleteLocalRef(env, super);
            super = next;
        }
        if (super) (*env)->DeleteLocalRef(env, super);
    }

    if (!define_class) {
        (*env)->ExceptionClear(env);

        /* Method 2: Use Unsafe.defineClass */
        printf("[payload] Using Unsafe.defineClass\n");

        jclass unsafe_class = (*env)->FindClass(env, "sun/misc/Unsafe");
        if (!unsafe_class) {
            (*env)->ExceptionClear(env);
            unsafe_class = (*env)->FindClass(env, "jdk/internal/misc/Unsafe");
        }

        if (!unsafe_class) {
            fprintf(stderr, "[payload] Could not find Unsafe class\n");
            return NULL;
        }

        /* Get theUnsafe field */
        jfieldID the_unsafe = (*env)->GetStaticFieldID(env, unsafe_class, "theUnsafe", "Lsun/misc/Unsafe;");
        if (!the_unsafe) {
            (*env)->ExceptionClear(env);
            the_unsafe = (*env)->GetStaticFieldID(env, unsafe_class, "theUnsafe", "Ljdk/internal/misc/Unsafe;");
        }

        if (!the_unsafe) {
            fprintf(stderr, "[payload] Could not find theUnsafe field\n");
            return NULL;
        }

        jobject unsafe = (*env)->GetStaticObjectField(env, unsafe_class, the_unsafe);
        if (!unsafe) {
            fprintf(stderr, "[payload] Could not get Unsafe instance\n");
            return NULL;
        }

        /* Unsafe.defineClass(String name, byte[] b, int off, int len, ClassLoader loader, ProtectionDomain pd) */
        jmethodID unsafe_define = (*env)->GetMethodID(env, unsafe_class, "defineClass",
            "(Ljava/lang/String;[BIILjava/lang/ClassLoader;Ljava/security/ProtectionDomain;)Ljava/lang/Class;");

        if (!unsafe_define) {
            (*env)->ExceptionDescribe(env);
            fprintf(stderr, "[payload] Could not find Unsafe.defineClass\n");
            return NULL;
        }

        jstring jname = (*env)->NewStringUTF(env, name);
        jbyteArray jbytes = (*env)->NewByteArray(env, len);
        (*env)->SetByteArrayRegion(env, jbytes, 0, len, (jbyte*)bytecode);

        result = (jclass)(*env)->CallObjectMethod(env, unsafe, unsafe_define,
            jname, jbytes, 0, (jint)len, loader, NULL);

        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            result = NULL;
        }

        (*env)->DeleteLocalRef(env, jname);
        (*env)->DeleteLocalRef(env, jbytes);
        (*env)->DeleteLocalRef(env, unsafe);
        (*env)->DeleteLocalRef(env, unsafe_class);

    } else {
        /* Use defineClass directly */
        jstring jname = (*env)->NewStringUTF(env, name);
        jbyteArray jbytes = (*env)->NewByteArray(env, len);
        (*env)->SetByteArrayRegion(env, jbytes, 0, len, (jbyte*)bytecode);

        result = (jclass)(*env)->CallObjectMethod(env, loader, define_class,
            jname, jbytes, 0, (jint)len);

        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            result = NULL;
        }

        (*env)->DeleteLocalRef(env, jname);
        (*env)->DeleteLocalRef(env, jbytes);
    }

    (*env)->DeleteLocalRef(env, cl_class);
    return result;
}

/*
 * Read class file from disk
 */
static unsigned char* read_class_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *buf = malloc(len);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_len = len;
    return buf;
}

/*
 * Load a class file from disk and define it in the isolated classloader.
 * Searches the given NULL-terminated path list.
 * Returns the jclass (caller must DeleteLocalRef) or NULL on failure.
 */
static jclass load_and_define(const char *name, const char **paths) {
    unsigned char *bytecode = NULL;
    size_t bytecode_len = 0;

    for (int i = 0; paths[i]; i++) {
        bytecode = read_class_file(paths[i], &bytecode_len);
        if (bytecode) {
            printf("[payload] Loaded %s.class from %s\n", name, paths[i]);
            break;
        }
    }

    if (!bytecode) {
        fprintf(stderr, "[payload] Could not find %s.class\n", name);
        return NULL;
    }

    jclass cls = define_class_in_loader(g_env, g_isolated_loader,
        name, bytecode, bytecode_len);
    free(bytecode);

    if (!cls) {
        fprintf(stderr, "[payload] Failed to define %s class\n", name);
        return NULL;
    }

    printf("[payload] Defined %s class\n", name);
    return cls;
}

/*
 * Bootstrap the Java code - load all classes in dependency order, then init.
 */
static int bootstrap_java(void) {
    jclass cls;

    /* Class loading order matters: each class must be defined before
     * any class that references it at the bytecode level. */

    /*
     * Load classes in dependency order.
     * Package classes go in /tmp/client/ and /tmp/client/rendering/.
     * RenderHook stays in default package (/tmp/).
     */

    /* --- client.rendering package (no cross-deps first) --- */

    const char *simplegui_paths[] = {
        "/tmp/client/rendering/SimpleGui.class",
        "./client/rendering/SimpleGui.class", NULL};
    cls = load_and_define("client.rendering.SimpleGui", simplegui_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *renderable_paths[] = {
        "/tmp/client/rendering/Renderable.class",
        "./client/rendering/Renderable.class", NULL};
    cls = load_and_define("client.rendering.Renderable", renderable_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    /* --- client package --- */

    const char *mcrefl_paths[] = {
        "/tmp/client/McReflection.class",
        "./client/McReflection.class", NULL};
    cls = load_and_define("client.McReflection", mcrefl_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *restore_paths[] = {
        "/tmp/client/RestoreManager.class",
        "./client/RestoreManager.class", NULL};
    cls = load_and_define("client.RestoreManager", restore_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *restore_action_paths[] = {
        "/tmp/client/RestoreManager$RestoreAction.class",
        "./client/RestoreManager$RestoreAction.class", NULL};
    cls = load_and_define("client.RestoreManager$RestoreAction", restore_action_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *input_paths[] = {
        "/tmp/client/InputManager.class",
        "./client/InputManager.class", NULL};
    cls = load_and_define("client.InputManager", input_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *keybind_paths[] = {
        "/tmp/client/InputManager$Keybind.class",
        "./client/InputManager$Keybind.class", NULL};
    cls = load_and_define("client.InputManager$Keybind", keybind_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *chat_paths[] = {
        "/tmp/client/ChatUtil.class",
        "./client/ChatUtil.class", NULL};
    cls = load_and_define("client.ChatUtil", chat_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    /* --- client.rendering (depends on client) --- */

    const char *renderhandler_paths[] = {
        "/tmp/client/rendering/RenderHandler.class",
        "./client/rendering/RenderHandler.class", NULL};
    cls = load_and_define("client.rendering.RenderHandler", renderhandler_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    const char *mainoverlay_paths[] = {
        "/tmp/client/rendering/MainOverlay.class",
        "./client/rendering/MainOverlay.class", NULL};
    cls = load_and_define("client.rendering.MainOverlay", mainoverlay_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    /* --- RenderHook (default package, extends MC stubs) --- */

    const char *hook_paths[] = {"/tmp/RenderHook.class", "./RenderHook.class", NULL};
    cls = load_and_define("RenderHook", hook_paths);
    if (!cls) return -1;
    (*g_env)->DeleteLocalRef(g_env, cls);

    /* --- client.Bootstrap (references everything) --- */

    const char *boot_paths[] = {
        "/tmp/client/Bootstrap.class",
        "./client/Bootstrap.class", NULL};
    g_bootstrap_class = load_and_define("client.Bootstrap", boot_paths);
    if (!g_bootstrap_class) return -1;

    /* Register native methods */
    {
        JNINativeMethod methods[] = {
            {"nativeUnload", "()V", (void *)Java_client_Bootstrap_nativeUnload}
        };
        jint reg_res = (*g_env)->RegisterNatives(g_env, g_bootstrap_class, methods, 1);
        if (reg_res != JNI_OK) {
            fprintf(stderr, "[payload] Failed to register native methods: %d\n", reg_res);
        } else {
            printf("[payload] Registered native methods\n");
        }
    }

    /* Call Bootstrap.init() */
    jmethodID init_method = (*g_env)->GetStaticMethodID(g_env, g_bootstrap_class, "init", "()V");
    if (!init_method) {
        fprintf(stderr, "[payload] Could not find Bootstrap.init()\n");
        return -1;
    }

    (*g_env)->CallStaticVoidMethod(g_env, g_bootstrap_class, init_method);

    if ((*g_env)->ExceptionCheck(g_env)) {
        (*g_env)->ExceptionDescribe(g_env);
        (*g_env)->ExceptionClear(g_env);
        fprintf(stderr, "[payload] Bootstrap.init() threw exception\n");
        return -1;
    }

    printf("[payload] Bootstrap.init() completed\n");
    return 0;
}

/*
 * Cleanup and prepare for unload
 */
void payload_unload(void) {
    JNIEnv *env = NULL;

    printf("[payload] Unloading...\n");

    if (!g_jvm) {
        printf("[payload] No JVM reference, nothing to clean up\n");
        g_initialized = 0;
        return;
    }

    /* Attach THIS thread to the JVM to get a valid JNIEnv.
     * g_env belongs to the original worker thread and CANNOT be used
     * from a different thread - JNIEnv is thread-local in JNI. */
    jint res = (*g_jvm)->AttachCurrentThread(g_jvm, (void**)&env, NULL);
    if (res != JNI_OK || !env) {
        fprintf(stderr, "[payload] Failed to attach unload thread to JVM: %d\n", res);
        g_initialized = 0;
        return;
    }

    /* Call Bootstrap.shutdown() to stop tick thread and clear Java refs */
    if (g_bootstrap_class) {
        jmethodID shutdown = (*env)->GetStaticMethodID(env, g_bootstrap_class, "shutdown", "()V");
        if (shutdown) {
            (*env)->CallStaticVoidMethod(env, g_bootstrap_class, shutdown);
            (*env)->ExceptionClear(env);
        }
    }

    /* Wait for pending scheduled tasks to drain from MC's task queue */
    printf("[payload] Waiting for pending tasks to drain...\n");
    usleep(500000);  /* 500ms = ~10 game ticks */

    if (g_bootstrap_class) {
        /* Unregister native methods BEFORE deleting class ref.
         * Without this, the JVM has dangling pointers into payload.so
         * and will SIGABRT when dlclose unmaps the library. */
        (*env)->UnregisterNatives(env, g_bootstrap_class);
        (*env)->ExceptionClear(env);

        (*env)->DeleteGlobalRef(env, g_bootstrap_class);
        g_bootstrap_class = NULL;
    }

    /* Delete the isolated loader - allows classes to be GC'd */
    if (g_isolated_loader) {
        jclass cl_class = (*env)->GetObjectClass(env, g_isolated_loader);
        jmethodID close = (*env)->GetMethodID(env, cl_class, "close", "()V");
        if (close) {
            (*env)->CallVoidMethod(env, g_isolated_loader, close);
            (*env)->ExceptionClear(env);
        }
        (*env)->DeleteLocalRef(env, cl_class);

        (*env)->DeleteGlobalRef(env, g_isolated_loader);
        g_isolated_loader = NULL;
    }

    /* Request GC */
    jclass system = (*env)->FindClass(env, "java/lang/System");
    if (system) {
        jmethodID gc = (*env)->GetStaticMethodID(env, system, "gc", "()V");
        if (gc) {
            (*env)->CallStaticVoidMethod(env, system, gc);
        }
        (*env)->DeleteLocalRef(env, system);
    }

    /* Detach this thread from JVM */
    (*g_jvm)->DetachCurrentThread(g_jvm);

    /* Also detach the original worker thread's env */
    g_env = NULL;
    g_initialized = 0;
    printf("[payload] JNI cleanup complete, scheduling self-unload\n");

    /* Self-unload: dlclose payload.so from a trampoline on anonymous memory.
     * We already called UnregisterNatives above to remove the JVM's
     * dangling pointers into payload.so before it gets unmapped. */
    schedule_self_unload();

    printf("[payload] Unload complete\n");
}

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
 * Schedule self-unload via anonymous memory trampoline.
 *
 * Builds x86_64 shellcode on mmap'd memory (outside payload.so) that:
 *   1. Sleeps 200ms (SYS_nanosleep) to let calling threads leave payload.so
 *   2. Calls dlclose(handle) to unmap payload.so
 *   3. Calls SYS_exit(0) to terminate just this thread
 *
 * The thread's code lives entirely in anonymous memory + libc, so it
 * survives payload.so being unmapped.
 */
/*
 * Mapping entry for direct munmap-based unload.
 */
struct map_region {
    unsigned long addr;
    unsigned long len;
};

static void schedule_self_unload(void) {
    /*
     * Collect all payload.so mappings from /proc/self/maps so we can
     * munmap them directly.  This bypasses dlclose entirely, avoiding
     * the SIGSEGV in _dl_catch_exception that glibc triggers when
     * dlclose walks TLS / link_map structures from a thread that was
     * created while the library was loaded.
     */
    struct map_region regions[16];
    int num_regions = 0;

    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        fprintf(stderr, "[payload] Could not open /proc/self/maps\n");
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), maps) && num_regions < 16) {
        if (!strstr(line, "payload")) continue;
        if (!strstr(line, ".so")) continue;

        unsigned long start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            regions[num_regions].addr = start;
            regions[num_regions].len  = end - start;
            printf("[payload] Region %d: 0x%lx - 0x%lx (%lu bytes)\n",
                   num_regions, start, end, end - start);
            num_regions++;
        }
    }
    fclose(maps);

    if (num_regions == 0) {
        fprintf(stderr, "[payload] No payload.so mappings found\n");
        return;
    }

    printf("[payload] Found %d payload.so region(s) to munmap\n", num_regions);

    /*
     * Allocate an anonymous RWX page for the trampoline.
     * Layout:
     *   [0x000 .. code_end)  shellcode
     *   [0x200 .. 0x200 + N*16)  region table  (addr, len pairs)
     */
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        fprintf(stderr, "[payload] Could not mmap trampoline page\n");
        return;
    }

    /* Copy region table to offset 0x200 in the page */
    memcpy(page + 0x200, regions, num_regions * sizeof(struct map_region));

    /*
     * Build shellcode (called as pthread start_routine):
     *
     *   ; nanosleep({0, 200000000}, NULL) - wait 200ms for threads to exit
     *   ; loop over region table at (page + 0x200):
     *   ;   munmap(region.addr, region.len)   ; SYS_munmap = 11
     *   ; return NULL
     *
     * Register usage:
     *   r12 = pointer to current region entry
     *   r13 = remaining count
     *   r14 = base of this page (for reaching data at +0x200)
     */
    int off = 0;

    /* push callee-saved registers we use */
    page[off++] = 0x41; page[off++] = 0x54;  /* push r12 */
    page[off++] = 0x41; page[off++] = 0x55;  /* push r13 */
    page[off++] = 0x41; page[off++] = 0x56;  /* push r14 */

    /* sub rsp, 16  (for nanosleep timespec, maintains alignment) */
    page[off++] = 0x48; page[off++] = 0x83; page[off++] = 0xEC; page[off++] = 0x10;

    /* mov qword [rsp], 0       ; tv_sec = 0 */
    page[off++] = 0x48; page[off++] = 0xC7; page[off++] = 0x04; page[off++] = 0x24;
    page[off++] = 0x00; page[off++] = 0x00; page[off++] = 0x00; page[off++] = 0x00;

    /* mov dword [rsp+8], 200000000 (0x0BEBC200) ; tv_nsec */
    page[off++] = 0xC7; page[off++] = 0x44; page[off++] = 0x24; page[off++] = 0x08;
    uint32_t nsec = 200000000;
    memcpy(&page[off], &nsec, 4); off += 4;

    /* mov dword [rsp+12], 0 */
    page[off++] = 0xC7; page[off++] = 0x44; page[off++] = 0x24; page[off++] = 0x0C;
    page[off++] = 0x00; page[off++] = 0x00; page[off++] = 0x00; page[off++] = 0x00;

    /* mov rdi, rsp */
    page[off++] = 0x48; page[off++] = 0x89; page[off++] = 0xE7;
    /* xor esi, esi */
    page[off++] = 0x31; page[off++] = 0xF6;
    /* mov eax, 35 (SYS_nanosleep) */
    page[off++] = 0xB8; page[off++] = 35; page[off++] = 0x00; page[off++] = 0x00; page[off++] = 0x00;
    /* syscall */
    page[off++] = 0x0F; page[off++] = 0x05;

    /* add rsp, 16 */
    page[off++] = 0x48; page[off++] = 0x83; page[off++] = 0xC4; page[off++] = 0x10;

    /* lea r14, [rip - (off + 7)]   -- compute page base
     * Encoded as: 4c 8d 35 <rel32>
     * rel32 = -(off + 7)  because RIP-relative is from end of instruction */
    page[off++] = 0x4C; page[off++] = 0x8D; page[off++] = 0x35;
    int32_t rel = -(int32_t)(off + 4);  /* off+4 = end of this instruction */
    memcpy(&page[off], &rel, 4); off += 4;

    /* lea r12, [r14 + 0x200]   ; pointer to region table */
    page[off++] = 0x4D; page[off++] = 0x8D; page[off++] = 0xA6;
    uint32_t data_off = 0x200;
    memcpy(&page[off], &data_off, 4); off += 4;

    /* mov r13d, num_regions */
    page[off++] = 0x41; page[off++] = 0xBD;
    uint32_t nr = (uint32_t)num_regions;
    memcpy(&page[off], &nr, 4); off += 4;

    /* loop_top: */
    int loop_top = off;

    /* test r13d, r13d */
    page[off++] = 0x45; page[off++] = 0x85; page[off++] = 0xED;

    /* jz loop_done (patched below) */
    page[off++] = 0x74;
    int jz_patch = off;
    page[off++] = 0x00;  /* placeholder */

    /* mov rdi, [r12]       ; region.addr */
    page[off++] = 0x49; page[off++] = 0x8B; page[off++] = 0x3C; page[off++] = 0x24;

    /* mov rsi, [r12 + 8]   ; region.len */
    page[off++] = 0x49; page[off++] = 0x8B; page[off++] = 0x74; page[off++] = 0x24; page[off++] = 0x08;

    /* mov eax, 11 (SYS_munmap) */
    page[off++] = 0xB8; page[off++] = 11; page[off++] = 0x00; page[off++] = 0x00; page[off++] = 0x00;

    /* syscall */
    page[off++] = 0x0F; page[off++] = 0x05;

    /* add r12, 16   ; next entry */
    page[off++] = 0x49; page[off++] = 0x83; page[off++] = 0xC4; page[off++] = 0x10;

    /* dec r13d */
    page[off++] = 0x41; page[off++] = 0xFF; page[off++] = 0xCD;

    /* jmp loop_top */
    page[off] = 0xEB;
    page[off + 1] = (unsigned char)(loop_top - (off + 2));
    off += 2;

    /* loop_done: patch the jz offset */
    page[jz_patch] = (unsigned char)(off - (jz_patch + 1));

    /* pop callee-saved */
    page[off++] = 0x41; page[off++] = 0x5E;  /* pop r14 */
    page[off++] = 0x41; page[off++] = 0x5D;  /* pop r13 */
    page[off++] = 0x41; page[off++] = 0x5C;  /* pop r12 */

    /* xor eax, eax ; return NULL */
    page[off++] = 0x31; page[off++] = 0xC0;
    /* ret */
    page[off++] = 0xC3;

    printf("[payload] Trampoline built: %d bytes of shellcode\n", off);

    /* Launch the trampoline on a new thread */
    pthread_t t;
    if (pthread_create(&t, NULL, (void *(*)(void *))page, NULL) == 0) {
        pthread_detach(t);
        printf("[payload] Self-unload thread launched\n");
    } else {
        fprintf(stderr, "[payload] Failed to create self-unload thread\n");
        munmap(page, 4096);
    }
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

/*
 * Exported function to trigger unload from Java
 */
JNIEXPORT void JNICALL Java_client_Bootstrap_nativeUnload(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;

    printf("[payload] Native unload requested\n");
    g_should_unload = 1;

    /* Schedule unload on a separate thread to avoid deadlock */
    pthread_t thread;
    pthread_create(&thread, NULL, (void*(*)(void*))payload_unload, NULL);
    pthread_detach(thread);
}
