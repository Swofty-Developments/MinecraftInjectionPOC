package client.rendering;

import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;
import java.util.function.Consumer;

/**
 * Simple immediate-mode GUI using raw OpenGL calls.
 * Works with LWJGL 2 (Minecraft 1.8.9) without external dependencies.
 */
public class SimpleGui {

    // Colors (ARGB format)
    private static final int COLOR_WINDOW_BG = 0xE6141414;
    private static final int COLOR_TITLE_BAR = 0xFF1A3D1A;
    private static final int COLOR_TITLE_BAR_ACTIVE = 0xFF2D5A2D;
    private static final int COLOR_BUTTON = 0xFF2D5A2D;
    private static final int COLOR_BUTTON_HOVER = 0xFF3D7A3D;
    private static final int COLOR_BUTTON_ACTIVE = 0xFF1A3D1A;
    private static final int COLOR_CHECKBOX_BG = 0xFF1F1F1F;
    private static final int COLOR_CHECKBOX_CHECK = 0xFF4CAF50;
    private static final int COLOR_SLIDER_BG = 0xFF1F1F1F;
    private static final int COLOR_SLIDER_FILL = 0xFF4CAF50;
    private static final int COLOR_SLIDER_KNOB = 0xFFFFFFFF;
    private static final int COLOR_TEXT = 0xFFFFFFFF;
    private static final int COLOR_BORDER = 0xFF333333;

    // Layout constants
    private static final int TITLE_BAR_HEIGHT = 24;
    private static final int PADDING = 8;
    private static final int ITEM_SPACING = 6;
    private static final int BUTTON_HEIGHT = 24;
    private static final int CHECKBOX_SIZE = 18;
    private static final int SLIDER_HEIGHT = 18;

    // Current window state
    private static String currentWindow = null;
    private static int windowX, windowY, windowWidth, windowHeight;
    private static int cursorX, cursorY;

    // Window positions (persistent)
    private static final Map<String, int[]> windowPositions = new HashMap<>();

    // Dragging state
    private static String draggingWindow = null;
    private static int dragOffsetX, dragOffsetY;

    // Input state (updated each frame)
    private static int mouseX, mouseY;
    private static boolean mouseDown, mouseClicked;
    private static boolean prevMouseDown = false;

    // Font renderer (obtained via reflection)
    private static Object fontRenderer;
    private static Method drawStringMethod;
    private static Method getStringWidthMethod;

    // GL methods (obtained via reflection)
    private static Method glEnableMethod, glDisableMethod;
    private static Method glColorMethod;
    private static Method glBeginMethod, glEndMethod, glVertexMethod;
    private static Method glPushMatrixMethod, glPopMatrixMethod;
    private static Method glBlendFuncMethod;
    private static Method glLineWidthMethod;

    // Visibility
    private static boolean visible = true;

    // Collapsing headers
    private static final Map<String, Boolean> collapsedSections = new HashMap<>();

    public static void initialize(Object mcFontRenderer, ClassLoader mcClassLoader) {
        fontRenderer = mcFontRenderer;

        try {
            Class<?> frClass = mcFontRenderer.getClass();

            for (String name : new String[]{"drawStringWithShadow", "func_175063_a", "a"}) {
                try {
                    drawStringMethod = frClass.getMethod(name, String.class, float.class, float.class, int.class);
                    break;
                } catch (NoSuchMethodException e) {}
                try {
                    drawStringMethod = frClass.getMethod(name, String.class, int.class, int.class, int.class);
                    break;
                } catch (NoSuchMethodException e) {}
            }

            for (String name : new String[]{"getStringWidth", "func_78256_a", "a"}) {
                try {
                    getStringWidthMethod = frClass.getMethod(name, String.class);
                    break;
                } catch (NoSuchMethodException e) {}
            }

            Class<?> gl11 = Class.forName("org.lwjgl.opengl.GL11");
            glEnableMethod = gl11.getMethod("glEnable", int.class);
            glDisableMethod = gl11.getMethod("glDisable", int.class);
            glColorMethod = gl11.getMethod("glColor4f", float.class, float.class, float.class, float.class);
            glBeginMethod = gl11.getMethod("glBegin", int.class);
            glEndMethod = gl11.getMethod("glEnd");
            glVertexMethod = gl11.getMethod("glVertex2f", float.class, float.class);
            glPushMatrixMethod = gl11.getMethod("glPushMatrix");
            glPopMatrixMethod = gl11.getMethod("glPopMatrix");
            glBlendFuncMethod = gl11.getMethod("glBlendFunc", int.class, int.class);
            glLineWidthMethod = gl11.getMethod("glLineWidth", float.class);

            System.out.println("[SimpleGui] Initialized");
        } catch (Exception e) {
            System.err.println("[SimpleGui] Init failed: " + e.getMessage());
            e.printStackTrace();
        }
    }

    public static void shutdown() {
        currentWindow = null;
        draggingWindow = null;
        windowPositions.clear();
        collapsedSections.clear();
        prevMouseDown = false;
        mouseDown = false;
        mouseClicked = false;
        visible = true;
        fontRenderer = null;
        drawStringMethod = null;
        getStringWidthMethod = null;
        glEnableMethod = null;
        glDisableMethod = null;
        glColorMethod = null;
        glBeginMethod = null;
        glEndMethod = null;
        glVertexMethod = null;
        glPushMatrixMethod = null;
        glPopMatrixMethod = null;
        glBlendFuncMethod = null;
        glLineWidthMethod = null;
    }

    public static void updateInput() {
        try {
            Class<?> mouseClass = Class.forName("org.lwjgl.input.Mouse");
            Method getX = mouseClass.getMethod("getX");
            Method getY = mouseClass.getMethod("getY");
            Method isButtonDown = mouseClass.getMethod("isButtonDown", int.class);

            mouseX = (int) getX.invoke(null);

            Class<?> displayClass = Class.forName("org.lwjgl.opengl.Display");
            Method getHeight = displayClass.getMethod("getHeight");
            int displayHeight = (int) getHeight.invoke(null);
            mouseY = displayHeight - (int) getY.invoke(null);

            boolean currentMouseDown = (boolean) isButtonDown.invoke(null, 0);
            mouseClicked = currentMouseDown && !prevMouseDown;
            mouseDown = currentMouseDown;
            prevMouseDown = currentMouseDown;
        } catch (Exception e) {}
    }

    public static void toggleVisible() { visible = !visible; }
    public static void setVisible(boolean v) { visible = v; }
    public static boolean isVisible() { return visible; }

    // =========================================================================
    // Window
    // =========================================================================

    public static void begin(String title, int defaultX, int defaultY, int width, int height) {
        if (!visible) return;

        currentWindow = title;
        windowWidth = width;
        windowHeight = height;

        int[] pos = windowPositions.get(title);
        if (pos == null) {
            pos = new int[]{defaultX, defaultY};
            windowPositions.put(title, pos);
        }
        windowX = pos[0];
        windowY = pos[1];

        boolean overTitleBar = mouseX >= windowX && mouseX <= windowX + windowWidth &&
                              mouseY >= windowY && mouseY <= windowY + TITLE_BAR_HEIGHT;

        if (overTitleBar && mouseClicked && draggingWindow == null) {
            draggingWindow = title;
            dragOffsetX = mouseX - windowX;
            dragOffsetY = mouseY - windowY;
        }

        if (draggingWindow != null && draggingWindow.equals(title)) {
            if (mouseDown) {
                pos[0] = mouseX - dragOffsetX;
                pos[1] = mouseY - dragOffsetY;
                windowX = pos[0];
                windowY = pos[1];
            } else {
                draggingWindow = null;
            }
        }

        setupGL();
        drawRect(windowX, windowY, windowX + windowWidth, windowY + windowHeight, COLOR_WINDOW_BG);
        drawRectOutline(windowX, windowY, windowX + windowWidth, windowY + windowHeight, COLOR_BORDER);

        int titleBarColor = (draggingWindow != null && draggingWindow.equals(title)) ?
                           COLOR_TITLE_BAR_ACTIVE : COLOR_TITLE_BAR;
        drawRect(windowX, windowY, windowX + windowWidth, windowY + TITLE_BAR_HEIGHT, titleBarColor);
        drawString(title, windowX + PADDING, windowY + (TITLE_BAR_HEIGHT - 8) / 2, COLOR_TEXT);

        cursorX = windowX + PADDING;
        cursorY = windowY + TITLE_BAR_HEIGHT + PADDING;
    }

    public static void end() {
        if (!visible || currentWindow == null) return;
        restoreGL();
        currentWindow = null;
    }

    // =========================================================================
    // Widgets
    // =========================================================================

    public static void label(String text) {
        if (!visible || currentWindow == null) return;
        drawString(text, cursorX, cursorY, COLOR_TEXT);
        cursorY += 12 + ITEM_SPACING;
    }

    public static void label(String text, int color) {
        if (!visible || currentWindow == null) return;
        drawString(text, cursorX, cursorY, color);
        cursorY += 12 + ITEM_SPACING;
    }

    public static boolean button(String text) {
        if (!visible || currentWindow == null) return false;

        int buttonWidth = windowWidth - PADDING * 2;
        int x = cursorX;
        int y = cursorY;

        boolean hovered = isMouseOver(x, y, buttonWidth, BUTTON_HEIGHT);
        boolean clicked = false;

        int color;
        if (hovered && mouseDown) {
            color = COLOR_BUTTON_ACTIVE;
        } else if (hovered) {
            color = COLOR_BUTTON_HOVER;
            if (mouseClicked) clicked = true;
        } else {
            color = COLOR_BUTTON;
        }

        drawRect(x, y, x + buttonWidth, y + BUTTON_HEIGHT, color);

        int textWidth = getStringWidth(text);
        int textX = x + (buttonWidth - textWidth) / 2;
        int textY = y + (BUTTON_HEIGHT - 8) / 2;
        drawString(text, textX, textY, COLOR_TEXT);

        cursorY += BUTTON_HEIGHT + ITEM_SPACING;
        return clicked;
    }

    public static void checkbox(String text, boolean value, Consumer<Boolean> onChange) {
        if (!visible || currentWindow == null) return;

        int x = cursorX;
        int y = cursorY;
        boolean hovered = isMouseOver(x, y, CHECKBOX_SIZE, CHECKBOX_SIZE);

        drawRect(x, y, x + CHECKBOX_SIZE, y + CHECKBOX_SIZE, COLOR_CHECKBOX_BG);
        if (value) {
            int pad = 4;
            drawRect(x + pad, y + pad, x + CHECKBOX_SIZE - pad, y + CHECKBOX_SIZE - pad, COLOR_CHECKBOX_CHECK);
        }
        if (hovered) {
            drawRectOutline(x, y, x + CHECKBOX_SIZE, y + CHECKBOX_SIZE, COLOR_TEXT);
        }

        drawString(text, x + CHECKBOX_SIZE + 6, y + (CHECKBOX_SIZE - 8) / 2, COLOR_TEXT);

        if (hovered && mouseClicked && onChange != null) {
            onChange.accept(!value);
        }
        cursorY += CHECKBOX_SIZE + ITEM_SPACING;
    }

    public static void slider(String text, float value, float min, float max, Consumer<Float> onChange) {
        if (!visible || currentWindow == null) return;

        int sliderWidth = windowWidth - PADDING * 2;
        int x = cursorX;
        int y = cursorY;

        drawString(text + ": " + String.format("%.1f", value), x, y, COLOR_TEXT);
        y += 14;

        drawRect(x, y, x + sliderWidth, y + SLIDER_HEIGHT, COLOR_SLIDER_BG);
        float percent = (value - min) / (max - min);
        int fillWidth = (int) (sliderWidth * percent);
        drawRect(x, y, x + fillWidth, y + SLIDER_HEIGHT, COLOR_SLIDER_FILL);

        int knobX = x + fillWidth - 3;
        drawRect(knobX, y, knobX + 6, y + SLIDER_HEIGHT, COLOR_SLIDER_KNOB);

        boolean hovered = isMouseOver(x, y, sliderWidth, SLIDER_HEIGHT);
        if (hovered && mouseDown && onChange != null) {
            float newPercent = Math.max(0, Math.min(1, (float)(mouseX - x) / sliderWidth));
            onChange.accept(min + newPercent * (max - min));
        }
        cursorY = y + SLIDER_HEIGHT + ITEM_SPACING;
    }

    public static void spacing(int pixels) {
        if (!visible || currentWindow == null) return;
        cursorY += pixels;
    }

    public static void separator() {
        if (!visible || currentWindow == null) return;
        int y = cursorY + ITEM_SPACING / 2;
        drawRect(cursorX, y, cursorX + windowWidth - PADDING * 2, y + 1, COLOR_BORDER);
        cursorY += ITEM_SPACING;
    }

    public static boolean collapsingHeader(String text) {
        if (!visible || currentWindow == null) return false;

        String id = currentWindow + "_header_" + text;
        Boolean collapsed = collapsedSections.get(id);
        if (collapsed == null) collapsed = false;

        int x = cursorX;
        int y = cursorY;
        int width = windowWidth - PADDING * 2;

        boolean hovered = isMouseOver(x, y, width, BUTTON_HEIGHT);
        int color = hovered ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
        drawRect(x, y, x + width, y + BUTTON_HEIGHT, color);

        drawString(collapsed ? ">" : "v", x + 4, y + (BUTTON_HEIGHT - 8) / 2, COLOR_TEXT);
        drawString(text, x + 16, y + (BUTTON_HEIGHT - 8) / 2, COLOR_TEXT);

        if (hovered && mouseClicked) {
            collapsed = !collapsed;
            collapsedSections.put(id, collapsed);
        }

        cursorY += BUTTON_HEIGHT + ITEM_SPACING;
        return !collapsed;
    }

    // =========================================================================
    // Drawing helpers
    // =========================================================================

    private static void setupGL() {
        try {
            glPushMatrixMethod.invoke(null);
            glEnableMethod.invoke(null, 3042); // GL_BLEND
            glBlendFuncMethod.invoke(null, 770, 771);
            glDisableMethod.invoke(null, 3553); // GL_TEXTURE_2D
        } catch (Exception e) {}
    }

    private static void restoreGL() {
        try {
            glEnableMethod.invoke(null, 3553);
            glPopMatrixMethod.invoke(null);
        } catch (Exception e) {}
    }

    private static void drawRect(int x1, int y1, int x2, int y2, int color) {
        try {
            float a = (color >> 24 & 255) / 255.0f;
            float r = (color >> 16 & 255) / 255.0f;
            float g = (color >> 8 & 255) / 255.0f;
            float b = (color & 255) / 255.0f;

            glDisableMethod.invoke(null, 3553);
            glColorMethod.invoke(null, r, g, b, a);
            glBeginMethod.invoke(null, 7); // GL_QUADS
            glVertexMethod.invoke(null, (float)x1, (float)y2);
            glVertexMethod.invoke(null, (float)x2, (float)y2);
            glVertexMethod.invoke(null, (float)x2, (float)y1);
            glVertexMethod.invoke(null, (float)x1, (float)y1);
            glEndMethod.invoke(null);
            glEnableMethod.invoke(null, 3553);
        } catch (Exception e) {}
    }

    private static void drawRectOutline(int x1, int y1, int x2, int y2, int color) {
        try {
            float a = (color >> 24 & 255) / 255.0f;
            float r = (color >> 16 & 255) / 255.0f;
            float g = (color >> 8 & 255) / 255.0f;
            float b = (color & 255) / 255.0f;

            glDisableMethod.invoke(null, 3553);
            glColorMethod.invoke(null, r, g, b, a);
            glLineWidthMethod.invoke(null, 1.0f);
            glBeginMethod.invoke(null, 2); // GL_LINE_LOOP
            glVertexMethod.invoke(null, (float)x1, (float)y1);
            glVertexMethod.invoke(null, (float)x2, (float)y1);
            glVertexMethod.invoke(null, (float)x2, (float)y2);
            glVertexMethod.invoke(null, (float)x1, (float)y2);
            glEndMethod.invoke(null);
            glEnableMethod.invoke(null, 3553);
        } catch (Exception e) {}
    }

    private static void drawString(String text, int x, int y, int color) {
        if (fontRenderer == null || drawStringMethod == null) return;
        try {
            glEnableMethod.invoke(null, 3553);
            glColorMethod.invoke(null, 1f, 1f, 1f, 1f);
            if (drawStringMethod.getParameterTypes()[1] == float.class) {
                drawStringMethod.invoke(fontRenderer, text, (float)x, (float)y, color);
            } else {
                drawStringMethod.invoke(fontRenderer, text, x, y, color);
            }
        } catch (Exception e) {}
    }

    private static int getStringWidth(String text) {
        if (fontRenderer == null || getStringWidthMethod == null) return text.length() * 6;
        try { return (int) getStringWidthMethod.invoke(fontRenderer, text); }
        catch (Exception e) { return text.length() * 6; }
    }

    private static boolean isMouseOver(int x, int y, int width, int height) {
        return mouseX >= x && mouseX <= x + width && mouseY >= y && mouseY <= y + height;
    }
}
