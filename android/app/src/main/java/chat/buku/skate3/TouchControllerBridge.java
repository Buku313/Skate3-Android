package chat.buku.skate3;

final class TouchControllerBridge {
    private TouchControllerBridge() {}

    static native void setState(int buttons,
                                float leftX, float leftY,
                                float rightX, float rightY,
                                float leftTrigger, float rightTrigger);
}
