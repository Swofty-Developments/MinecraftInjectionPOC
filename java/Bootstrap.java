import java.lang.reflect.*;

public class Bootstrap {

    // State
    private static volatile boolean running = false;
    private static volatile boolean shouldShutdown = false;

    // Minecraft objects (found via reflection)
    private static Object minecraft;
    private static Object ingameGUI;
    private static ClassLoader mcClassLoader;

    // Cached methods
    private static Method addScheduledTask;
    private static Method actionBarMethod; // setRecordPlaying(String, boolean)

    /**
     * Called from native code after class is defined.
     */
    public static void init() {
        System.out.println("[Bootstrap] Initializing...");

        try {
            // Find Minecraft's class loader
            mcClassLoader = findMinecraftClassLoader();
            if (mcClassLoader == null) {
                System.err.println("[Bootstrap] Could not find Minecraft ClassLoader");
                return;
            }

            // Find Minecraft instance
            minecraft = findMinecraft();
            if (minecraft == null) {
                System.err.println("[Bootstrap] Could not find Minecraft instance");
                return;
            }

            // Cache addScheduledTask(Runnable)
            cacheScheduleMethod();

            // Find ingameGUI for action bar text
            ingameGUI = findIngameGUI();
            if (ingameGUI != null) {
                cacheActionBarMethod();
            }

            running = true;

            // Change window title
            changeTitle("Minecraft 1.8.9 - Injected");

            // Send chat message on main thread (player may not exist yet)
            scheduleTask(() -> {
                try {
                    sendChat("\u00a7a[Injected] \u00a7fPayload loaded successfully!");
                } catch (Exception e) {
                    System.err.println("[Bootstrap] Chat failed: " + e);
                }
            });

            // Start daemon thread for action bar updates + key polling
            startTickThread();

            System.out.println("[Bootstrap] Started successfully!");

        } catch (Exception e) {
            System.err.println("[Bootstrap] Error during init: " + e.getMessage());
            e.printStackTrace();
        }
    }

    /**
     * Called from native code before unloading.
     */
    public static void shutdown() {
        System.out.println("[Bootstrap] Shutting down...");

        running = false;
        shouldShutdown = true;

        // Clear action bar
        try {
            if (ingameGUI != null && actionBarMethod != null) {
                actionBarMethod.invoke(ingameGUI, "", true);
            }
        } catch (Exception e) {}

        // Restore window title
        try {
            changeTitle("Minecraft 1.8.9");
        } catch (Exception e) {}

        // Clear all references
        minecraft = null;
        ingameGUI = null;
        mcClassLoader = null;
        addScheduledTask = null;
        actionBarMethod = null;

        // Request GC
        System.gc();

        System.out.println("[Bootstrap] Shutdown complete.");
    }

    /**
     * Native method to trigger .so unload.
     */
    public static native void nativeUnload();

    // =========================================================================
    // Tick thread - action bar updates + period key polling
    // =========================================================================

    private static void startTickThread() {
        Thread t = new Thread(() -> {
            while (running && !shouldShutdown) {
                try {
                    Thread.sleep(50); // ~20 tps, matches MC tick rate
                    if (!running || shouldShutdown) break;

                    scheduleTask(() -> {
                        if (shouldShutdown || !running) return;

                        // Check period key (LWJGL keycode 52)
                        if (isKeyDown(52)) {
                            System.out.println("[Bootstrap] Unload key pressed");
                            triggerUnload();
                            return;
                        }

                        // Keep action bar text visible - MC renders this during HUD pass
                        showActionBar("\u00a7a\u00a7l[Injected] \u00a7r\u00a7fSwoftyMcClient");
                    });
                } catch (InterruptedException e) {
                    break;
                } catch (Exception e) {
                    // Continue
                }
            }
        });
        t.setDaemon(true);
        t.setName("bootstrap-tick");
        t.start();
    }

    private static void triggerUnload() {
        running = false;
        shouldShutdown = true;

        // Send unload message
        try {
            sendChat("\u00a7c[Injected] \u00a7fUnloading...");
        } catch (Exception e) {}

        // Call native unload on a separate thread
        new Thread(() -> {
            try {
                Thread.sleep(100);  // Let current frame finish
                nativeUnload();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }).start();
    }

    // =========================================================================
    // Action bar display
    // =========================================================================

    private static void showActionBar(String text) {
        if (ingameGUI == null || actionBarMethod == null) return;
        try {
            actionBarMethod.invoke(ingameGUI, text, true);
        } catch (Exception e) {}
    }

    private static void cacheActionBarMethod() {
        // setRecordPlayingMessage(String, boolean) on GuiIngame
        // In obfuscated 1.8.9: a(String, boolean) on avo
        Class<?> guiClass = ingameGUI.getClass();

        // Search class and superclass chain
        Class<?> current = guiClass;
        while (current != null && current != Object.class) {
            for (Method m : current.getDeclaredMethods()) {
                Class<?>[] params = m.getParameterTypes();
                if (params.length == 2 && params[0] == String.class && params[1] == boolean.class) {
                    m.setAccessible(true);
                    actionBarMethod = m;
                    System.out.println("[Bootstrap] Found action bar method: " + current.getName() + "." + m.getName());
                    return;
                }
            }
            current = current.getSuperclass();
        }

        System.err.println("[Bootstrap] Could not find action bar method on " + guiClass.getName());
    }

    // =========================================================================
    // Chat messages
    // =========================================================================

    private static void sendChat(String message) throws Exception {
        Object player = findPlayer();
        if (player == null) {
            System.err.println("[Bootstrap] No player found for chat");
            return;
        }

        // Strategy: find addChatMessage on player by looking for methods that
        // take a single interface/abstract parameter (IChatComponent), then
        // find a concrete class implementing it with a String constructor
        // (ChatComponentText).
        for (Method m : player.getClass().getMethods()) {
            Class<?>[] params = m.getParameterTypes();
            if (params.length != 1) continue;
            Class<?> paramType = params[0];
            if (paramType.isPrimitive() || paramType == Object.class || paramType == String.class) continue;

            // Try to create a chat component matching this parameter type
            Object chatComp = createChatComponent(paramType, message);
            if (chatComp != null) {
                try {
                    m.setAccessible(true);
                    m.invoke(player, chatComp);
                    System.out.println("[Bootstrap] Chat sent via " + m.getName() + "(" + paramType.getName() + ")");
                    return;
                } catch (Exception e) {
                    // Wrong method, keep searching
                }
            }
        }

        System.err.println("[Bootstrap] Could not find addChatMessage method");
    }

    private static Object createChatComponent(Class<?> targetType, String text) {
        // If targetType itself has a String constructor, try it directly
        try {
            return targetType.getConstructor(String.class).newInstance(text);
        } catch (Exception e) {}

        // Search for a concrete implementation with a String constructor
        // MC 1.8.9 obfuscated classes are two-letter names in default package
        for (char c1 = 'a'; c1 <= 'z'; c1++) {
            for (char c2 = 'a'; c2 <= 'z'; c2++) {
                String name = "" + c1 + c2;
                try {
                    Class<?> cls = mcClassLoader.loadClass(name);
                    if (!targetType.isAssignableFrom(cls)) continue;
                    if (cls.isInterface() || java.lang.reflect.Modifier.isAbstract(cls.getModifiers())) continue;
                    Object comp = cls.getConstructor(String.class).newInstance(text);
                    System.out.println("[Bootstrap] Found ChatComponentText: " + name);
                    return comp;
                } catch (Exception e) {}
            }
        }

        // Also try unobfuscated name
        try {
            Class<?> cls = mcClassLoader.loadClass("net.minecraft.util.ChatComponentText");
            return cls.getConstructor(String.class).newInstance(text);
        } catch (Exception e) {}

        return null;
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    private static void scheduleTask(Runnable task) {
        if (addScheduledTask == null || minecraft == null) return;
        try {
            addScheduledTask.invoke(minecraft, task);
        } catch (Exception e) {}
    }

    private static void changeTitle(String title) {
        try {
            Class<?> display = Class.forName("org.lwjgl.opengl.Display");
            Method setTitle = display.getMethod("setTitle", String.class);
            setTitle.invoke(null, title);
            System.out.println("[Bootstrap] Window title: " + title);
        } catch (Exception e) {
            System.err.println("[Bootstrap] Could not change title: " + e);
        }
    }

    private static boolean isKeyDown(int keyCode) {
        try {
            Class<?> keyboard = Class.forName("org.lwjgl.input.Keyboard");
            Method isKeyDown = keyboard.getMethod("isKeyDown", int.class);
            return (boolean) isKeyDown.invoke(null, keyCode);
        } catch (Exception e) {
            return false;
        }
    }

    private static Class<?> findClass(String... names) {
        for (String name : names) {
            try {
                return mcClassLoader.loadClass(name);
            } catch (ClassNotFoundException e) {}
        }
        return null;
    }

    // =========================================================================
    // Finding Minecraft objects
    // =========================================================================

    private static ClassLoader findMinecraftClassLoader() {
        // Try thread context loader
        ClassLoader cl = Thread.currentThread().getContextClassLoader();
        if (hasMinecraftClass(cl)) return cl;

        // Try system loader
        cl = ClassLoader.getSystemClassLoader();
        if (hasMinecraftClass(cl)) return cl;

        // Walk parent chain
        cl = Thread.currentThread().getContextClassLoader();
        while (cl != null) {
            if (hasMinecraftClass(cl)) return cl;
            cl = cl.getParent();
        }

        // Search all threads' context classloaders
        Thread[] threads = new Thread[Thread.activeCount() + 10];
        int count = Thread.enumerate(threads);
        for (int i = 0; i < count; i++) {
            cl = threads[i].getContextClassLoader();
            if (hasMinecraftClass(cl)) {
                System.out.println("[Bootstrap] Found MC ClassLoader from thread: " + threads[i].getName());
                return cl;
            }
        }

        return null;
    }

    private static boolean hasMinecraftClass(ClassLoader cl) {
        if (cl == null) return false;

        String[] classes = {
            "net.minecraft.client.Minecraft",
            "ave"  // Obfuscated 1.8.9
        };

        for (String name : classes) {
            try {
                cl.loadClass(name);
                return true;
            } catch (ClassNotFoundException ignored) {}
        }
        return false;
    }

    private static Object findMinecraft() {
        String[][] attempts = {
            {"ave", "A"},  // Obfuscated 1.8.9
            {"net.minecraft.client.Minecraft", "getMinecraft"},
            {"net.minecraft.client.Minecraft", "func_71410_x"}
        };

        for (String[] attempt : attempts) {
            try {
                Class<?> mcClass = mcClassLoader.loadClass(attempt[0]);
                Method method = mcClass.getDeclaredMethod(attempt[1]);
                method.setAccessible(true);
                Object instance = method.invoke(null);
                if (instance != null) {
                    System.out.println("[Bootstrap] Found Minecraft: " + attempt[0] + "." + attempt[1]);
                    return instance;
                }
            } catch (Exception ignored) {}
        }

        return null;
    }

    private static Object findIngameGUI() {
        if (minecraft == null) return null;
        Class<?> mcClass = minecraft.getClass();

        // Try known field names
        String[] names = {"q", "ingameGUI", "field_71456_v"};
        for (String name : names) {
            try {
                Field f = mcClass.getDeclaredField(name);
                f.setAccessible(true);
                Object gui = f.get(minecraft);
                if (gui != null) {
                    System.out.println("[Bootstrap] Found ingameGUI: " + name);
                    return gui;
                }
            } catch (Exception e) {}
        }

        // Search by type name
        for (Field f : mcClass.getDeclaredFields()) {
            String typeName = f.getType().getName();
            if (typeName.contains("GuiIngame") || typeName.equals("avo")) {
                try {
                    f.setAccessible(true);
                    Object gui = f.get(minecraft);
                    if (gui != null) {
                        System.out.println("[Bootstrap] Found ingameGUI by type: " + f.getName());
                        return gui;
                    }
                } catch (Exception e) {}
            }
        }

        return null;
    }

    private static Object findPlayer() {
        if (minecraft == null) return null;
        Class<?> mcClass = minecraft.getClass();

        // Try known field names
        String[] names = {"h", "thePlayer", "field_71439_g"};
        for (String name : names) {
            try {
                Field f = mcClass.getDeclaredField(name);
                f.setAccessible(true);
                Object p = f.get(minecraft);
                if (p != null) return p;
            } catch (Exception e) {}
        }

        // Search by type name
        for (Field f : mcClass.getDeclaredFields()) {
            String typeName = f.getType().getName();
            if (typeName.contains("EntityPlayerSP") || typeName.contains("Player")) {
                try {
                    f.setAccessible(true);
                    Object p = f.get(minecraft);
                    if (p != null) return p;
                } catch (Exception e) {}
            }
        }

        return null;
    }

    private static void cacheScheduleMethod() {
        Class<?> mcClass = minecraft.getClass();

        // Find addScheduledTask(Runnable) by name + signature
        String[] names = {"addScheduledTask", "func_152344_a", "a"};
        for (String name : names) {
            try {
                Method m = mcClass.getDeclaredMethod(name, Runnable.class);
                m.setAccessible(true);
                addScheduledTask = m;
                System.out.println("[Bootstrap] Found addScheduledTask: " + name);
                return;
            } catch (NoSuchMethodException e) {}
        }

        // Fallback: any method taking exactly Runnable
        for (Method m : mcClass.getDeclaredMethods()) {
            Class<?>[] params = m.getParameterTypes();
            if (params.length == 1 && params[0] == Runnable.class) {
                m.setAccessible(true);
                addScheduledTask = m;
                System.out.println("[Bootstrap] Found schedule method by sig: " + m.getName());
                return;
            }
        }

        System.err.println("[Bootstrap] Could not find addScheduledTask");
    }
}
