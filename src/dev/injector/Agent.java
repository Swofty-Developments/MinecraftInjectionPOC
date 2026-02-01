package dev.injector;

import java.lang.instrument.Instrumentation;

public class Agent {

    private static final String HOOK_KEY = "__anticheat_hook__";

    public static void agentmain(String args, Instrumentation inst) {
        try {
            AgentBootstrap.initialize(inst, HOOK_KEY);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public static void premain(String args, Instrumentation inst) {
        agentmain(args, inst);
    }

    public static void destruct() {
        Object hook = System.getProperties().get(HOOK_KEY);
        if (hook instanceof Runnable) {
            ((Runnable) hook).run();
            System.getProperties().remove(HOOK_KEY);
        }
    }
}
