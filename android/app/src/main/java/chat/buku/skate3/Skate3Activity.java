package chat.buku.skate3;

import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

public class Skate3Activity extends SDLActivity {
    private static final String INPUT_TAG = "Skate3Input";
    private TouchControllerView touchController;

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "rexruntime", "skate3" };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String files = getFilesDir().getAbsolutePath();
        nativeSetenv("XDG_DATA_HOME", files);
        nativeSetenv("HOME", files);
        touchController = new TouchControllerView(this);
        mLayout.addView(touchController, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    protected void onPause() {
        if (touchController != null) touchController.clearInput();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (touchController != null) touchController.disconnect();
        super.onDestroy();
    }

    private static int getAllSources(int deviceId, int eventSource) {
        InputDevice device = InputDevice.getDevice(deviceId);
        return device == null ? eventSource : eventSource | device.getSources();
    }

    private static boolean isControllerEvent(int deviceId, int eventSource) {
        int sources = getAllSources(deviceId, eventSource);
        return (sources & (InputDevice.SOURCE_GAMEPAD |
                           InputDevice.SOURCE_JOYSTICK |
                           InputDevice.SOURCE_DPAD)) != 0;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int deviceId = event.getDeviceId();
        if (isControllerEvent(deviceId, event.getSource())) {
            boolean handled = false;
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                handled = SDLControllerManager.onNativePadDown(deviceId, event.getKeyCode());
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                handled = SDLControllerManager.onNativePadUp(deviceId, event.getKeyCode());
            }
            Log.i(INPUT_TAG, "key action=" + event.getAction() +
                    " code=" + event.getKeyCode() + " device=" + deviceId +
                    " eventSources=0x" + Integer.toHexString(event.getSource()) +
                    " allSources=0x" + Integer.toHexString(getAllSources(deviceId, event.getSource())) +
                    " handled=" + handled);
            if (handled) {
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isControllerEvent(event.getDeviceId(), event.getSource()) &&
                SDLControllerManager.handleJoystickMotionEvent(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

}
