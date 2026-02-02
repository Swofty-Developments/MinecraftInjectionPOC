package client;

import java.util.ArrayList;
import java.util.List;

/**
 * Keybind registration and polling.
 * Register keybinds once, then call tick() each game tick.
 */
public class InputManager {

    private static final List<Keybind> keybinds = new ArrayList<>();

    public static void registerKeybind(int keyCode, String name, Runnable onPress) {
        keybinds.add(new Keybind(keyCode, name, onPress));
        System.out.println("[InputManager] Registered keybind: " + name + " (key " + keyCode + ")");
    }

    public static void tick() {
        for (Keybind kb : keybinds) {
            boolean down = McReflection.isKeyDown(kb.keyCode);
            if (down && !kb.wasDown) {
                kb.onPress.run();
            }
            kb.wasDown = down;
        }
    }

    public static void shutdown() {
        keybinds.clear();
    }

    private static class Keybind {
        final int keyCode;
        final String name;
        final Runnable onPress;
        boolean wasDown;

        Keybind(int keyCode, String name, Runnable onPress) {
            this.keyCode = keyCode;
            this.name = name;
            this.onPress = onPress;
            this.wasDown = false;
        }
    }
}
