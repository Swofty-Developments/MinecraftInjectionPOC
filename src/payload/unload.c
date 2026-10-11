/*
 * unload.c - Reverse everything the payload did and erase its mappings.
 *
 * Order matters: drain the JVM's scheduled tasks, unregister native methods
 * (so the JVM holds no dangling pointers into this .so), drop all Java refs,
 * then munmap our own regions directly. We never call dlclose: glibc corrupts
 * its TLS / link_map state unmapping a library from a thread created while it
 * was loaded, and crashes in _dl_catch_exception.
 */

#include "payload.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

static void schedule_self_unload(void);

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
