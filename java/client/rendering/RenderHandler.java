package client.rendering;

import client.McReflection;
import client.RestoreManager;

import java.lang.reflect.*;
import java.util.ArrayList;
import java.util.List;

/**
 * Central rendering dispatcher. Manages the render hook into MC's pipeline
 * and dispatches to registered Renderable instances each frame.
 */
public class RenderHandler {

    private static Method glPushMatrix;
    private static Method glPopMatrix;
    private static final List<Renderable> renderables = new ArrayList<>();
    private static volatile boolean visible = false;

    public static void init() {
        cacheGLMethods();
        installRenderHook();
        System.out.println("[RenderHandler] Initialized");
    }

    public static void shutdown() {
        for (Renderable r : renderables) {
            try { r.onDisable(); }
            catch (Exception e) {
                System.err.println("[RenderHandler] Error disabling " + r.getName() + ": " + e);
            }
        }
        renderables.clear();
        glPushMatrix = null;
        glPopMatrix = null;
        visible = false;
    }

    public static void register(Renderable renderable) {
        renderables.add(renderable);
        renderable.onEnable();
        System.out.println("[RenderHandler] Registered: " + renderable.getName());
    }

    public static void unregister(String name) {
        renderables.removeIf(r -> {
            if (r.getName().equals(name)) {
                r.onDisable();
                return true;
            }
            return false;
        });
    }

    /**
     * Called every frame from RenderHook during MC's actual render pass.
     */
    public static void onRenderOverlay() {
        if (glPushMatrix == null || !visible) return;

        try {
            SimpleGui.updateInput();
            glPushMatrix.invoke(null);

            for (Renderable r : renderables) {
                if (r.isEnabled()) {
                    try { r.render(); }
                    catch (Exception e) {}
                }
            }

            glPopMatrix.invoke(null);
        } catch (Exception e) {}
    }

    public static boolean isVisible() { return visible; }

    public static void toggleVisible() {
        visible = !visible;
        SimpleGui.setVisible(visible);
    }

    public static void setVisible(boolean v) {
        visible = v;
        SimpleGui.setVisible(v);
    }

    // === Internal ===

    private static void cacheGLMethods() {
        try {
            Class<?> gl11 = Class.forName("org.lwjgl.opengl.GL11");
            glPushMatrix = gl11.getMethod("glPushMatrix");
            glPopMatrix = gl11.getMethod("glPopMatrix");
        } catch (Exception e) {
            System.err.println("[RenderHandler] Could not cache GL methods: " + e);
        }
    }

    private static void installRenderHook() {
        try {
            Object minecraft = McReflection.getMinecraft();
            Object originalGUI = McReflection.getIngameGUI();

            Class<?> hookClass = RenderHandler.class.getClassLoader().loadClass("RenderHook");
            Class<?> mcClass = minecraft.getClass();

            Constructor<?> ctor = hookClass.getConstructor(mcClass);
            ctor.setAccessible(true);
            Object hookInstance = ctor.newInstance(minecraft);

            if (originalGUI != null) {
                copyFields(originalGUI, hookInstance);
            }

            Field guiField = findGuiField(mcClass, originalGUI);
            if (guiField != null) {
                guiField.setAccessible(true);
                guiField.set(minecraft, hookInstance);
                McReflection.setIngameGUI(hookInstance);
                System.out.println("[RenderHandler] Render hook installed");

                RestoreManager.register("ingameGUI", () -> {
                    try {
                        if (originalGUI != null) copyFields(hookInstance, originalGUI);
                        guiField.set(minecraft, originalGUI);
                        McReflection.setIngameGUI(originalGUI);
                    } catch (Exception e) {
                        System.err.println("[RenderHandler] Failed to restore ingameGUI: " + e);
                    }
                });
            } else {
                System.err.println("[RenderHandler] Could not find ingameGUI field");
            }
        } catch (Exception e) {
            System.err.println("[RenderHandler] Failed to install render hook: " + e.getMessage());
            e.printStackTrace();
        }
    }

    private static Field findGuiField(Class<?> mcClass, Object originalGUI) {
        for (String name : new String[]{"q", "ingameGUI", "field_71456_v"}) {
            try { return mcClass.getDeclaredField(name); }
            catch (NoSuchFieldException e) {}
        }
        if (originalGUI != null) {
            for (Field f : mcClass.getDeclaredFields()) {
                if (f.getType().getName().equals(originalGUI.getClass().getName())) return f;
            }
        }
        return null;
    }

    private static void copyFields(Object src, Object dst) {
        Class<?> cls = src.getClass();
        while (cls != null && cls != Object.class) {
            for (Field f : cls.getDeclaredFields()) {
                if (Modifier.isStatic(f.getModifiers())) continue;
                try {
                    f.setAccessible(true);
                    f.set(dst, f.get(src));
                } catch (Exception e) {}
            }
            cls = cls.getSuperclass();
        }
    }
}
