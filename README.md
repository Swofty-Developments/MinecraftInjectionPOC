# Minecraft JVMTI Injection POC

## Background

So I was recommended a YouTube video that was displaying a mod for a PVP server screensharing a player, and I thought this was odd, because surely a screenshare is not a reliable indicator? These tools claim they can detect injected cheats by checking things like JVM arguments, loaded classes, registered transformers, and heap dumps. I wanted to see if that's actually true.

Turns out, it's not. This project is a proof of concept that injects into a running Minecraft 1.8.9 client, renders text on screen using Minecraft's own rendering pipeline, and then fully cleans up after itself, leaving essentially nothing for a screenshare tool to find.

The payload is just green "Injected" text at the top of the screen. Press `RShift` and everything reverts to vanilla. It's not a cheat, it's a demonstration that the detection methods these tools rely on are fundamentally broken.

## How it works

The injector uses the Java Attach API to connect to a running Minecraft JVM and load an agent JAR. Think of it as the Linux equivalent of DLL injection, except there's no `-javaagent` flag in the process arguments, no modified files on disk, nothing in the launch command that looks suspicious.

Once inside, the agent uses ASM to rewrite `GuiIngame.renderGameOverlay` (obfuscated as `avo.a(F)V`) in memory. The injected bytecode calls directly into Minecraft's own `FontRenderer` and `ScaledResolution` classes to draw text. From a bytecode perspective, the rendering calls are indistinguishable from vanilla Minecraft code because they *are* vanilla Minecraft rendering calls, just with our string.

## Why screenshare tools can't catch it

Here's most detection methods these tools commonly use, and why none of them work:

**Checking `-javaagent` flags?** We don't use one. The Attach API loads the agent at runtime over a Unix socket. `/proc/<pid>/cmdline` is clean.

**Scanning for registered transformers?** Ours removes itself immediately after modifying the bytecode. The transformer only exists for a single `retransformClasses()` call, then it's gone. The `TransformerManager` is empty.

**Running `jmap -histo` to look for suspicious classes?** The agent classes live in an isolated `URLClassLoader` with `parent=null`. When self-destruct fires, the classloader is closed, all references are nulled, and the GC collects every class it loaded. They're just gone.

This works because we split things into two JARs:

```
bootstrap.jar (minimal, on bootstrap classloader):
  Agent.class, AgentBootstrap.class, HookRegistry.class

agent.jar (loaded by our custom URLClassLoader):
  Everything above + OverlayTransformer, TransformerCoordinator, DestructHandler, ASM
```

Only `bootstrap.jar` goes on the bootstrap classloader, and it doesn't contain any of the stealth classes. So when the custom classloader gets GC'd, those classes go with it.

**Comparing in-memory bytecode against the original JAR?** On self-destruct, the transformer re-registers briefly with `isActive=false`, calls `retransformClasses()`, and returns `null` telling the JVM to restore the original bytecode. After that, `avo` is byte-for-byte identical to vanilla.

**Tracing class references from the injected code?** The destruct callback is stored as a `Runnable` in `System.getProperties()` and invoked indirectly. Static analysis of `Agent.class` shows no dependency on the stealth package. The property is removed after use.

## What's actually left after self-destruct

Not much:

- **`sun.instrument.InstrumentationImpl`** - this exists whenever any agent has ever been loaded. Forge uses agents. Fabric uses agents. VisualVM uses agents. Half the profilers out there use agents. It's present in tons of legitimate setups and proves nothing on its own.
- **`jdk.internal.org.objectweb.asm.Type`** - part of the JDK itself, loaded by lambda metafactory internals and other standard JDK stuff. Not our ASM. Weak evidence.

That's the full list. Every trace has a completely legitimate explanation.

## So what should anti-cheat tools actually do?

The honest answer is that most of what screenshare tools check is security theater, if an actual competent Java developer really wanted to make an "undetectable" cheat, they could!

[<img src="https://discordapp.com/assets/e4923594e694a21542a489471ecffa50.svg" alt="Discord" height="55" />](https://discord.swofty.net)

## License

Educational and security research purposes only.
