package client;

import client.rendering.MainOverlay;
import client.rendering.RenderHandler;
import client.rendering.SimpleGui;

/**
 * Thin orchestrator that wires all modules together.
 * Called from native code (payload.c) after classes are defined.
 */
public class Bootstrap {

    private static volatile boolean running = false;
    private static volatile boolean shouldShutdown = false;
    private static Thread tickThread;

    public static void init() {
        System.out.println("[Bootstrap] Initializing...");

        try {
            if (!McReflection.init()) {
                System.err.println("[Bootstrap] McReflection init failed");
                return;
            }

            ChatUtil.init();

            Object fontRenderer = McReflection.getFontRenderer();
            if (fontRenderer != null) {
                SimpleGui.initialize(fontRenderer, McReflection.getClassLoader());
                SimpleGui.setVisible(false);

                RenderHandler.init();
                RenderHandler.register(new MainOverlay());

                System.out.println("[Bootstrap] GUI system ready");
            } else {
                System.err.println("[Bootstrap] Could not find FontRenderer, GUI disabled");
            }

            InputManager.registerKeybind(52, "Unload", Bootstrap::requestUnload);
            InputManager.registerKeybind(54, "Toggle GUI", RenderHandler::toggleVisible);

            running = true;
            startTickThread();

            McReflection.changeTitle("Minecraft 1.8.9 - Injected");
            RestoreManager.register("window title", () -> McReflection.changeTitle("Minecraft 1.8.9"));

            McReflection.scheduleTask(() -> {
                try {
                    ChatUtil.sendChat("\u00a7a[Injected] \u00a7fPayload loaded successfully!");
                } catch (Exception e) {
                    System.err.println("[Bootstrap] Chat failed: " + e);
                }
            });

            System.out.println("[Bootstrap] Started successfully!");
        } catch (Exception e) {
            System.err.println("[Bootstrap] Error during init: " + e.getMessage());
            e.printStackTrace();
        }
    }

    public static void shutdown() {
        System.out.println("[Bootstrap] Shutting down...");

        running = false;
        shouldShutdown = true;

        // Wait for tick thread to stop scheduling new tasks
        if (tickThread != null) {
            try { tickThread.join(2000); }
            catch (InterruptedException e) {}
            tickThread = null;
        }

        // Restore game state ON MC's main thread so we don't race with
        // the render thread (e.g. swapping ingameGUI while a frame is drawing).
        // Use a CountDownLatch to block until MC executes it.
        java.util.concurrent.CountDownLatch latch = new java.util.concurrent.CountDownLatch(1);
        McReflection.scheduleTask(() -> {
            try {
                RestoreManager.restoreAll();
                ChatUtil.shutdown();
                RenderHandler.shutdown();
            } catch (Exception e) {
                System.err.println("[Bootstrap] Error during main-thread restore: " + e);
            } finally {
                latch.countDown();
            }
        });

        // Wait up to 3 seconds for MC to run our restore task
        try { latch.await(3, java.util.concurrent.TimeUnit.SECONDS); }
        catch (InterruptedException e) {}

        // These are safe to clear from any thread (no game state)
        InputManager.shutdown();
        SimpleGui.shutdown();
        McReflection.shutdown();
        RestoreManager.shutdown();

        System.gc();
        System.out.println("[Bootstrap] Shutdown complete.");
    }

    /**
     * Trigger unload — can be called from any module (e.g. MainOverlay button).
     */
    public static void requestUnload() {
        System.out.println("[Bootstrap] Unload requested");
        running = false;
        shouldShutdown = true;

        try {
            ChatUtil.sendChat("\u00a7c[Injected] \u00a7fUnloading...");
        } catch (Exception e) {}

        new Thread(() -> {
            try {
                Thread.sleep(100);
                nativeUnload();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }).start();
    }

    public static native void nativeUnload();

    // === Tick thread ===

    private static void startTickThread() {
        tickThread = new Thread(() -> {
            while (running && !shouldShutdown) {
                try {
                    Thread.sleep(50);
                    if (!running || shouldShutdown) break;

                    McReflection.scheduleTask(() -> {
                        if (shouldShutdown || !running) return;

                        InputManager.tick();

                        if (!RenderHandler.isVisible()) {
                            ChatUtil.showActionBar("\u00a7a\u00a7l[Injected] \u00a7r\u00a7fSwoftyMcClient");
                        }
                    });
                } catch (InterruptedException e) {
                    break;
                } catch (Exception e) {}
            }
        });
        tickThread.setDaemon(true);
        tickThread.setName("bootstrap-tick");
        tickThread.start();
    }
}
