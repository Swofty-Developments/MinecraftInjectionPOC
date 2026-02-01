package dev.injector.stealth;

public class DestructHandler implements Runnable {

    private final TransformerCoordinator coordinator;

    public DestructHandler(TransformerCoordinator coordinator) {
        this.coordinator = coordinator;
    }

    @Override
    public void run() {
        if (coordinator != null) {
            coordinator.performDestruct();
        }
    }
}
