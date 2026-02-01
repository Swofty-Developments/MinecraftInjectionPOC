package dev.injector.stealth;

import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.Instrumentation;

public class TransformerCoordinator {

    private final Instrumentation instrumentation;
    private final String hookKey;
    private volatile ClassFileTransformer transformer;
    private volatile DestructHandler destructHandler;

    private TransformerCoordinator(Instrumentation inst, String hookKey) {
        this.instrumentation = inst;
        this.hookKey = hookKey;
    }

    public static TransformerCoordinator create(Instrumentation inst, String hookKey) throws Exception {
        TransformerCoordinator coordinator = new TransformerCoordinator(inst, hookKey);
        coordinator.initialize();
        return coordinator;
    }

    private void initialize() throws Exception {
        destructHandler = new DestructHandler(this);
        System.getProperties().put(hookKey, destructHandler);

        transformer = new OverlayTransformer(instrumentation, new Runnable() {
            @Override
            public void run() {
                ClassFileTransformer t = transformer;
                if (t != null) {
                    instrumentation.removeTransformer(t);
                }
            }
        });
        instrumentation.addTransformer(transformer, true);

        retransformTargetClasses();
    }

    private void retransformTargetClasses() {
        for (Class<?> clazz : instrumentation.getAllLoadedClasses()) {
            if (OverlayTransformer.isTargetClass(clazz.getName())
                    && instrumentation.isModifiableClass(clazz)) {
                try {
                    instrumentation.retransformClasses(clazz);
                } catch (Exception e) {
                    e.printStackTrace();
                }
                break;
            }
        }
    }

    public void performDestruct() {
        if (transformer != null) {
            ((OverlayTransformer) transformer).deactivate();
            instrumentation.addTransformer(transformer, true);
            restoreOriginalBytecode();
            instrumentation.removeTransformer(transformer);
            transformer = null;
        }

        System.getProperties().remove(hookKey);
        destructHandler = null;
        notifyClassLoaderRelease();
    }

    private void restoreOriginalBytecode() {
        for (Class<?> clazz : instrumentation.getAllLoadedClasses()) {
            if (OverlayTransformer.isTargetClass(clazz.getName())
                    && instrumentation.isModifiableClass(clazz)) {
                try {
                    instrumentation.retransformClasses(clazz);
                } catch (Exception e) { }
                break;
            }
        }
    }

    private void notifyClassLoaderRelease() {
        try {
            Class<?> bootstrapClass = Class.forName("dev.injector.AgentBootstrap",
                    true, ClassLoader.getSystemClassLoader());
            bootstrapClass.getMethod("releaseClassLoader").invoke(null);

            Class<?> registryClass = Class.forName("dev.injector.HookRegistry",
                    true, ClassLoader.getSystemClassLoader());
            registryClass.getMethod("clearCoordinator").invoke(null);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
