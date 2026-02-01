package dev.injector;

import com.sun.tools.attach.AgentLoadException;
import com.sun.tools.attach.VirtualMachine;
import com.sun.tools.attach.VirtualMachineDescriptor;

import java.io.File;
import java.util.List;

public class Injector {

    private static final String[] MC_SIGNATURES = {
        "minecraft", "Minecraft", "GradleStart", "net.minecraft",
        "cpw.mods.modlauncher", "launchwrapper", "LaunchWrapper",
        "prismlauncher", "EntryPoint", "MultiMC"
    };

    public static void main(String[] args) throws Exception {
        String agentPath = args.length > 0 ? args[0] : "agent.jar";
        File agentFile = new File(agentPath).getAbsoluteFile();

        if (!agentFile.exists()) {
            System.out.println("[!] Agent JAR not found: " + agentFile);
            return;
        }

        System.out.println("[*] Searching for Minecraft JVM...");

        List<VirtualMachineDescriptor> vms = VirtualMachine.list();
        String targetPid = null;
        String targetName = null;

        for (VirtualMachineDescriptor vmd : vms) {
            String name = vmd.displayName();
            for (String sig : MC_SIGNATURES) {
                if (name.toLowerCase().contains(sig.toLowerCase())) {
                    targetPid = vmd.id();
                    targetName = name;
                    break;
                }
            }
            if (targetPid != null) break;
        }

        if (targetPid == null) {
            System.out.println("[!] Minecraft JVM not found. Running JVMs:");
            for (VirtualMachineDescriptor vmd : vms) {
                System.out.println("    PID " + vmd.id() + ": " + vmd.displayName());
            }
            return;
        }

        if (args.length > 1) {
            targetPid = args[1];
            System.out.println("[*] Using manual PID: " + targetPid);
        } else {
            System.out.println("[*] Found Minecraft: " + targetName + " (PID: " + targetPid + ")");
        }

        System.out.println("[*] Attaching to JVM...");
        VirtualMachine vm = VirtualMachine.attach(targetPid);

        System.out.println("[*] Loading agent: " + agentFile.getPath());
        try {
            vm.loadAgent(agentFile.getPath());
        } catch (AgentLoadException e) {
            String msg = e.getMessage();
            if (msg != null && msg.trim().equals("0")) {
                // JDK 21 -> Java 8 protocol mismatch, "0" = success
            } else {
                throw e;
            }
        }

        vm.detach();
        System.out.println("[+] Injected successfully!");
    }
}
