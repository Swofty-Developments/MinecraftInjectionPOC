/*
 * unloader.c - Utility to trigger unload of the injected payload
 *
 * Compile: gcc -o unloader unloader.c -ldl
 * Usage:   sudo ./unloader <pid>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/mman.h>

#define SYS_MMAP    9
#define SYS_MEMFD_CREATE 319
#define SYS_WRITE   1
#define SYS_CLOSE   3

/*
 * Find library base address and path in target process
 */
static int find_payload_info(pid_t pid, unsigned long *base, char *path, size_t path_len) {
    char maps_path[64];
    char line[512];

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *fp = fopen(maps_path, "r");
    if (!fp) {
        perror("fopen maps");
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "payload.so") && strstr(line, "r-xp")) {
            *base = strtoul(line, NULL, 16);

            char *p = strchr(line, '/');
            if (p) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                strncpy(path, p, path_len - 1);
                path[path_len - 1] = '\0';
            }

            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;
}

/*
 * Write memory to target process
 */
static int write_memory(pid_t pid, unsigned long addr, const void *data, size_t len) {
    const unsigned char *p = data;
    size_t i = 0;

    while (i < len) {
        long word = 0;
        size_t chunk = (len - i < sizeof(long)) ? (len - i) : sizeof(long);

        if (chunk < sizeof(long)) {
            word = ptrace(PTRACE_PEEKDATA, pid, addr + i, NULL);
        }

        memcpy(&word, p + i, chunk);

        if (ptrace(PTRACE_POKEDATA, pid, addr + i, word) == -1) {
            perror("PTRACE_POKEDATA");
            return -1;
        }
        i += sizeof(long);
    }
    return 0;
}

/*
 * Find function address in target
 */
static unsigned long find_remote_symbol(pid_t pid, const char *libname, const char *symbol) {
    char maps_path[64];
    char line[512];
    char libpath[256] = {0};
    unsigned long remote_base = 0;

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *fp = fopen(maps_path, "r");
    if (!fp) return 0;

    /* Find the FIRST mapping (lowest address = load base) for this library */
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libname)) {
            remote_base = strtoul(line, NULL, 16);
            char *p = strchr(line, '/');
            if (p) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                strncpy(libpath, p, sizeof(libpath) - 1);
            }
            break;
        }
    }
    fclose(fp);

    if (!remote_base || !libpath[0]) return 0;

    /* Get symbol offset from local copy */
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

    /* Verify symbol resolved from the expected library */
    if (info.dli_fname && strstr(info.dli_fname, libpath) == NULL &&
        strstr(libpath, info.dli_fname) == NULL) {
        const char *info_base = strrchr(info.dli_fname, '/');
        const char *lib_base = strrchr(libpath, '/');
        info_base = info_base ? info_base + 1 : info.dli_fname;
        lib_base = lib_base ? lib_base + 1 : libpath;
        if (strstr(info_base, lib_base) == NULL && strstr(lib_base, info_base) == NULL) {
            fprintf(stderr, "Warning: %s resolved from %s, not %s\n", symbol, info.dli_fname, libpath);
            dlclose(handle);
            return 0;
        }
    }

    unsigned long offset = (unsigned long)sym - (unsigned long)info.dli_fbase;
    dlclose(handle);

    return remote_base + offset;
}

/*
 * Execute a syscall in target via PTRACE_SINGLESTEP.
 * No memory modification - reuses the existing syscall instruction at RIP-2.
 */
static long remote_syscall(pid_t pid, long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    struct user_regs_struct old_regs, regs;

    if (ptrace(PTRACE_GETREGS, pid, NULL, &old_regs) == -1) {
        perror("PTRACE_GETREGS");
        return -1;
    }

    /* Verify syscall instruction at RIP-2 */
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, pid, old_regs.rip - 2, NULL);
    if (errno != 0 || (word & 0xFFFF) != 0x050F) {
        fprintf(stderr, "[remote_syscall] no syscall at RIP-2\n");
        return -1;
    }

    for (int attempt = 0; attempt < 100; attempt++) {
        regs = old_regs;
        regs.rip = old_regs.rip - 2;
        regs.rax = nr;
        regs.rdi = a1;
        regs.rsi = a2;
        regs.rdx = a3;
        regs.r10 = a4;
        regs.r8 = a5;
        regs.r9 = a6;
        regs.orig_rax = -1;

        if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1) return -1;
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) == -1) return -1;

        int status;
        waitpid(pid, &status, 0);
        if (!WIFSTOPPED(status)) return -1;

        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) {
            ptrace(PTRACE_GETREGS, pid, NULL, &regs);
            ptrace(PTRACE_SETREGS, pid, NULL, &old_regs);
            return regs.rax;
        }

        /* Suppress pending signal and retry */
    }

    fprintf(stderr, "[remote_syscall] too many retries\n");
    return -1;
}

/*
 * Call a function in the target process via memfd exec page.
 * No memory modification of existing code - avoids race conditions
 * with other threads executing the same libc code.
 *
 * Builds: movabs rax, <func>; call rax; int3
 * on a memfd-backed executable page.
 */
static unsigned long remote_call(pid_t pid, unsigned long func_addr,
                                  unsigned long arg1, unsigned long arg2) {
    struct user_regs_struct old_regs;

    if (ptrace(PTRACE_GETREGS, pid, NULL, &old_regs) == -1) {
        perror("PTRACE_GETREGS");
        return -1;
    }

    /* Build trampoline: movabs rax, <func>; call rax; int3 */
    unsigned char trampoline[16];
    int off = 0;
    trampoline[off++] = 0x48; trampoline[off++] = 0xB8;
    memcpy(&trampoline[off], &func_addr, 8); off += 8;
    trampoline[off++] = 0xFF; trampoline[off++] = 0xD0;  /* call rax */
    trampoline[off++] = 0xCC;  /* int3 */

    /* Write trampoline to a scratch page */
    unsigned char page[256];
    memset(page, 0xCC, sizeof(page));
    memcpy(page, trampoline, off);

    /* Write "x\0" name at offset 128 */
    page[128] = 'x'; page[129] = '\0';

    /* Allocate scratch page for staging */
    unsigned long scratch = remote_syscall(pid, SYS_MMAP,
        0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == (unsigned long)-1 || scratch == 0) {
        fprintf(stderr, "Failed to allocate scratch page\n");
        return -1;
    }

    write_memory(pid, scratch, page, sizeof(page));

    /* Create memfd, write trampoline, mmap as executable */
    long fd = remote_syscall(pid, SYS_MEMFD_CREATE, scratch + 128, 1, 0, 0, 0, 0);
    if (fd < 0) {
        fprintf(stderr, "memfd_create failed: %ld\n", fd);
        return -1;
    }

    remote_syscall(pid, SYS_WRITE, fd, scratch, 256, 0, 0, 0);

    unsigned long exec_page = remote_syscall(pid, SYS_MMAP,
        0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    remote_syscall(pid, SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    if (exec_page == (unsigned long)-1 || exec_page == 0) {
        fprintf(stderr, "Failed to mmap exec page\n");
        return -1;
    }

    /* Set up registers: RIP to exec page, args in rdi/rsi */
    struct user_regs_struct regs = old_regs;
    regs.rip = exec_page;
    regs.rdi = arg1;
    regs.rsi = arg2;
    regs.rsp = old_regs.rsp & ~0xFUL;  /* 16-byte align */
    regs.orig_rax = -1;

    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1) {
        perror("PTRACE_SETREGS");
        return -1;
    }

    if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) {
        perror("PTRACE_CONT");
        return -1;
    }

    /* Wait for int3, suppressing signals */
    for (int i = 0; i < 100; i++) {
        int status;
        waitpid(pid, &status, 0);
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "process died during remote_call, status=0x%x\n", status);
            return -1;
        }
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) break;
        /* Suppress and continue */
        ptrace(PTRACE_CONT, pid, NULL, NULL);
    }

    /* Get result */
    ptrace(PTRACE_GETREGS, pid, NULL, &regs);

    /* Restore */
    ptrace(PTRACE_SETREGS, pid, NULL, &old_regs);

    return regs.rax;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <pid>\n", argv[0]);
        return 1;
    }

    pid_t pid = atoi(argv[1]);
    if (pid <= 0) {
        fprintf(stderr, "Invalid PID\n");
        return 1;
    }

    printf("[*] Looking for payload.so in process %d\n", pid);

    unsigned long payload_base;
    char payload_path[256];

    if (find_payload_info(pid, &payload_base, payload_path, sizeof(payload_path)) != 0) {
        fprintf(stderr, "payload.so not found in target process\n");
        return 1;
    }

    printf("[*] Found payload.so at 0x%lx (%s)\n", payload_base, payload_path);

    printf("[*] Attaching to process\n");
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
        perror("PTRACE_ATTACH");
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);

    /* Find dlopen and dlclose */
    unsigned long dlopen_addr = find_remote_symbol(pid, "libc", "dlopen");
    if (!dlopen_addr) {
        dlopen_addr = find_remote_symbol(pid, "libdl", "dlopen");
    }
    if (!dlopen_addr) {
        fprintf(stderr, "Could not find dlopen\n");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    printf("[*] Found dlopen at 0x%lx\n", dlopen_addr);

    unsigned long dlclose_addr = find_remote_symbol(pid, "libc", "dlclose");
    if (!dlclose_addr) {
        dlclose_addr = find_remote_symbol(pid, "libdl", "dlclose");
    }
    if (!dlclose_addr) {
        fprintf(stderr, "Could not find dlclose\n");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    printf("[*] Found dlclose at 0x%lx\n", dlclose_addr);

    /* Allocate memory for path string */
    unsigned long remote_str = remote_syscall(pid, SYS_MMAP,
        0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (remote_str == (unsigned long)-1 || remote_str == 0) {
        fprintf(stderr, "Failed to allocate memory\n");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }

    write_memory(pid, remote_str, payload_path, strlen(payload_path) + 1);

    /* Get handle via dlopen(path, RTLD_NOLOAD | RTLD_NOW)
     * RTLD_NOLOAD (0x4) returns existing handle without incrementing refcount.
     * RTLD_NOW (0x2) for immediate binding. */
    printf("[*] Getting handle via dlopen(RTLD_NOLOAD | RTLD_NOW)\n");
    unsigned long handle = remote_call(pid, dlopen_addr, remote_str, 0x4 | 0x2);

    if (handle == 0) {
        printf("[!] Could not get handle - library may already be unloaded\n");
    } else {
        printf("[*] Got handle: 0x%lx\n", handle);

        /* RTLD_NOLOAD doesn't increment refcount, so one dlclose is enough.
         * The injector's original dlopen added refcount=1, so one dlclose
         * brings it to 0 and triggers the destructor. */
        printf("[*] Calling dlclose\n");
        unsigned long result = remote_call(pid, dlclose_addr, handle, 0);
        printf("[*] dlclose returned: %lu\n", result);

        /* Verify it's gone */
        unsigned long check = remote_call(pid, dlopen_addr, remote_str, 0x4);
        if (check == 0) {
            printf("[+] payload.so successfully unloaded!\n");
        } else {
            printf("[!] payload.so still loaded (handle=0x%lx), calling dlclose again\n", check);
            remote_call(pid, dlclose_addr, check, 0);
        }
    }

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    printf("[*] Detached\n");

    return 0;
}
