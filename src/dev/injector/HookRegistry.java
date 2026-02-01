package dev.injector;

public class HookRegistry {

    private static volatile Object coordinator;

    public static void setCoordinator(Object coord) {
        coordinator = coord;
    }

    public static Object getCoordinator() {
        return coordinator;
    }

    public static void clearCoordinator() {
        coordinator = null;
    }
}
