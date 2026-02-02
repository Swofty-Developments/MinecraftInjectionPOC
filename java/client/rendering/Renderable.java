package client.rendering;

/**
 * Interface for anything that draws on screen during the render pass.
 *
 * To add a new overlay:
 *   1. Create a class implementing Renderable
 *   2. Register it with RenderHandler.register(new MyOverlay())
 *   3. Done — it renders every frame when the overlay is visible
 */
public interface Renderable {

    /** Unique name for this renderable. */
    String getName();

    /** Called once when registered. */
    default void onEnable() {}

    /** Called when unregistered or on shutdown. */
    default void onDisable() {}

    /** Called every frame during MC's render pass when overlay is visible. */
    void render();

    /** Whether this renderable should draw. Can be toggled independently. */
    default boolean isEnabled() { return true; }
}
