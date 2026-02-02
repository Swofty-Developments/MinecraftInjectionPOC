package client;

import java.lang.reflect.*;

/**
 * Chat and action bar utilities.
 */
public class ChatUtil {

    private static Method actionBarMethod;

    public static void init() {
        Object ingameGUI = McReflection.getIngameGUI();
        if (ingameGUI != null) {
            cacheActionBarMethod(ingameGUI);
        }
        System.out.println("[ChatUtil] Initialized");
    }

    public static void shutdown() {
        try {
            Object ingameGUI = McReflection.getIngameGUI();
            if (ingameGUI != null && actionBarMethod != null) {
                actionBarMethod.invoke(ingameGUI, "", true);
            }
        } catch (Exception e) {}
        actionBarMethod = null;
    }

    public static void sendChat(String message) {
        Object player = McReflection.findPlayer();
        if (player == null) {
            System.err.println("[ChatUtil] No player found for chat");
            return;
        }

        for (Method m : player.getClass().getMethods()) {
            Class<?>[] params = m.getParameterTypes();
            if (params.length != 1) continue;
            Class<?> paramType = params[0];
            if (paramType.isPrimitive() || paramType == Object.class || paramType == String.class) continue;

            Object chatComp = createChatComponent(paramType, message);
            if (chatComp != null) {
                try {
                    m.setAccessible(true);
                    m.invoke(player, chatComp);
                    return;
                } catch (Exception e) {}
            }
        }
        System.err.println("[ChatUtil] Could not find addChatMessage method");
    }

    public static void showActionBar(String text) {
        Object ingameGUI = McReflection.getIngameGUI();
        if (ingameGUI == null || actionBarMethod == null) return;
        try { actionBarMethod.invoke(ingameGUI, text, true); }
        catch (Exception e) {}
    }

    // === Internal ===

    private static void cacheActionBarMethod(Object ingameGUI) {
        Class<?> current = ingameGUI.getClass();
        while (current != null && current != Object.class) {
            for (Method m : current.getDeclaredMethods()) {
                Class<?>[] params = m.getParameterTypes();
                if (params.length == 2 && params[0] == String.class && params[1] == boolean.class) {
                    m.setAccessible(true);
                    actionBarMethod = m;
                    System.out.println("[ChatUtil] Found action bar method: " + current.getName() + "." + m.getName());
                    return;
                }
            }
            current = current.getSuperclass();
        }
        System.err.println("[ChatUtil] Could not find action bar method");
    }

    private static Object createChatComponent(Class<?> targetType, String text) {
        try { return targetType.getConstructor(String.class).newInstance(text); }
        catch (Exception e) {}

        ClassLoader mcClassLoader = McReflection.getClassLoader();
        if (mcClassLoader == null) return null;

        for (char c1 = 'a'; c1 <= 'z'; c1++) {
            for (char c2 = 'a'; c2 <= 'z'; c2++) {
                String name = "" + c1 + c2;
                try {
                    Class<?> cls = mcClassLoader.loadClass(name);
                    if (!targetType.isAssignableFrom(cls)) continue;
                    if (cls.isInterface() || Modifier.isAbstract(cls.getModifiers())) continue;
                    return cls.getConstructor(String.class).newInstance(text);
                } catch (Exception e) {}
            }
        }

        try {
            Class<?> cls = mcClassLoader.loadClass("net.minecraft.util.ChatComponentText");
            return cls.getConstructor(String.class).newInstance(text);
        } catch (Exception e) {}

        return null;
    }
}
