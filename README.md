# Minecraft Native Injection POC

## Background

So I was recommended a YouTube video that was displaying a mod for a PVP server screensharing a player, and I thought this was odd, because surely a screenshare is not a reliable indicator? These tools claim they can detect injected cheats by checking things like JVM arguments, loaded classes, registered transformers, and heap dumps. I wanted to see if that's actually true.

Turns out, it's not. This project is a proof of concept that injects into a running Minecraft 1.8.9 client at the native level, below Java entirely, using Linux `ptrace` to load a shared library directly into the JVM process, bootstrap Java code via JNI, render on screen using Minecraft's own systems, and then fully clean up after itself, leaving essentially nothing for a screenshare tool to find.

The payload is action bar text and a modified window title. Press `.` (period) and everything reverts to vanilla. The `.so` is removed from process memory via direct `munmap` shellcode.

## How it works

The injector uses Linux `ptrace` to attach to a running Minecraft JVM process, the same mechanism debuggers like GDB use. It finds a safe thread (one blocked in a syscall, not holding userspace locks), creates an anonymous executable memory region via `memfd_create`, writes shellcode to it, and spawns a new thread in the target process via `pthread_create`. That thread calls `dlopen` to load our `payload.so` into the JVM's address space.

Once loaded, `payload.so`'s constructor fires. It finds the running JVM via `JNI_GetCreatedJavaVMs`, attaches a native thread, creates an isolated `URLClassLoader` with no parent, and defines a `Bootstrap` class into it using `Unsafe.defineClass`. Then it calls `Bootstrap.init()`, which discovers Minecraft objects entirely through reflection, with it walking through thread context classloaders, trying obfuscated and mapped field names, and finding the Minecraft singleton, the GUI, and the scheduled task system.

From there, a daemon tick thread runs at ~20 tps, updating the action bar and polling for the unload key.

## Why screenshare tools can't catch it

Here's most detection methods these tools commonly use, and why none of them work:

**Checking JVM arguments?** There are none. No `-javaagent`, no attach API socket, nothing in `/proc/<pid>/cmdline`.

**Scanning for `InstrumentationImpl`?** It doesn't exist. We never used the Instrumentation API. The JVM has no record of any agent ever loading.

**Running `jmap -histo`?** The Bootstrap class lives in an isolated `URLClassLoader`. On unload, all references are nulled, the classloader is closed, and the GC collects everything. The class is gone from the heap.

**Checking `/proc/<pid>/maps` for suspicious `.so` files?** On unload, the payload generates x86_64 shellcode on an anonymous memory page that `munmap`s every region belonging to `payload.so`. The `.so` vanishes from the process maps entirely. No `dlclose`, since that would crash due to glibc TLS corruption (as I roughly found out), just raw `munmap` of every mapped region.

**Looking for loaded native libraries via JNI?** The JNI native methods are explicitly unregistered before unload. No dangling function pointers, no registered native methods pointing into unmapped memory.

**Checking for modified bytecode?** We don't modify any bytecode. The rendering is done from Java code defined via JNI into an isolated classloader. Minecraft's own classes are untouched, byte-for-byte identical to vanilla.

## What's actually left after unload

One thing:

- A small (~4KB) `memfd:x (deleted)` mapping; this is the anonymous executable page used as a trampoline during injection. It contains no identifying information, just generic function-call shellcode. It's indistinguishable from other anonymous mappings that various libraries create.

That's it. No `.so` in the maps, no classes in the heap, no registered transformers, no native methods, no Instrumentation instance, no agent traces. The JVM has no idea anything happened.

## Building

```bash
make
```

Requires:
- GCC
- JDK 8+ (for JNI headers and `javac`)
- Linux x86_64

## Usage

```bash
# Build and install to /tmp
make install

# Inject (requires root for ptrace)
sudo /tmp/injector <pid> /tmp/payload.so

# Or find by process name
sudo /tmp/injector minecraft /tmp/payload.so

# Unload externally
sudo /tmp/unloader <pid>

# Or press '.' (period) in-game to unload
```

## Version Support

Bootstrap auto-detects Minecraft mappings:
- Obfuscated 1.8.9 (vanilla / Prism Launcher)
- MCP-mapped (development)
- SRG-mapped (Forge production)
- Yarn-mapped (Fabric)

## So what should anti-cheat tools actually do?

The honest answer is that most of what screenshare tools check is security theater. If an actual competent developer really wanted to make an undetectable cheat, they could.

## License

Educational and security research purposes only.
