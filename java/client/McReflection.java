package client;

import java.lang.reflect.*;

/**
 * Centralized MC object discovery via reflection.
 * Finds Minecraft instance, FontRenderer, ingameGUI, player, classloader,
 * and provides utility methods for scheduling tasks, checking keys, etc.
 */
public class McReflection {

    private static Object minecraft;
    private static Object ingameGUI;
    private static Object fontRenderer;
    private static ClassLoader mcClassLoader;
    private static Method addScheduledTask;

    public static boolean init() {
        try {
            mcClassLoader = findMinecraftClassLoader();
            if (mcClassLoader == null) {
                System.err.println("[McReflection] Could not find Minecraft ClassLoader");
                return false;
            }

            minecraft = findMinecraft();
            if (minecraft == null) {
                System.err.println("[McReflection] Could not find Minecraft instance");
                return false;
            }

            cacheScheduleMethod();
            ingameGUI = findIngameGUI();
            fontRenderer = findFontRenderer();

            System.out.println("[McReflection] Initialized");
            return true;
        } catch (Exception e) {
            System.err.println("[McReflection] Init failed: " + e.getMessage());
            e.printStackTrace();
            return false;
        }
    }

    public static void shutdown() {
        minecraft = null;
        ingameGUI = null;
        fontRenderer = null;
        mcClassLoader = null;
        addScheduledTask = null;
    }

    // === Getters ===

    public static Object getMinecraft() { return minecraft; }
    public static Object getIngameGUI() { return ingameGUI; }
    public static void setIngameGUI(Object gui) { ingameGUI = gui; }
    public static Object getFontRenderer() { return fontRenderer; }
    public static ClassLoader getClassLoader() { return mcClassLoader; }

    // === Utility methods ===

    public static void scheduleTask(Runnable task) {
        if (addScheduledTask == null || minecraft == null) return;
        try {
            addScheduledTask.invoke(minecraft, task);
        } catch (Exception e) {}
    }

    public static void changeTitle(String title) {
        try {
            Class<?> display = Class.forName("org.lwjgl.opengl.Display");
            Method setTitle = display.getMethod("setTitle", String.class);
            setTitle.invoke(null, title);
            System.out.println("[McReflection] Window title: " + title);
        } catch (Exception e) {
            System.err.println("[McReflection] Could not change title: " + e);
        }
    }

    public static boolean isKeyDown(int keyCode) {
        try {
            Class<?> keyboard = Class.forName("org.lwjgl.input.Keyboard");
            Method m = keyboard.getMethod("isKeyDown", int.class);
            return (boolean) m.invoke(null, keyCode);
        } catch (Exception e) {
            return false;
        }
    }

    public static Class<?> findClass(String... names) {
        for (String name : names) {
            try { return mcClassLoader.loadClass(name); }
            catch (ClassNotFoundException e) {}
        }
        return null;
    }

    public static Object findPlayer() {
        if (minecraft == null) return null;
        Class<?> mcClass = minecraft.getClass();

        for (String name : new String[]{"h", "thePlayer", "field_71439_g"}) {
            try {
                Field f = mcClass.getDeclaredField(name);
                f.setAccessible(true);
                Object p = f.get(minecraft);
                if (p != null) return p;
            } catch (Exception e) {}
        }

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

    // === Internal finders ===

    private static ClassLoader findMinecraftClassLoader() {
        ClassLoader cl = Thread.currentThread().getContextClassLoader();
        if (hasMinecraftClass(cl)) return cl;

        cl = ClassLoader.getSystemClassLoader();
        if (hasMinecraftClass(cl)) return cl;

        cl = Thread.currentThread().getContextClassLoader();
        while (cl != null) {
            if (hasMinecraftClass(cl)) return cl;
            cl = cl.getParent();
        }

        Thread[] threads = new Thread[Thread.activeCount() + 10];
        int count = Thread.enumerate(threads);
        for (int i = 0; i < count; i++) {
            cl = threads[i].getContextClassLoader();
            if (hasMinecraftClass(cl)) {
                System.out.println("[McReflection] Found MC ClassLoader from thread: " + threads[i].getName());
                return cl;
            }
        }
        return null;
    }

    private static boolean hasMinecraftClass(ClassLoader cl) {
        if (cl == null) return false;
        for (String name : new String[]{"net.minecraft.client.Minecraft", "ave"}) {
            try { cl.loadClass(name); return true; }
            catch (ClassNotFoundException ignored) {}
        }
        return false;
    }

    private static Object findMinecraft() {
        String[][] attempts = {
            {"ave", "A"},
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
                    System.out.println("[McReflection] Found Minecraft: " + attempt[0] + "." + attempt[1]);
                    return instance;
                }
            } catch (Exception ignored) {}
        }
        return null;
    }

    private static Object findFontRenderer() {
        if (minecraft == null) return null;
        Class<?> mcClass = minecraft.getClass();

        for (String name : new String[]{"k", "fontRendererObj", "field_71466_p"}) {
            try {
                Field f = mcClass.getDeclaredField(name);
                f.setAccessible(true);
                Object fr = f.get(minecraft);
                if (fr != null) {
                    System.out.println("[McReflection] Found FontRenderer: " + name);
                    return fr;
                }
            } catch (Exception e) {}
        }

        for (Field f : mcClass.getDeclaredFields()) {
            String typeName = f.getType().getName();
            if (typeName.contains("FontRenderer") || typeName.equals("avn")) {
                try {
                    f.setAccessible(true);
                    Object fr = f.get(minecraft);
                    if (fr != null) {
                        System.out.println("[McReflection] Found FontRenderer by type: " + f.getName());
                        return fr;
                    }
                } catch (Exception e) {}
            }
        }
        return null;
    }

    private static Object findIngameGUI() {
        if (minecraft == null) return null;
        Class<?> mcClass = minecraft.getClass();

        for (String name : new String[]{"q", "ingameGUI", "field_71456_v"}) {
            try {
                Field f = mcClass.getDeclaredField(name);
                f.setAccessible(true);
                Object gui = f.get(minecraft);
                if (gui != null) {
                    System.out.println("[McReflection] Found ingameGUI: " + name);
                    return gui;
                }
            } catch (Exception e) {}
        }

        for (Field f : mcClass.getDeclaredFields()) {
            String typeName = f.getType().getName();
            if (typeName.contains("GuiIngame") || typeName.equals("avo")) {
                try {
                    f.setAccessible(true);
                    Object gui = f.get(minecraft);
                    if (gui != null) {
                        System.out.println("[McReflection] Found ingameGUI by type: " + f.getName());
                        return gui;
                    }
                } catch (Exception e) {}
            }
        }
        return null;
    }

    private static void cacheScheduleMethod() {
        Class<?> mcClass = minecraft.getClass();

        for (String name : new String[]{"addScheduledTask", "func_152344_a", "a"}) {
            try {
                Method m = mcClass.getDeclaredMethod(name, Runnable.class);
                m.setAccessible(true);
                addScheduledTask = m;
                System.out.println("[McReflection] Found addScheduledTask: " + name);
                return;
            } catch (NoSuchMethodException e) {}
        }

        for (Method m : mcClass.getDeclaredMethods()) {
            Class<?>[] params = m.getParameterTypes();
            if (params.length == 1 && params[0] == Runnable.class) {
                m.setAccessible(true);
                addScheduledTask = m;
                System.out.println("[McReflection] Found schedule method by sig: " + m.getName());
                return;
            }
        }
        System.err.println("[McReflection] Could not find addScheduledTask");
    }
}
