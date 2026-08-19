package chat.buku.skate3;

final class TouchControllerBridge {
    private static volatile boolean nativeAvailable = true;

    private TouchControllerBridge() {}

    static native void setState(int buttons,
                                float leftX, float leftY,
                                float rightX, float rightY,
                                float leftTrigger, float rightTrigger);

    static void trySetState(int buttons,
                            float leftX, float leftY,
                            float rightX, float rightY,
                            float leftTrigger, float rightTrigger) {
        if (!nativeAvailable) return;
        try {
            setState(buttons, leftX, leftY, rightX, rightY,
                     leftTrigger, rightTrigger);
        } catch (UnsatisfiedLinkError error) {
            nativeAvailable = false;
        }
    }
}
