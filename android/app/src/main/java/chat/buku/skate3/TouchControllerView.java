package chat.buku.skate3;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLControllerManager;

import java.util.ArrayList;
import java.util.List;

/** A lightweight multitouch Xbox-style controller layered over the SDL surface. */
public final class TouchControllerView extends View {
    private static final int DPAD_UP = 0x0001;
    private static final int DPAD_DOWN = 0x0002;
    private static final int DPAD_LEFT = 0x0004;
    private static final int DPAD_RIGHT = 0x0008;
    private static final int START = 0x0010;
    private static final int BACK = 0x0020;
    private static final int L3 = 0x0040;
    private static final int R3 = 0x0080;
    private static final int LB = 0x0100;
    private static final int RB = 0x0200;
    private static final int A = 0x1000;
    private static final int B = 0x2000;
    private static final int X = 0x4000;
    private static final int Y = 0x8000;
    private static final String PREFS = "skate3_touch_controller";
    private static final String PREF_VISIBLE = "visible";

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<Control> controls = new ArrayList<>();
    private final SparseArray<Control> pointers = new SparseArray<>();
    private final SharedPreferences preferences;
    private boolean controlsVisible;
    private RectF toggle = new RectF();
    private float lastWidth;
    private float lastHeight;

    private Stick leftStick;
    private Stick rightStick;
    private Trigger leftTrigger;
    private Trigger rightTrigger;

    public TouchControllerView(Context context) {
        super(context);
        setWillNotDraw(false);
        setFocusable(false);
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(dp(2));
        label.setColor(Color.WHITE);
        label.setTextAlign(Paint.Align.CENTER);
        label.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        preferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        controlsVisible = preferences.contains(PREF_VISIBLE)
            ? preferences.getBoolean(PREF_VISIBLE, true)
            : !hasPhysicalController();
    }

    public void clearInput() {
        for (Control control : controls) control.release();
        pointers.clear();
        sendState();
        invalidate();
    }

    public void disconnect() {
        for (Control control : controls) control.release();
        pointers.clear();
        TouchControllerBridge.trySetState(-1, 0, 0, 0, 0, 0, 0);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        sendState();
    }

    @Override
    protected void onDetachedFromWindow() {
        disconnect();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        buildControls(width, height);
    }

    private void buildControls(float width, float height) {
        lastWidth = width;
        lastHeight = height;
        controls.clear();
        float unit = Math.min(width / 16f, height / 9f);
        float stickRadius = unit * 0.78f;
        float buttonRadius = unit * 0.38f;

        leftStick = new Stick("L", width * .16f, height * .73f, stickRadius, true);
        rightStick = new Stick("R", width * .67f, height * .77f, stickRadius, false);
        controls.add(leftStick);
        controls.add(rightStick);

        float dpadX = width * .32f;
        float dpadY = height * .72f;
        controls.add(new Dpad(dpadX, dpadY, unit * .98f));

        float faceX = width * .85f;
        float faceY = height * .68f;
        controls.add(new ButtonControl("A", faceX, faceY + unit * .68f, buttonRadius, A));
        controls.add(new ButtonControl("B", faceX + unit * .68f, faceY, buttonRadius, B));
        controls.add(new ButtonControl("X", faceX - unit * .68f, faceY, buttonRadius, X));
        controls.add(new ButtonControl("Y", faceX, faceY - unit * .68f, buttonRadius, Y));

        float shoulderW = unit * 1.25f;
        float shoulderH = unit * .55f;
        leftTrigger = new Trigger("LT", width * .07f, height * .10f, shoulderW, shoulderH, true);
        rightTrigger = new Trigger("RT", width * .93f, height * .10f, shoulderW, shoulderH, false);
        controls.add(leftTrigger);
        controls.add(new RectButton("LB", width * .20f, height * .10f, shoulderW, shoulderH, LB));
        controls.add(new RectButton("RB", width * .80f, height * .10f, shoulderW, shoulderH, RB));
        controls.add(rightTrigger);

        controls.add(new RectButton("BACK", width * .44f, height * .79f,
                                    unit * .82f, unit * .45f, BACK));
        controls.add(new RectButton("START", width * .55f, height * .79f,
                                    unit * .92f, unit * .45f, START));
        controls.add(new ButtonControl("L3", width * .07f, height * .48f,
                                       buttonRadius * .82f, L3));
        controls.add(new ButtonControl("R3", width * .73f, height * .48f,
                                       buttonRadius * .82f, R3));

        float toggleWidth = controlsVisible ? unit * 1.05f : unit * 1.35f;
        float toggleTop = Math.max(dp(18), height * .035f);
        toggle.set(width * .5f - toggleWidth * .5f, toggleTop,
                   width * .5f + toggleWidth * .5f, toggleTop + unit * .46f);
        sendState();
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        drawToggle(canvas);
        if (!controlsVisible) return;
        for (Control control : controls) control.draw(canvas);
    }

    private void drawToggle(Canvas canvas) {
        fill.setColor(Color.argb(175, 15, 15, 18));
        stroke.setColor(Color.argb(220, 255, 112, 28));
        canvas.drawRoundRect(toggle, dp(9), dp(9), fill);
        canvas.drawRoundRect(toggle, dp(9), dp(9), stroke);
        drawLabel(canvas, controlsVisible ? "HIDE" : "TOUCH", toggle.centerX(),
                  toggle.centerY(), dp(11));
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        int pointerId = event.getPointerId(index);
        float x = event.getX(index);
        float y = event.getY(index);

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            if (toggle.contains(x, y)) {
                setControlsVisible(!controlsVisible);
                return true;
            }
            if (!controlsVisible) return false;
            for (int i = controls.size() - 1; i >= 0; --i) {
                Control control = controls.get(i);
                if (!control.inUse && control.contains(x, y)) {
                    pointers.put(pointerId, control);
                    control.press(x, y);
                    sendState();
                    invalidate();
                    return true;
                }
            }
            return false;
        }

        if (action == MotionEvent.ACTION_MOVE) {
            boolean handled = false;
            for (int i = 0; i < event.getPointerCount(); ++i) {
                Control control = pointers.get(event.getPointerId(i));
                if (control != null) {
                    control.move(event.getX(i), event.getY(i));
                    handled = true;
                }
            }
            if (handled) {
                sendState();
                invalidate();
            }
            return handled;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            Control control = pointers.get(pointerId);
            if (control == null) return false;
            control.release();
            pointers.remove(pointerId);
            sendState();
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_CANCEL) {
            clearInput();
            return true;
        }
        return false;
    }

    private void setControlsVisible(boolean visible) {
        controlsVisible = visible;
        preferences.edit().putBoolean(PREF_VISIBLE, visible).apply();
        for (Control control : controls) control.release();
        pointers.clear();
        if (lastWidth > 0 && lastHeight > 0) buildControls(lastWidth, lastHeight);
        sendState();
        invalidate();
    }

    private void sendState() {
        if (!controlsVisible) {
            TouchControllerBridge.trySetState(-1, 0, 0, 0, 0, 0, 0);
            return;
        }
        int buttons = 0;
        for (Control control : controls) buttons |= control.buttons();
        TouchControllerBridge.trySetState(
            buttons,
            leftStick == null ? 0 : leftStick.xValue,
            leftStick == null ? 0 : leftStick.yValue,
            rightStick == null ? 0 : rightStick.xValue,
            rightStick == null ? 0 : rightStick.yValue,
            leftTrigger != null && leftTrigger.inUse ? 1 : 0,
            rightTrigger != null && rightTrigger.inUse ? 1 : 0);
    }

    private boolean hasPhysicalController() {
        for (int deviceId : InputDevice.getDeviceIds()) {
            if (SDLControllerManager.isDeviceSDLJoystick(deviceId)) return true;
        }
        return false;
    }

    private abstract class Control {
        boolean inUse;
        abstract boolean contains(float x, float y);
        void press(float x, float y) { inUse = true; move(x, y); }
        void move(float x, float y) {}
        void release() { inUse = false; }
        int buttons() { return 0; }
        abstract void draw(Canvas canvas);
    }

    private final class ButtonControl extends Control {
        final String text;
        final float x;
        final float y;
        final float radius;
        final int mask;
        ButtonControl(String text, float x, float y, float radius, int mask) {
            this.text = text; this.x = x; this.y = y; this.radius = radius; this.mask = mask;
        }
        boolean contains(float px, float py) {
            float dx = px - x, dy = py - y;
            return dx * dx + dy * dy <= radius * radius * 1.35f;
        }
        int buttons() { return inUse ? mask : 0; }
        void draw(Canvas canvas) {
            drawCircle(canvas, x, y, radius, inUse);
            drawLabel(canvas, text, x, y, radius * .72f);
        }
    }

    private class RectButton extends Control {
        final String text;
        final RectF bounds;
        final int mask;
        RectButton(String text, float x, float y, float width, float height, int mask) {
            this.text = text;
            this.bounds = new RectF(x - width / 2, y - height / 2,
                                    x + width / 2, y + height / 2);
            this.mask = mask;
        }
        boolean contains(float x, float y) { return bounds.contains(x, y); }
        int buttons() { return inUse ? mask : 0; }
        void draw(Canvas canvas) {
            fill.setColor(inUse ? Color.argb(190, 255, 112, 28) : Color.argb(105, 12, 12, 15));
            stroke.setColor(Color.argb(205, 255, 255, 255));
            canvas.drawRoundRect(bounds, dp(10), dp(10), fill);
            canvas.drawRoundRect(bounds, dp(10), dp(10), stroke);
            drawLabel(canvas, text, bounds.centerX(), bounds.centerY(), dp(11));
        }
    }

    private final class Trigger extends RectButton {
        Trigger(String text, float x, float y, float width, float height, boolean left) {
            super(text, x, y, width, height, 0);
        }
    }

    private final class Stick extends Control {
        final String text;
        final float centerX;
        final float centerY;
        final float radius;
        final boolean left;
        float xValue;
        float yValue;
        Stick(String text, float x, float y, float radius, boolean left) {
            this.text = text; this.centerX = x; this.centerY = y;
            this.radius = radius; this.left = left;
        }
        boolean contains(float x, float y) {
            float dx = x - centerX, dy = y - centerY;
            return dx * dx + dy * dy <= radius * radius * 1.55f;
        }
        void move(float x, float y) {
            float dx = (x - centerX) / radius;
            float dy = (y - centerY) / radius;
            float length = (float) Math.sqrt(dx * dx + dy * dy);
            if (length > 1) { dx /= length; dy /= length; }
            float deadzone = .08f;
            xValue = Math.abs(dx) < deadzone ? 0 : dx;
            yValue = Math.abs(dy) < deadzone ? 0 : -dy;
        }
        void release() { super.release(); xValue = 0; yValue = 0; }
        void draw(Canvas canvas) {
            drawCircle(canvas, centerX, centerY, radius, false);
            float knobX = centerX + xValue * radius * .58f;
            float knobY = centerY - yValue * radius * .58f;
            drawCircle(canvas, knobX, knobY, radius * .43f, inUse);
            drawLabel(canvas, text, knobX, knobY, radius * .34f);
        }
    }

    private final class Dpad extends Control {
        final float centerX;
        final float centerY;
        final float radius;
        int mask;
        Dpad(float x, float y, float radius) { centerX = x; centerY = y; this.radius = radius; }
        boolean contains(float x, float y) {
            return Math.abs(x - centerX) <= radius && Math.abs(y - centerY) <= radius;
        }
        void move(float x, float y) {
            float dx = x - centerX, dy = y - centerY;
            float threshold = radius * .20f;
            mask = 0;
            if (dx < -threshold) mask |= DPAD_LEFT;
            if (dx > threshold) mask |= DPAD_RIGHT;
            if (dy < -threshold) mask |= DPAD_UP;
            if (dy > threshold) mask |= DPAD_DOWN;
        }
        void release() { super.release(); mask = 0; }
        int buttons() { return inUse ? mask : 0; }
        void draw(Canvas canvas) {
            float arm = radius * .36f;
            fill.setColor(inUse ? Color.argb(180, 255, 112, 28) : Color.argb(105, 12, 12, 15));
            stroke.setColor(Color.argb(205, 255, 255, 255));
            RectF vertical = new RectF(centerX - arm, centerY - radius,
                                       centerX + arm, centerY + radius);
            RectF horizontal = new RectF(centerX - radius, centerY - arm,
                                         centerX + radius, centerY + arm);
            canvas.drawRoundRect(vertical, dp(6), dp(6), fill);
            canvas.drawRoundRect(horizontal, dp(6), dp(6), fill);
            canvas.drawRoundRect(vertical, dp(6), dp(6), stroke);
            canvas.drawRoundRect(horizontal, dp(6), dp(6), stroke);
        }
    }

    private void drawCircle(Canvas canvas, float x, float y, float radius, boolean pressed) {
        fill.setColor(pressed ? Color.argb(190, 255, 112, 28) : Color.argb(105, 12, 12, 15));
        stroke.setColor(Color.argb(205, 255, 255, 255));
        canvas.drawCircle(x, y, radius, fill);
        canvas.drawCircle(x, y, radius, stroke);
    }

    private void drawLabel(Canvas canvas, String text, float x, float y, float size) {
        label.setTextSize(size);
        Paint.FontMetrics metrics = label.getFontMetrics();
        canvas.drawText(text, x, y - (metrics.ascent + metrics.descent) / 2, label);
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }
}
