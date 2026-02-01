package dev.injector;

import java.io.IOException;
import java.lang.instrument.Instrumentation;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.jar.JarFile;

public class AgentBootstrap {

    private static volatile URLClassLoader stealthLoader;

    public static void initialize(Instrumentation inst, String hookKey) throws Exception {
        String agentPath = resolveAgentPath();

        addMinimalBootstrapClasses(inst, agentPath);
        stealthLoader = createStealthClassLoader(agentPath);

        Class<?> coordinatorClass = stealthLoader.loadClass("dev.injector.stealth.TransformerCoordinator");
        Object coordinator = coordinatorClass
                .getMethod("create", Instrumentation.class, String.class)
                .invoke(null, inst, hookKey);

        HookRegistry.setCoordinator(coordinator);
    }

    private static String resolveAgentPath() throws Exception {
        return Agent.class.getProtectionDomain()
                .getCodeSource()
                .getLocation()
                .toURI()
                .getPath();
    }

    private static void addMinimalBootstrapClasses(Instrumentation inst, String agentPath) throws IOException {
        String bootstrapPath = agentPath.replace("agent.jar", "bootstrap.jar");
        inst.appendToBootstrapClassLoaderSearch(new JarFile(bootstrapPath));
    }

    private static URLClassLoader createStealthClassLoader(String agentPath) throws Exception {
        URL jarUrl = new URL("file:" + agentPath);
        return new URLClassLoader(new URL[]{jarUrl}, null);
    }

    public static void releaseClassLoader() {
        if (stealthLoader != null) {
            try {
                stealthLoader.close();
            } catch (IOException e) { }
            stealthLoader = null;
        }
    }
}
