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

There are no JVM arguments to find. No -javaagent, no attach API socket, nothing in /proc/<pid>/cmdline. The JVM was never told to load anything. InstrumentationImpl doesn't exist in the runtime because we never touched the Instrumentation API, so as far as the JVM is concerned, no agent has ever loaded. Running jmap -histo won't help either since the Bootstrap class lives in an isolated URLClassLoader, and on unload every reference gets nulled, the classloader is closed, and GC cleans up the rest. The class just isn't on the heap anymore.                                                                       

Checking /proc/<pid>/maps for suspicious .so files comes up empty too. On unload, the payload builds x86_64 shellcode on an anonymous memory page that munmaps every region belonging to payload.so, and the .so is gone from process maps entirely. We can't use dlclose for this since glibc corrupts its own TLS state and crashes in _dl_catch_exception, so instead we just rip the pages out directly with raw syscalls. Native methods are explicitly unregistered before any of this happens, so there are no dangling function pointers and nothing pointing into unmapped memory.                                       

No bytecode is modified at any point. Rendering is done from Java code defined via JNI into an isolated classloader, and Minecraft's own classes are completely untouched, byte-for-byte identical to vanilla.  

## What's actually left after unload

One thing:

- A small (~4KB) `memfd:x (deleted)` mapping; this is the anonymous executable page used as a trampoline during injection. It contains no identifying information, just generic function-call shellcode. It's indistinguishable from other anonymous mappings that various libraries create.

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
