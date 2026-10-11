/*
 * jni_bootstrap.c - Find the running JVM, attach, and define our classes
 * into an isolated URLClassLoader so they can be GC'd cleanly on unload.
 */

#include "payload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/*
 * Find libjvm.so and get JNI_GetCreatedJavaVMs
 */
int find_jvm(void) {
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
int attach_to_jvm(void) {
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
jobject create_isolated_classloader(JNIEnv *env) {
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
int bootstrap_java(void) {
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
