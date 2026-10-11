/*
 * remote.c - Primitives for operating on a traced target process: memory
 * reads/writes, symbol resolution, single-stepped syscalls, and picking a
 * thread that's safe to hijack. Shared by the injector and the unloader.
 */

#define _GNU_SOURCE
#include "remote.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <dirent.h>
#include <signal.h>

int read_string(pid_t pid, unsigned long addr, char *buf, size_t len) {
    size_t i = 0;
    while (i < len - 1) {
        long word = ptrace(PTRACE_PEEKDATA, pid, addr + i, NULL);
        if (word == -1 && errno != 0) return -1;

        for (int j = 0; j < sizeof(long) && i < len - 1; j++, i++) {
            buf[i] = (word >> (j * 8)) & 0xFF;
            if (buf[i] == '\0') return 0;
        }
    }
    buf[len - 1] = '\0';
    return 0;
}

int write_memory(pid_t pid, unsigned long addr, const void *data, size_t len) {
    const unsigned char *p = data;
    size_t i = 0;

    while (i < len) {
        long word = 0;
        size_t chunk = (len - i < sizeof(long)) ? (len - i) : sizeof(long);

        /* Read existing word if partial write */
        if (chunk < sizeof(long)) {
            word = ptrace(PTRACE_PEEKDATA, pid, addr + i, NULL);
            if (word == -1 && errno != 0) return -1;
        }

        memcpy(&word, p + i, chunk);

        if (ptrace(PTRACE_POKEDATA, pid, addr + i, word) == -1) {
            return -1;
        }
        i += sizeof(long);
    }
    return 0;
}

int read_memory(pid_t pid, unsigned long addr, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t i = 0;

    while (i < len) {
        long word = ptrace(PTRACE_PEEKDATA, pid, addr + i, NULL);
        if (word == -1 && errno != 0) return -1;

        size_t chunk = (len - i < sizeof(long)) ? (len - i) : sizeof(long);
        memcpy(p + i, &word, chunk);
        i += sizeof(long);
    }
    return 0;
}

unsigned long find_library_base(pid_t pid, const char *libname) {
    char maps_path[64];
    char line[512];
    unsigned long base = 0;

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *fp = fopen(maps_path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libname)) {
            base = strtoul(line, NULL, 16);
            break;  /* First mapping = lowest address = load base */
        }
    }

    fclose(fp);
    return base;
}

unsigned long find_symbol_offset(const char *libpath, const char *symbol) {
    void *handle = dlopen(libpath, RTLD_NOW);
    if (!handle) return 0;

    void *sym = dlsym(handle, symbol);
    if (!sym) {
        dlclose(handle);
        return 0;
    }

    Dl_info info;
    if (!dladdr(sym, &info)) {
        dlclose(handle);
        return 0;
    }

    /* Verify the symbol actually lives in the library we opened.
     * On modern glibc, libdl is a stub and dlsym("dlopen") resolves
     * to libc, making the offset relative to libc's base, not libdl's. */
    if (info.dli_fname && strstr(info.dli_fname, libpath) == NULL &&
        strstr(libpath, info.dli_fname) == NULL) {
        /* Try to match by basename */
        const char *info_base = strrchr(info.dli_fname, '/');
        const char *lib_base = strrchr(libpath, '/');
        info_base = info_base ? info_base + 1 : info.dli_fname;
        lib_base = lib_base ? lib_base + 1 : libpath;
        if (strstr(info_base, lib_base) == NULL && strstr(lib_base, info_base) == NULL) {
            fprintf(stderr, "Warning: %s resolved from %s, not %s (offset mismatch!)\n",
                    symbol, info.dli_fname, libpath);
            dlclose(handle);
            return 0;
        }
    }

    unsigned long offset = (unsigned long)sym - (unsigned long)info.dli_fbase;
    dlclose(handle);
    return offset;
}

unsigned long get_remote_func(pid_t pid, const char *libname, const char *funcname) {
    unsigned long remote_base = find_library_base(pid, libname);
    if (!remote_base) {
        fprintf(stderr, "Could not find %s in target process\n", libname);
        return 0;
    }

    /* Find the library path in target */
    char maps_path[64];
    char line[512];
    char libpath[256] = {0};

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *fp = fopen(maps_path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libname)) {
            char *path = strchr(line, '/');
            if (path) {
                char *nl = strchr(path, '\n');
                if (nl) *nl = '\0';
                strncpy(libpath, path, sizeof(libpath) - 1);
            }
            break;
        }
    }
    fclose(fp);

    if (!libpath[0]) {
        fprintf(stderr, "Could not find path for %s in target maps\n", libname);
        return 0;
    }

    unsigned long offset = find_symbol_offset(libpath, funcname);
    if (!offset) {
        fprintf(stderr, "Could not find %s in %s\n", funcname, libpath);
        return 0;
    }

    fprintf(stderr, "[get_remote_func] %s: remote_base=0x%lx offset=0x%lx addr=0x%lx (lib=%s)\n",
            funcname, remote_base, offset, remote_base + offset, libpath);
    return remote_base + offset;
}

int wait_for_trap(pid_t pid) {
    int status;
    for (;;) {
        waitpid(pid, &status, 0);
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "[wait_for_trap] process not stopped, status=0x%x\n", status);
            return -1;
        }
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) {
            return 0;
        }
        /* Re-deliver the signal and continue waiting */
        fprintf(stderr, "[wait_for_trap] forwarding signal %d to target\n", sig);
        if (ptrace(PTRACE_CONT, pid, NULL, (void*)(long)sig) == -1) {
            perror("PTRACE_CONT (signal forward)");
            return -1;
        }
    }
}

long remote_syscall(pid_t pid, long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    struct user_regs_struct old_regs, regs;

    /* Save registers */
    if (ptrace(PTRACE_GETREGS, pid, NULL, &old_regs) == -1) {
        perror("PTRACE_GETREGS");
        return -1;
    }

    /* Verify syscall instruction at RIP-2 (0x0F 0x05) */
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, pid, old_regs.rip - 2, NULL);
    if (errno != 0) {
        perror("PTRACE_PEEKDATA (verify syscall)");
        return -1;
    }
    if ((word & 0xFFFF) != 0x050F) {
        fprintf(stderr, "[remote_syscall] no syscall at RIP-2 (rip=0x%llx, bytes=0x%lx)\n",
                old_regs.rip, word & 0xFFFF);
        return -1;
    }

    /* Retry loop: single-step may be preempted by pending signals */
    for (int attempt = 0; attempt < 100; attempt++) {
        /* Set up registers: point RIP to the syscall instruction */
        regs = old_regs;
        regs.rip = old_regs.rip - 2;
        regs.rax = nr;
        regs.rdi = a1;
        regs.rsi = a2;
        regs.rdx = a3;
        regs.r10 = a4;
        regs.r8 = a5;
        regs.r9 = a6;
        regs.orig_rax = -1; /* prevent kernel syscall restart */

        if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1) {
            perror("PTRACE_SETREGS");
            return -1;
        }

        /* Single-step: execute the syscall instruction */
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) == -1) {
            perror("PTRACE_SINGLESTEP");
            return -1;
        }

        int status;
        waitpid(pid, &status, 0);
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "[remote_syscall] process died, status=0x%x\n", status);
            return -1;
        }

        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) {
            /* Single step completed successfully */
            if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) == -1) {
                perror("PTRACE_GETREGS result");
                return -1;
            }

            /* Restore registers */
            if (ptrace(PTRACE_SETREGS, pid, NULL, &old_regs) == -1) {
                perror("PTRACE_SETREGS restore");
                return -1;
            }

            return regs.rax;
        }

        /* A signal was intercepted before our instruction executed.
         * Suppress it (don't deliver) and retry. This is safe because:
         * - JVM SIGSEGV for safepoints: suppressing temporarily is fine,
         *   the JVM will re-trigger it when we restore the thread.
         * - Our registers haven't changed (signal was caught before delivery).
         */
        if (attempt < 3) {
            fprintf(stderr, "[remote_syscall] suppressing signal %d, retry %d\n", sig, attempt);
        }
    }

    fprintf(stderr, "[remote_syscall] too many signal retries\n");
    return -1;
}

unsigned long remote_mmap(pid_t pid, size_t size) {
    return remote_syscall(pid, SYS_MMAP,
        0,                              /* addr (let kernel choose) */
        size,                           /* length */
        PROT_READ | PROT_WRITE,         /* prot */
        MAP_PRIVATE | MAP_ANONYMOUS,    /* flags */
        -1,                             /* fd */
        0                               /* offset */
    );
}

int remote_munmap(pid_t pid, unsigned long addr, size_t size) {
    return remote_syscall(pid, SYS_MUNMAP, addr, size, 0, 0, 0, 0);
}

pid_t find_safe_thread(pid_t pid) {
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);

    DIR *dir = opendir(task_path);
    if (!dir) return 0;

    pid_t best_tid = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        pid_t tid = atoi(entry->d_name);
        if (tid == 0) continue;

        /* Check if thread is in a syscall via /proc/tid/syscall */
        char syscall_path[128];
        snprintf(syscall_path, sizeof(syscall_path), "/proc/%d/syscall", tid);
        FILE *fp = fopen(syscall_path, "r");
        if (!fp) continue;

        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            long syscall_nr = strtol(line, NULL, 10);
            /* Thread is blocked in a syscall if syscall_nr >= 0.
             * -1 means "not in syscall" (running userspace code).
             * Prefer threads in futex(202), poll(7), epoll_wait(232),
             * ppoll(271), clock_nanosleep(230), etc. */
            if (syscall_nr >= 0) {
                fprintf(stderr, "[find_safe_thread] tid %d in syscall %ld\n", tid, syscall_nr);
                best_tid = tid;
                fclose(fp);
                break;  /* First one in a syscall is good enough */
            }
        }
        fclose(fp);
    }
    closedir(dir);
    return best_tid;
}
