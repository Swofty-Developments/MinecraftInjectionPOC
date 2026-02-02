/*
 * injector.c - Linux ptrace-based shared library injector
 *
 * Compile: gcc -o injector injector.c -ldl
 * Usage:   sudo ./injector <pid> /path/to/payload.so
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <limits.h>

/* x86_64 syscall numbers */
#define SYS_MMAP    9
#define SYS_MUNMAP  11
#define SYS_MPROTECT 10
#define SYS_WRITE   1
#define SYS_CLOSE   3
#define SYS_MEMFD_CREATE 319
#define SYS_RT_SIGPROCMASK 14

/* Function pointer types */
typedef void* (*dlopen_t)(const char*, int);
typedef int (*dlclose_t)(void*);
typedef void* (*dlsym_t)(void*, const char*);

/* Remote process memory/execution helpers */
struct remote_info {
    pid_t pid;
    unsigned long libc_base;
    unsigned long libdl_base;
    unsigned long dlopen_addr;
    unsigned long dlsym_addr;
    unsigned long dlclose_addr;
    unsigned long malloc_addr;
    unsigned long free_addr;
};

/*
 * Read a NUL-terminated string from target process memory
 */
static int read_string(pid_t pid, unsigned long addr, char *buf, size_t len) {
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

/*
 * Write data to target process memory
 */
static int write_memory(pid_t pid, unsigned long addr, const void *data, size_t len) {
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

/*
 * Read data from target process memory
 */
static int read_memory(pid_t pid, unsigned long addr, void *buf, size_t len) {
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

/*
 * Find library base address in target process.
 * Returns the lowest mapped address for the library (the real load base),
 * not the executable segment, so offset calculations match dladdr's dli_fbase.
 */
static unsigned long find_library_base(pid_t pid, const char *libname) {
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

/*
 * Find symbol offset in a library (using our local copy)
 */
static unsigned long find_symbol_offset(const char *libpath, const char *symbol) {
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

/*
 * Get remote function address
 */
static unsigned long get_remote_func(pid_t pid, const char *libname, const char *funcname) {
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

/*
 * Wait for SIGTRAP, re-injecting any other signals back to the process.
 * JVMs use SIGSEGV for safepoint polling, so we must forward those.
 */
static int wait_for_trap(pid_t pid) {
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

/*
 * Execute a syscall in the target process using PTRACE_SINGLESTEP.
 *
 * Old approach: wrote "syscall; int3" at the thread's RIP in libc, then
 * continued execution. Race condition: other threads executing at the same
 * libc location would hit the int3 and kill the process with SIGTRAP.
 *
 * New approach: the thread was interrupted while in a syscall, so RIP-2
 * is a syscall instruction. We point RIP there, set up registers, and
 * single-step exactly one instruction. No memory modification needed.
 * If a signal is pending (JVM SIGSEGV safepoint), we suppress it and retry.
 */
static long remote_syscall(pid_t pid, long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
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

/*
 * Call a function in the target process via pthread_create.
 *
 * Problem: hijacking a JVM thread to call dlopen directly causes either
 * safepoint deadlocks (JVM can't GC) or crashes (raw clone has no TLS).
 *
 * Solution: call pthread_create on the hijacked thread to spawn a proper
 * thread with TLS. pthread_create is fast (microseconds) so it completes
 * before the JVM's safepoint timeout triggers. The new thread calls the
 * target function (dlopen), stores the result, and returns normally.
 *
 * The function code lives on a memfd-backed executable page (kernel W^X
 * prevents anonymous executable pages).
 */
static unsigned long remote_call(pid_t pid, unsigned long func_addr,
                                  unsigned long a1, unsigned long a2,
                                  unsigned long a3, unsigned long a4,
                                  unsigned long a5, unsigned long a6,
                                  unsigned long scratch_page) {
    struct user_regs_struct old_regs, regs;
    (void)a3; (void)a4; (void)a5; (void)a6;

    /*
     * Result layout on scratch page:
     *   +128: uint64_t result (return value from function)
     *   +136: uint8_t  done_flag (set to 1 when complete)
     *   +256: pthread_t (8 bytes, for pthread_create)
     */
    unsigned long result_addr = scratch_page + 128;

    /* Save registers */
    if (ptrace(PTRACE_GETREGS, pid, NULL, &old_regs) == -1) {
        perror("PTRACE_GETREGS");
        return -1;
    }

    /* Resolve pthread_create in target */
    unsigned long pcreate_addr = get_remote_func(pid, "libc.so", "pthread_create");
    if (!pcreate_addr)
        pcreate_addr = get_remote_func(pid, "libpthread", "pthread_create");
    if (!pcreate_addr) {
        fprintf(stderr, "Could not find pthread_create in target\n");
        return -1;
    }

    /* ---- Build exec page shellcode ---- */
    /*
     * This is a proper C-style function: void* worker(void* arg)
     * Called by pthread_create, so it has proper TLS and stack.
     *
     *   sub    rsp, 8             ; align stack for call
     *   movabs rdi, <a1>          ; first arg (path string)
     *   mov    esi, <a2>          ; second arg (RTLD_NOW)
     *   movabs rax, <func_addr>   ; function to call
     *   call   rax
     *   movabs rdi, <result_addr> ; where to store result
     *   mov    [rdi], rax         ; store return value
     *   mov    byte [rdi+8], 1    ; set completion flag
     *   xor    eax, eax           ; return NULL
     *   add    rsp, 8
     *   ret                       ; return to pthread cleanup
     */
    unsigned char exec_shellcode[80];
    int off = 0;

    /* sub rsp, 8 */
    exec_shellcode[off++] = 0x48; exec_shellcode[off++] = 0x83;
    exec_shellcode[off++] = 0xEC; exec_shellcode[off++] = 0x08;

    /* movabs rdi, a1 */
    exec_shellcode[off++] = 0x48; exec_shellcode[off++] = 0xBF;
    memcpy(&exec_shellcode[off], &a1, 8); off += 8;

    /* mov esi, a2 */
    exec_shellcode[off++] = 0xBE;
    uint32_t a2_32 = (uint32_t)a2;
    memcpy(&exec_shellcode[off], &a2_32, 4); off += 4;

    /* movabs rax, func_addr */
    exec_shellcode[off++] = 0x48; exec_shellcode[off++] = 0xB8;
    memcpy(&exec_shellcode[off], &func_addr, 8); off += 8;

    /* call rax */
    exec_shellcode[off++] = 0xFF; exec_shellcode[off++] = 0xD0;

    /* movabs rdi, result_addr */
    exec_shellcode[off++] = 0x48; exec_shellcode[off++] = 0xBF;
    memcpy(&exec_shellcode[off], &result_addr, 8); off += 8;

    /* mov [rdi], rax */
    exec_shellcode[off++] = 0x48; exec_shellcode[off++] = 0x89;
    exec_shellcode[off++] = 0x07;

    /* mov byte [rdi+8], 1 */
    exec_shellcode[off++] = 0xC6; exec_shellcode[off++] = 0x47;
    exec_shellcode[off++] = 0x08; exec_shellcode[off++] = 0x01;

    /* xor eax, eax */
    exec_shellcode[off++] = 0x31; exec_shellcode[off++] = 0xC0;

    /* add rsp, 8 */
    exec_shellcode[off++] = 0x48; exec_shellcode[off++] = 0x83;
    exec_shellcode[off++] = 0xC4; exec_shellcode[off++] = 0x08;

    /* ret */
    exec_shellcode[off++] = 0xC3;

    int exec_len = off;

    /* Build trampoline at offset 256: movabs rax, <pcreate>; call rax; int3 */
    unsigned char trampoline[16];
    memset(trampoline, 0xCC, sizeof(trampoline)); /* int3 padding */
    int toff = 0;
    trampoline[toff++] = 0x48; trampoline[toff++] = 0xB8;
    memcpy(&trampoline[toff], &pcreate_addr, 8); toff += 8;
    trampoline[toff++] = 0xFF; trampoline[toff++] = 0xD0;
    trampoline[toff++] = 0xCC;

    /* Clear the result/flag area */
    unsigned char zero[16] = {0};
    write_memory(pid, result_addr, zero, sizeof(zero));

    /* Write exec page content to scratch page:
     *   offset 0:   worker function (exec_shellcode)
     *   offset 256: trampoline (calls pthread_create then int3) */
    unsigned char page_buf[512];
    memset(page_buf, 0xCC, sizeof(page_buf)); /* fill with int3 */
    memcpy(page_buf, exec_shellcode, exec_len);
    memcpy(page_buf + 256, trampoline, sizeof(trampoline));

    write_memory(pid, scratch_page, page_buf, sizeof(page_buf));

    /* Write "x\0" name for memfd_create */
    unsigned char name[] = { 'x', '\0' };
    write_memory(pid, scratch_page + 512, name, 2);

    /* 1. memfd_create("x", MFD_CLOEXEC) -> fd */
    long fd = remote_syscall(pid, SYS_MEMFD_CREATE, scratch_page + 512, 1, 0, 0, 0, 0);
    if (fd < 0) {
        fprintf(stderr, "memfd_create failed: %ld\n", fd);
        return -1;
    }
    fprintf(stderr, "[remote_call] memfd_create -> fd %ld\n", fd);

    /* 2. write(fd, scratch_page, 512) - full page with worker + trampoline */
    long written = remote_syscall(pid, SYS_WRITE, fd, scratch_page, 512, 0, 0, 0);
    if (written != 512) {
        fprintf(stderr, "write to memfd failed: %ld\n", written);
        remote_syscall(pid, SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        return -1;
    }

    /* 3. mmap(NULL, 4096, PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 0) */
    unsigned long exec_page = remote_syscall(pid, SYS_MMAP,
        0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    fprintf(stderr, "[remote_call] exec mmap -> 0x%lx\n", exec_page);
    remote_syscall(pid, SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    if (exec_page == (unsigned long)-1 || exec_page == 0) {
        fprintf(stderr, "Failed to mmap executable page from memfd\n");
        return -1;
    }

    /* ---- Block all signals to prevent JVM SIGSEGV during our call ---- */
    /*
     * The JVM uses SIGSEGV for safepoint polling. If a pending SIGSEGV is
     * delivered before our code runs, the JVM signal handler finds corrupted
     * registers and crashes. Block all signals first via rt_sigprocmask.
     */
    {
        unsigned long all_blocked = ~0UL;
        write_memory(pid, scratch_page + 192, &all_blocked, 8);
    }
    long mask_ret = remote_syscall(pid, SYS_RT_SIGPROCMASK,
        0 /* SIG_BLOCK */, scratch_page + 192, scratch_page + 200,
        8 /* sizeof(sigset_t) for kernel */, 0, 0);
    fprintf(stderr, "[remote_call] rt_sigprocmask(SIG_BLOCK, all) -> %ld\n", mask_ret);

    /* ---- Call pthread_create via the exec page trampoline ---- */
    /*
     * We put the trampoline on the exec page (at offset 256) instead of
     * writing code at RIP. This avoids modifying shared libc code that
     * other threads could hit.
     *
     * Trampoline: movabs rax, <pthread_create>; call rax; int3
     */

    /* Set up registers: RIP points to trampoline on exec page */
    regs = old_regs;
    regs.rip = exec_page + 256;      /* trampoline location */
    regs.rdi = scratch_page + 272;   /* &pthread_t */
    regs.rsi = 0;                     /* attr = NULL */
    regs.rdx = exec_page;            /* start_routine = worker (offset 0) */
    regs.rcx = 0;                     /* arg = NULL */
    regs.rsp = old_regs.rsp & ~0xFUL; /* 16-byte align before call */
    regs.orig_rax = -1; /* prevent kernel from restarting interrupted syscall */

    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1) {
        perror("PTRACE_SETREGS");
        goto fail_cleanup;
    }

    fprintf(stderr, "[remote_call] calling pthread_create(start=0x%lx) via trampoline at 0x%lx\n",
            exec_page, exec_page + 256);

    if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) {
        perror("PTRACE_CONT");
        goto fail_cleanup;
    }

    /* Wait for int3, suppressing any signals that slip through */
    {
        int status;
        int sig_count = 0;
        for (;;) {
            waitpid(pid, &status, 0);
            if (!WIFSTOPPED(status)) {
                fprintf(stderr, "[remote_call] process died during pthread_create, status=0x%x\n", status);
                goto fail_cleanup;
            }
            int sig = WSTOPSIG(status);
            if (sig == SIGTRAP) break;

            if (sig_count < 5) {
                struct user_regs_struct dbg_regs;
                siginfo_t si;
                ptrace(PTRACE_GETREGS, pid, NULL, &dbg_regs);
                ptrace(PTRACE_GETSIGINFO, pid, NULL, &si);
                fprintf(stderr, "[remote_call] signal %d: rip=0x%llx rax=0x%llx si_code=%d si_addr=%p\n",
                        sig, dbg_regs.rip, dbg_regs.rax, si.si_code, si.si_addr);
            }
            sig_count++;
            if (sig_count > 50) {
                fprintf(stderr, "[remote_call] too many signals (%d), aborting\n", sig_count);
                goto fail_cleanup;
            }
            ptrace(PTRACE_CONT, pid, NULL, NULL);
        }
    }

    /* Get pthread_create return value */
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) == -1) {
        perror("PTRACE_GETREGS result");
        goto fail_cleanup;
    }
    fprintf(stderr, "[remote_call] pthread_create returned %lld\n", (long long)regs.rax);

    /* Restore registers */
    ptrace(PTRACE_SETREGS, pid, NULL, &old_regs);

    /* Restore original signal mask (unblock signals) */
    remote_syscall(pid, SYS_RT_SIGPROCMASK,
        2 /* SIG_SETMASK */, scratch_page + 200, 0,
        8 /* sizeof(sigset_t) */, 0, 0);
    fprintf(stderr, "[remote_call] signal mask restored\n");

    /* Restore registers again (remote_syscall changes them) */
    ptrace(PTRACE_SETREGS, pid, NULL, &old_regs);

    if ((long long)regs.rax != 0) {
        fprintf(stderr, "[remote_call] pthread_create failed with error %lld\n",
                (long long)regs.rax);
        goto fail_cleanup;
    }

    /*
     * Worker thread is now running with proper TLS. The hijacked thread
     * is restored and stopped. Poll completion via PTRACE_PEEKDATA.
     * The worker shares the same address space, so its writes are visible.
     */
    fprintf(stderr, "[remote_call] worker thread running, polling for completion...\n");

    unsigned long result = 0;
    int done = 0;
    for (int i = 0; i < 3000; i++) { /* 30 second timeout */
        usleep(10000); /* 10ms */

        long flag_word = ptrace(PTRACE_PEEKDATA, pid, result_addr + 8, NULL);
        if (flag_word == -1 && errno != 0) {
            fprintf(stderr, "[remote_call] PEEKDATA failed: %s\n", strerror(errno));
            break;
        }

        if (flag_word & 0xFF) {
            long res_lo = ptrace(PTRACE_PEEKDATA, pid, result_addr, NULL);
            result = (unsigned long)res_lo;
            done = 1;
            fprintf(stderr, "[remote_call] worker completed, result=0x%lx\n", result);
            break;
        }
    }

    if (!done) {
        fprintf(stderr, "[remote_call] timeout waiting for worker thread\n");
    }

    /* Skip munmap cleanup - the few KB leak is acceptable.
     * Calling remote_syscall here risks leaving int3 in libc code
     * if the process state is unstable, which kills the process. */

    /* Restore registers */
    ptrace(PTRACE_SETREGS, pid, NULL, &old_regs);

    return result;

fail_cleanup:
    return -1;
}

/*
 * Allocate memory in target process using mmap syscall
 */
static unsigned long remote_mmap(pid_t pid, size_t size) {
    return remote_syscall(pid, SYS_MMAP,
        0,                              /* addr (let kernel choose) */
        size,                           /* length */
        PROT_READ | PROT_WRITE,         /* prot */
        MAP_PRIVATE | MAP_ANONYMOUS,    /* flags */
        -1,                             /* fd */
        0                               /* offset */
    );
}

/*
 * Free memory in target process
 */
static int remote_munmap(pid_t pid, unsigned long addr, size_t size) {
    return remote_syscall(pid, SYS_MUNMAP, addr, size, 0, 0, 0, 0);
}

/*
 * Find a thread that is safely blocked in a syscall (not holding libc locks).
 * Returns the tid, or 0 if none found.
 */
static pid_t find_safe_thread(pid_t pid) {
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

/*
 * Main injection function
 */
int inject(pid_t pid, const char *so_path) {
    int ret = -1;
    unsigned long remote_str = 0;
    unsigned long dlopen_addr = 0;
    size_t path_len = strlen(so_path) + 1;
    pid_t tid = 0;

    printf("[*] Finding safe thread in process %d\n", pid);

    /* Find a thread that's blocked in a syscall (safe to hijack) */
    tid = find_safe_thread(pid);
    if (!tid) {
        fprintf(stderr, "No thread found in syscall, using main thread\n");
        tid = pid;
    }

    printf("[*] Attaching to thread %d\n", tid);

    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) == -1) {
        perror("PTRACE_ATTACH");
        return -1;
    }

    int status;
    waitpid(tid, &status, 0);
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "Thread did not stop as expected\n");
        goto cleanup;
    }

    printf("[*] Attached successfully\n");

    /* Find dlopen in target */
    dlopen_addr = get_remote_func(pid, "libc.so", "dlopen");
    if (!dlopen_addr) {
        dlopen_addr = get_remote_func(pid, "libdl", "dlopen");
    }

    if (!dlopen_addr) {
        fprintf(stderr, "Could not find dlopen in target\n");
        goto cleanup;
    }

    printf("[*] Found dlopen at 0x%lx\n", dlopen_addr);

    /* Allocate memory for path string in target */
    remote_str = remote_mmap(tid, 4096);
    if (remote_str == (unsigned long)-1 || remote_str == 0) {
        fprintf(stderr, "Failed to allocate memory in target\n");
        goto cleanup;
    }

    printf("[*] Allocated remote memory at 0x%lx\n", remote_str);

    /* Write path to target memory */
    if (write_memory(tid, remote_str, so_path, path_len) == -1) {
        fprintf(stderr, "Failed to write path to target\n");
        goto cleanup;
    }

    /* Allocate scratch page for memfd shellcode staging */
    unsigned long scratch = remote_mmap(tid, 4096);
    if (scratch == (unsigned long)-1 || scratch == 0) {
        fprintf(stderr, "Failed to allocate scratch page\n");
        goto cleanup;
    }

    printf("[*] Calling dlopen(\"%s\", RTLD_NOW)\n", so_path);

    /* Call dlopen via memfd-backed executable page.
     * Since we picked a thread blocked in a syscall, it's not
     * holding any userspace locks, so dlopen won't deadlock. */
    unsigned long handle = remote_call(tid, dlopen_addr,
        remote_str, RTLD_NOW, 0, 0, 0, 0, scratch);

    /* Don't free scratch page - remote_syscall after injection risks
     * leaving int3 in libc code. The ~8KB leak is acceptable. */

    if (handle == 0 || handle == (unsigned long)-1) {
        fprintf(stderr, "dlopen failed in target process (handle=0x%lx)\n", handle);
        goto cleanup;
    }

    printf("[+] Library loaded successfully! Handle: 0x%lx\n", handle);
    ret = 0;

cleanup:

    /* Detach */
    ptrace(PTRACE_DETACH, tid, NULL, NULL);
    printf("[*] Detached from thread %d\n", tid);

    return ret;
}

/*
 * Find JVM process by name
 */
pid_t find_jvm_process(const char *name) {
    FILE *fp;
    char cmd[256];
    char line[32];
    pid_t pid = 0;

    snprintf(cmd, sizeof(cmd), "pgrep -f '%s' | head -1", name);
    fp = popen(cmd, "r");
    if (fp && fgets(line, sizeof(line), fp)) {
        pid = atoi(line);
    }
    if (fp) pclose(fp);

    return pid;
}

int main(int argc, char *argv[]) {
    pid_t pid;
    const char *so_path;

    if (argc < 3) {
        printf("Usage: %s <pid|process_name> <path_to_so>\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s 12345 /tmp/payload.so\n", argv[0]);
        printf("  %s minecraft /tmp/payload.so\n", argv[0]);
        printf("  %s java /tmp/payload.so\n", argv[0]);
        return 1;
    }

    /* Try to parse as PID first */
    pid = atoi(argv[1]);
    if (pid == 0) {
        /* Not a number, search by name */
        pid = find_jvm_process(argv[1]);
        if (pid == 0) {
            fprintf(stderr, "Could not find process: %s\n", argv[1]);
            return 1;
        }
        printf("[*] Found process '%s' with PID %d\n", argv[1], pid);
    }

    so_path = argv[2];

    /* Verify .so exists */
    struct stat st;
    if (stat(so_path, &st) != 0) {
        fprintf(stderr, "Cannot access %s: %s\n", so_path, strerror(errno));
        return 1;
    }

    /* Must be absolute path */
    if (so_path[0] != '/') {
        char abs_path[PATH_MAX];
        if (realpath(so_path, abs_path)) {
            so_path = strdup(abs_path);
            printf("[*] Using absolute path: %s\n", so_path);
        } else {
            fprintf(stderr, "Please provide absolute path to .so\n");
            return 1;
        }
    }

    return inject(pid, so_path);
}
