/**
 * Subclass of GuiIngame (obfuscated: avo) that hooks into MC's render pipeline.
 *
 * MC calls ingameGUI.a(float) every frame during the render pass to draw the HUD.
 * We override it to call super (draw normal HUD), then render our overlay on top.
 *
 * This class MUST stay in the default package because it extends avo (default package).
 * It references the client package by fully-qualified name.
 */
public class RenderHook extends avo {

    public RenderHook(ave mc) {
        super(mc);
    }

    @Override
    public void a(float partialTicks) {
        super.a(partialTicks);
        client.rendering.RenderHandler.onRenderOverlay();
    }
}
