/*
 * injector.c - Load a shared library into a running JVM via ptrace.
 *
 * Usage: sudo ./injector <pid|process_name> /path/to/payload.so
 *
 * We attach to a thread that's parked in a syscall, spawn a real pthread in
 * the target (so dlopen runs with proper TLS and no safepoint deadlock), and
 * have it call dlopen on our library. See remote.c for the shared plumbing.
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
#include <sys/stat.h>
#include <signal.h>
#include <limits.h>

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

    if (handle == 0 || handle == (unsigned long)-1) {
        fprintf(stderr, "dlopen failed in target process (handle=0x%lx)\n", handle);
        goto cleanup;
    }

    printf("[+] Library loaded successfully! Handle: 0x%lx\n", handle);
    ret = 0;

    /* The worker has finished dlopen, so the path buffer and the shellcode
     * staging page are no longer needed - free them so injection leaves no
     * anonymous scratch mappings behind. (The memfd exec page is unmapped
     * later by the payload itself, during unload.) remote_syscall single-steps
     * the thread's own parked syscall, so this writes no code into the
     * target. */
    remote_munmap(tid, remote_str, 4096);
    remote_munmap(tid, scratch, 4096);
    printf("[*] Freed injection scratch pages\n");

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
