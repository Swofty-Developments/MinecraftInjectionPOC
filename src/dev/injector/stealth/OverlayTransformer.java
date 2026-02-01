package dev.injector.stealth;

import org.objectweb.asm.*;

import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.Instrumentation;
import java.security.ProtectionDomain;

public class OverlayTransformer implements ClassFileTransformer {

    private static final String[] TARGET_CLASSES = {
        "avo",
        "net/minecraft/client/gui/GuiIngame",
        "net/minecraft/client/gui/hud/InGameHud"
    };

    private static final String[] METHOD_NAMES = {
        "a", "renderGameOverlay", "func_175180_a"
    };

    private static final String METHOD_DESC = "(F)V";

    private final Instrumentation instrumentation;
    private final Runnable onTransformComplete;
    private volatile boolean isActive = true;
    private volatile boolean hasTransformed = false;

    private String mcClass = "ave";
    private String frClass = "avn";
    private String srClass = "avr";
    private String getMc = "A";
    private String frField = "k";
    private String drawStrName = "a";
    private String strWidthName = "a";
    private String srWidthName = "a";

    public OverlayTransformer(Instrumentation instrumentation, Runnable onTransformComplete) {
        this.instrumentation = instrumentation;
        this.onTransformComplete = onTransformComplete;
    }

    public static boolean isTargetClass(String className) {
        if (className == null) return false;
        String slashName = className.replace('.', '/');
        for (String target : TARGET_CLASSES) {
            if (slashName.equals(target)) return true;
        }
        return false;
    }

    public void deactivate() {
        isActive = false;
    }

    @Override
    public byte[] transform(ClassLoader loader, String className, Class<?> classBeingRedefined,
                            ProtectionDomain protectionDomain, byte[] classfileBuffer) {
        if (className == null) return null;

        boolean isTarget = false;
        for (String target : TARGET_CLASSES) {
            if (className.equals(target)) { isTarget = true; break; }
        }
        if (!isTarget) return null;

        if (!isActive) return null;

        if (className.equals("net/minecraft/client/gui/GuiIngame")) {
            mcClass = "net/minecraft/client/Minecraft";
            frClass = "net/minecraft/client/gui/FontRenderer";
            srClass = "net/minecraft/client/gui/ScaledResolution";
            getMc = "func_71410_x";
            frField = "field_71466_p";
            drawStrName = "func_175063_a";
            strWidthName = "func_78256_a";
            srWidthName = "func_78326_a";
        }

        try {
            ClassReader cr = new ClassReader(classfileBuffer);
            ClassWriter cw = new ClassWriter(cr, ClassWriter.COMPUTE_MAXS);

            ClassVisitor cv = new ClassVisitor(Opcodes.ASM5, cw) {
                @Override
                public MethodVisitor visitMethod(int access, String name, String desc,
                                                 String signature, String[] exceptions) {
                    MethodVisitor mv = super.visitMethod(access, name, desc, signature, exceptions);
                    if (!desc.equals(METHOD_DESC)) return mv;

                    boolean match = false;
                    for (String mName : METHOD_NAMES) {
                        if (name.equals(mName)) { match = true; break; }
                    }
                    if (!match) return mv;

                    return new MethodVisitor(Opcodes.ASM5, mv) {
                        @Override
                        public void visitInsn(int opcode) {
                            if (opcode == Opcodes.RETURN) {
                                injectRenderCode(mv);
                            }
                            super.visitInsn(opcode);
                        }
                    };
                }
            };

            cr.accept(cv, 0);
            byte[] result = cw.toByteArray();

            if (!hasTransformed) {
                hasTransformed = true;
                if (onTransformComplete != null) {
                    onTransformComplete.run();
                }
            }

            return result;

        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    private void injectRenderCode(MethodVisitor mv) {
        Label tryStart = new Label();
        Label tryEnd = new Label();
        Label catchHandler = new Label();
        Label skipDestruct = new Label();

        mv.visitTryCatchBlock(tryStart, tryEnd, catchHandler, "java/lang/Throwable");
        mv.visitLabel(tryStart);

        mv.visitIntInsn(Opcodes.BIPUSH, 54);
        mv.visitMethodInsn(Opcodes.INVOKESTATIC, "org/lwjgl/input/Keyboard",
                "isKeyDown", "(I)Z", false);
        mv.visitJumpInsn(Opcodes.IFEQ, skipDestruct);

        mv.visitMethodInsn(Opcodes.INVOKESTATIC, "dev/injector/Agent",
                "destruct", "()V", false);
        mv.visitInsn(Opcodes.RETURN);

        mv.visitLabel(skipDestruct);

        mv.visitMethodInsn(Opcodes.INVOKESTATIC, mcClass, getMc,
                "()L" + mcClass + ";", false);
        mv.visitVarInsn(Opcodes.ASTORE, 10);

        mv.visitVarInsn(Opcodes.ALOAD, 10);
        mv.visitFieldInsn(Opcodes.GETFIELD, mcClass, frField,
                "L" + frClass + ";");
        mv.visitVarInsn(Opcodes.ASTORE, 11);

        mv.visitTypeInsn(Opcodes.NEW, srClass);
        mv.visitInsn(Opcodes.DUP);
        mv.visitVarInsn(Opcodes.ALOAD, 10);
        mv.visitMethodInsn(Opcodes.INVOKESPECIAL, srClass, "<init>",
                "(L" + mcClass + ";)V", false);
        mv.visitVarInsn(Opcodes.ASTORE, 12);

        mv.visitVarInsn(Opcodes.ALOAD, 12);
        mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, srClass, srWidthName,
                "()I", false);
        mv.visitVarInsn(Opcodes.ISTORE, 13);

        mv.visitVarInsn(Opcodes.ALOAD, 11);
        mv.visitLdcInsn("Injected");
        mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, frClass, strWidthName,
                "(Ljava/lang/String;)I", false);
        mv.visitVarInsn(Opcodes.ISTORE, 14);

        mv.visitVarInsn(Opcodes.ILOAD, 13);
        mv.visitVarInsn(Opcodes.ILOAD, 14);
        mv.visitInsn(Opcodes.ISUB);
        mv.visitInsn(Opcodes.I2F);
        mv.visitLdcInsn(2.0f);
        mv.visitInsn(Opcodes.FDIV);

        mv.visitVarInsn(Opcodes.ALOAD, 11);
        mv.visitInsn(Opcodes.SWAP);
        mv.visitLdcInsn("Injected");
        mv.visitInsn(Opcodes.SWAP);
        mv.visitLdcInsn(4.0f);
        mv.visitLdcInsn(0xFF00FF00);
        mv.visitMethodInsn(Opcodes.INVOKEVIRTUAL, frClass, drawStrName,
                "(Ljava/lang/String;FFI)I", false);
        mv.visitInsn(Opcodes.POP);

        mv.visitLabel(tryEnd);
        Label afterCatch = new Label();
        mv.visitJumpInsn(Opcodes.GOTO, afterCatch);

        mv.visitLabel(catchHandler);
        mv.visitInsn(Opcodes.POP);
        mv.visitLabel(afterCatch);
    }
}
