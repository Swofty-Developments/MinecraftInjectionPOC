package client;

import java.util.ArrayList;
import java.util.List;

/**
 * Tracks changes made to the game and restores them on unload.
 *
 * Modules call register() when they modify game state (replace a field,
 * change the window title, etc.). On shutdown, restoreAll() runs every
 * registered action in reverse order so the game returns to its original
 * state — enabling clean re-injection.
 */
public class RestoreManager {

    private static final List<RestoreAction> actions = new ArrayList<>();

    public static void register(String name, Runnable restore) {
        actions.add(new RestoreAction(name, restore));
    }

    public static void restoreAll() {
        System.out.println("[RestoreManager] Restoring " + actions.size() + " action(s)...");
        for (int i = actions.size() - 1; i >= 0; i--) {
            RestoreAction action = actions.get(i);
            try {
                action.restore.run();
                System.out.println("[RestoreManager] Restored: " + action.name);
            } catch (Exception e) {
                System.err.println("[RestoreManager] Failed to restore '" + action.name + "': " + e);
            }
        }
        actions.clear();
        System.out.println("[RestoreManager] All restored");
    }

    public static void shutdown() {
        actions.clear();
    }

    private static class RestoreAction {
        final String name;
        final Runnable restore;
        RestoreAction(String name, Runnable restore) {
            this.name = name;
            this.restore = restore;
        }
    }
}
