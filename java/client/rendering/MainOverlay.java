package client.rendering;

import client.Bootstrap;
import client.ChatUtil;

/**
 * The main SwoftyMcClient control panel overlay.
 */
public class MainOverlay implements Renderable {

    @Override
    public String getName() {
        return "MainOverlay";
    }

    @Override
    public void render() {
        SimpleGui.begin("SwoftyMcClient", 10, 10, 250, 300);
        SimpleGui.label("\u00a7aInjection Active");
        SimpleGui.separator();

        if (SimpleGui.button("Send Test Chat")) {
            try {
                ChatUtil.sendChat("\u00a7a[Injected] \u00a7fButton clicked!");
            } catch (Exception e) {}
        }

        SimpleGui.separator();

        if (SimpleGui.button("\u00a7cUnload")) {
            Bootstrap.requestUnload();
        }

        SimpleGui.end();
    }
}
