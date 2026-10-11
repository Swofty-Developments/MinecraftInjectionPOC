#ifndef INJECT_REMOTE_H
#define INJECT_REMOTE_H

#include <sys/types.h>
#include <stddef.h>

/* x86_64 syscall numbers used across the injector/unloader. */
#define SYS_MMAP          9
#define SYS_MPROTECT      10
#define SYS_MUNMAP        11
#define SYS_WRITE         1
#define SYS_CLOSE         3
#define SYS_MEMFD_CREATE  319
#define SYS_RT_SIGPROCMASK 14

/* ptrace-backed reads/writes of target process memory. */
int read_string(pid_t pid, unsigned long addr, char *buf, size_t len);
int write_memory(pid_t pid, unsigned long addr, const void *data, size_t len);
int read_memory(pid_t pid, unsigned long addr, void *buf, size_t len);

/* Symbol resolution in the target, via a local dlopen of the same library. */
unsigned long find_library_base(pid_t pid, const char *libname);
unsigned long find_symbol_offset(const char *libpath, const char *symbol);
unsigned long get_remote_func(pid_t pid, const char *libname, const char *funcname);

/* Run a single syscall in the target by single-stepping its own syscall insn. */
long remote_syscall(pid_t pid, long nr, long a1, long a2, long a3, long a4, long a5, long a6);
int  wait_for_trap(pid_t pid);

/* Convenience wrappers and target-thread selection. */
unsigned long remote_mmap(pid_t pid, size_t size);
int           remote_munmap(pid_t pid, unsigned long addr, size_t size);
pid_t         find_safe_thread(pid_t pid);

#endif /* INJECT_REMOTE_H */
