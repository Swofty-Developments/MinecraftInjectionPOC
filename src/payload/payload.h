#ifndef PAYLOAD_H
#define PAYLOAD_H

#define _GNU_SOURCE
#include <jni.h>
#include <stddef.h>

/* Shared payload state. Defined once in entry.c, referenced everywhere else. */
extern JavaVM  *g_jvm;
extern JNIEnv  *g_env;
extern jobject  g_isolated_loader;
extern jclass   g_bootstrap_class;
extern void    *g_libjvm_handle;
extern volatile int g_initialized;
extern volatile int g_should_unload;

typedef jint (JNICALL *JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);
extern JNI_GetCreatedJavaVMs_t JNI_GetCreatedJavaVMs_func;

/* jni_bootstrap.c: locate the JVM, attach, and define our classes into it. */
int     find_jvm(void);
int     attach_to_jvm(void);
jobject create_isolated_classloader(JNIEnv *env);
int     bootstrap_java(void);

/* unload.c: tear everything down and erase our mappings. */
void payload_unload(void);
JNIEXPORT void JNICALL Java_client_Bootstrap_nativeUnload(JNIEnv *env, jclass cls);

#endif /* PAYLOAD_H */
