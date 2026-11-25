package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen controller in the Xbox 360 layout: two sticks, a d-pad, coloured A/B/X/Y, LB/RB/LT/RT, BACK, START and
 * L3/R3. In play mode it tracks every finger and sends the pad state to the game (nativeSetPad, src/touch_input.cpp);
 * in edit mode (TouchEditActivity) fingers move and select controls instead.
 */
public class TouchControls extends View {
    // XInput button bits.
    static final int DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008, START = 0x0010,
            BACK = 0x0020, LTHUMB = 0x0040, RTHUMB = 0x0080, LSHOULDER = 0x0100, RSHOULDER = 0x0200, BTN_A = 0x1000,
            BTN_B = 0x2000, BTN_X = 0x4000, BTN_Y = 0x8000;

    static native void nativeSetPad(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);
    static native void nativeSetEnabled(boolean on);
    static native int nativeGetRumble();

    interface EditListener {
        void onSelect(int control);
    }

    private TouchLayout layout;
    private final boolean editMode;
    private boolean nativeReady;
    private boolean vibrate = true;
    private EditListener editListener;
    private int selected = -1;

    // Per pointer: which control it holds (-1 none), and for sticks / edit drags the touch start.
    private final int[] pointerControl = new int[16];
    private final float[] dragOffsetX = new float[16], dragOffsetY = new float[16];

    // Current pad state.
    private int buttons;
    private float lx, ly, rx, ry, lt, rt;
    private final float[] knobX = new float[2], knobY = new float[2];  // stick knob offsets in px
    private int dpadDir;  // DPAD_* bits held

    // r21: every button stays pressed for the game at least this long. A quick tap (finger down and up in 20-40 ms)
    // could start and end between two of the game's controller reads, so the game never saw it ("A button not
    // pressing properly", 1.2.1 testers). The game reads the pad once a frame (17-33 ms).
    private static final long MIN_PRESS_MS = 70;
    private final long[] pressedAt = new long[16];   // per button bit index: when it went down (uptime ms)
    private int lastFingers;                         // the buttons fingers pressed at the last update
    private final Runnable releaseHeld = this::releaseHeldButtons;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Path path = new Path();
    private final float density;

    public TouchControls(Context context, TouchLayout layout, boolean editMode) {
        super(context);
        this.layout = layout;
        this.editMode = editMode;
        density = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        for (int i = 0; i < pointerControl.length; i++) pointerControl[i] = -1;
        setHapticFeedbackEnabled(true);
    }

    void setNativeReady(boolean ready) { nativeReady = ready; }
    void setVibrate(boolean on) { vibrate = on; }
    void setEditListener(EditListener l) { editListener = l; }
    void setLayout(TouchLayout l) { layout = l; invalidate(); }
    TouchLayout getLayout() { return layout; }
    int getSelected() { return selected; }

    void setSelected(int control) {
        selected = control;
        invalidate();
    }

    // --- Geometry ---------------------------------------------------------------------------------------------------

    private float cx(int i) { return layout.x[i] * getWidth(); }
    private float cy(int i) { return layout.y[i] * getHeight(); }
    private float radius(int i) { return layout.sizeDp(i) * density / 2f; }

    private boolean isRound(int i) {
        return i != TouchLayout.LB && i != TouchLayout.RB && i != TouchLayout.LT && i != TouchLayout.RT
                && i != TouchLayout.BACK && i != TouchLayout.START;
    }

    /** The control under (x, y), with a generous margin; sticks and the d-pad win over small buttons near them. */
    private int hit(float x, float y) {
        // r21: a finger inside a face button's own circle always presses it, even when a stick's (bigger) touch area
        // overlaps it in a custom layout.
        for (int i = TouchLayout.A; i <= TouchLayout.Y; i++) {
            float r = radius(i);
            float dx = x - cx(i), dy = y - cy(i);
            if (dx * dx + dy * dy <= r * r) return i;
        }
        int best = -1;
        float bestScore = Float.MAX_VALUE;
        for (int i = 0; i < TouchLayout.COUNT; i++) {
            float r = radius(i) * (i == TouchLayout.LSTICK || i == TouchLayout.RSTICK ? 1.35f : 1.25f);
            float dx = x - cx(i), dy = y - cy(i);
            float d2 = dx * dx + dy * dy;
            if (!isRound(i)) {
                // Shoulder / BACK / START: a wide pill.
                float hw = radius(i) * 1.3f, hh = radius(i) * 0.75f;
                if (Math.abs(dx) <= hw * 1.2f && Math.abs(dy) <= hh * 1.4f) d2 = (dx * dx + dy * dy) * 0.5f;
                else continue;
            } else if (d2 > r * r) {
                continue;
            }
            float score = d2 / (r * r);
            if (score < bestScore) {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    // --- Touch ------------------------------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(index);
                if (id >= pointerControl.length) break;
                float x = e.getX(index), y = e.getY(index);
                int c = hit(x, y);
                pointerControl[id] = c;
                if (c >= 0) {
                    if (editMode) {
                        dragOffsetX[id] = cx(c) - x;
                        dragOffsetY[id] = cy(c) - y;
                        setSelected(c);
                        if (editListener != null) editListener.onSelect(c);
                    } else if (vibrate) {
                        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int p = 0; p < e.getPointerCount(); p++) {
                    int id = e.getPointerId(p);
                    if (id >= pointerControl.length) continue;
                    int c = pointerControl[id];
                    float x = e.getX(p), y = e.getY(p);
                    if (editMode) {
                        if (c >= 0) {
                            layout.x[c] = clamp01((x + dragOffsetX[id]) / getWidth());
                            layout.y[c] = clamp01((y + dragOffsetY[id]) / getHeight());
                        }
                    } else if (c >= TouchLayout.A && c <= TouchLayout.Y) {
                        // Sliding between face buttons (a common wrestling-game habit: A to B for reversals).
                        int now = hit(x, y);
                        if (now >= TouchLayout.A && now <= TouchLayout.Y && now != c) {
                            pointerControl[id] = now;
                            if (vibrate) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                        }
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < pointerControl.length; i++) pointerControl[i] = -1;
                } else {
                    int id = e.getPointerId(index);
                    if (id < pointerControl.length) pointerControl[id] = -1;
                }
                if (editMode && editListener != null) editListener.onSelect(selected);
                break;
            }
            default:
                break;
        }
        if (!editMode) updateState(e, action, index);
        invalidate();
        return true;
    }

    private static float clamp01(float v) { return Math.max(0.03f, Math.min(0.97f, v)); }

    /** Rebuilds the pad state from every finger still down and sends it when it changed. */
    private void updateState(MotionEvent e, int action, int upIndex) {
        int newButtons = 0;
        float nlx = 0, nly = 0, nrx = 0, nry = 0, nlt = 0, nrt = 0;
        knobX[0] = knobY[0] = knobX[1] = knobY[1] = 0;
        dpadDir = 0;
        boolean ending = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP;
        for (int p = 0; p < e.getPointerCount(); p++) {
            if (ending && p == upIndex) continue;
            if (action == MotionEvent.ACTION_CANCEL) break;
            int id = e.getPointerId(p);
            if (id >= pointerControl.length) continue;
            int c = pointerControl[id];
            if (c < 0) continue;
            float dx = e.getX(p) - cx(c), dy = e.getY(p) - cy(c);
            switch (c) {
                case TouchLayout.LSTICK:
                case TouchLayout.RSTICK: {
                    float r = radius(c) * 0.62f;
                    float len = (float) Math.hypot(dx, dy);
                    if (len > r) {
                        dx *= r / len;
                        dy *= r / len;
                    }
                    int s = c == TouchLayout.LSTICK ? 0 : 1;
                    knobX[s] = dx;
                    knobY[s] = dy;
                    float ax = dx / r, ay = -dy / r;  // up = +1
                    if (s == 0) { nlx = ax; nly = ay; } else { nrx = ax; nry = ay; }
                    break;
                }
                case TouchLayout.DPAD: {
                    float dead = radius(c) * 0.18f;
                    if (Math.hypot(dx, dy) < dead) break;
                    double angle = Math.toDegrees(Math.atan2(-dy, dx));  // 0 = right, 90 = up
                    if (angle < 0) angle += 360;
                    // 8 directions, 45 degrees each.
                    int sector = (int) Math.floor((angle + 22.5) / 45.0) % 8;
                    final int[] dirs = {DPAD_RIGHT, DPAD_RIGHT | DPAD_UP, DPAD_UP, DPAD_UP | DPAD_LEFT, DPAD_LEFT,
                            DPAD_LEFT | DPAD_DOWN, DPAD_DOWN, DPAD_DOWN | DPAD_RIGHT};
                    dpadDir |= dirs[sector];
                    break;
                }
                case TouchLayout.A: newButtons |= BTN_A; break;
                case TouchLayout.B: newButtons |= BTN_B; break;
                case TouchLayout.X: newButtons |= BTN_X; break;
                case TouchLayout.Y: newButtons |= BTN_Y; break;
                case TouchLayout.LB: newButtons |= LSHOULDER; break;
                case TouchLayout.RB: newButtons |= RSHOULDER; break;
                case TouchLayout.LT: nlt = 1f; break;
                case TouchLayout.RT: nrt = 1f; break;
                case TouchLayout.BACK: newButtons |= BACK; break;
                case TouchLayout.START: newButtons |= START; break;
                case TouchLayout.L3: newButtons |= LTHUMB; break;
                case TouchLayout.R3: newButtons |= RTHUMB; break;
                default: break;
            }
        }
        newButtons |= dpadDir;
        newButtons = withMinimumPress(newButtons);
        if (newButtons != buttons || nlx != lx || nly != ly || nrx != rx || nry != ry || nlt != lt || nrt != rt) {
            buttons = newButtons;
            lx = nlx; ly = nly; rx = nrx; ry = nry; lt = nlt; rt = nrt;
            send();
        }
    }

    /** Keeps a just-released button down until it has been pressed for MIN_PRESS_MS. */
    private int withMinimumPress(int fingers) {
        long now = android.os.SystemClock.uptimeMillis();
        int out = fingers;
        long wait = 0;
        for (int bit = 0; bit < 16; bit++) {
            int m = 1 << bit;
            if ((fingers & m) != 0 && (lastFingers & m) == 0) pressedAt[bit] = now;
            if ((fingers & m) == 0 && (buttons & m) != 0) {
                long left = pressedAt[bit] + MIN_PRESS_MS - now;
                if (left > 0) {
                    out |= m;
                    wait = Math.max(wait, left);
                }
            }
        }
        lastFingers = fingers;
        removeCallbacks(releaseHeld);
        if (wait > 0) postDelayed(releaseHeld, wait + 1);
        return out;
    }

    /** The minimum press time of a tapped button is over: send the pad without it. */
    private void releaseHeldButtons() {
        int next = withMinimumPress(lastFingers);
        if (next != buttons) {
            buttons = next;
            send();
            invalidate();
        }
    }

    void send() {
        if (!nativeReady) return;
        try {
            nativeSetPad(buttons, lx, ly, rx, ry, lt, rt);
        } catch (UnsatisfiedLinkError ignored) {
            nativeReady = false;
        }
    }

    void releaseAll() {
        for (int i = 0; i < pointerControl.length; i++) pointerControl[i] = -1;
        removeCallbacks(releaseHeld);
        lastFingers = 0;
        buttons = 0;
        lx = ly = rx = ry = lt = rt = 0;
        knobX[0] = knobY[0] = knobX[1] = knobY[1] = 0;
        dpadDir = 0;
        send();
        invalidate();
    }

    // --- Drawing ----------------------------------------------------------------------------------------------------

    private boolean held(int c) {
        for (int pc : pointerControl) if (pc == c) return true;
        return false;
    }

    private int a(float base) {
        return Math.round(255 * Math.min(1f, base * layout.opacity / 0.6f));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        for (int i = 0; i < TouchLayout.COUNT; i++) drawControl(canvas, i);
    }

    private void drawControl(Canvas c, int i) {
        float x = cx(i), y = cy(i), r = radius(i);
        boolean down = held(i) && !editMode;
        stroke.setStrokeWidth(2.2f * density);
        switch (i) {
            case TouchLayout.LSTICK:
            case TouchLayout.RSTICK: {
                int s = i == TouchLayout.LSTICK ? 0 : 1;
                fill.setColor(Color.argb(a(0.35f), 10, 10, 14));
                c.drawCircle(x, y, r, fill);
                stroke.setColor(Color.argb(a(0.55f), 255, 255, 255));
                c.drawCircle(x, y, r, stroke);
                fill.setColor(Color.argb(a(down ? 0.85f : 0.6f), 60, 62, 70));
                c.drawCircle(x + knobX[s], y + knobY[s], r * 0.42f, fill);
                stroke.setColor(Color.argb(a(0.7f), 255, 255, 255));
                c.drawCircle(x + knobX[s], y + knobY[s], r * 0.42f, stroke);
                break;
            }
            case TouchLayout.DPAD: {
                float arm = r * 0.34f;
                path.reset();
                path.addRoundRect(x - arm, y - r, x + arm, y + r, arm * 0.4f, arm * 0.4f, Path.Direction.CW);
                path.addRoundRect(x - r, y - arm, x + r, y + arm, arm * 0.4f, arm * 0.4f, Path.Direction.CW);
                fill.setColor(Color.argb(a(0.55f), 30, 31, 36));
                c.drawPath(path, fill);
                stroke.setColor(Color.argb(a(0.55f), 255, 255, 255));
                c.drawPath(path, stroke);
                // Highlight the held direction(s).
                fill.setColor(Color.argb(a(0.75f), 255, 255, 255));
                float k = r * 0.62f;
                if ((dpadDir & DPAD_UP) != 0) c.drawCircle(x, y - k, arm * 0.55f, fill);
                if ((dpadDir & DPAD_DOWN) != 0) c.drawCircle(x, y + k, arm * 0.55f, fill);
                if ((dpadDir & DPAD_LEFT) != 0) c.drawCircle(x - k, y, arm * 0.55f, fill);
                if ((dpadDir & DPAD_RIGHT) != 0) c.drawCircle(x + k, y, arm * 0.55f, fill);
                break;
            }
            case TouchLayout.A: faceButton(c, x, y, r, "A", Color.rgb(96, 186, 70), down); break;
            case TouchLayout.B: faceButton(c, x, y, r, "B", Color.rgb(222, 48, 44), down); break;
            case TouchLayout.X: faceButton(c, x, y, r, "X", Color.rgb(40, 120, 214), down); break;
            case TouchLayout.Y: faceButton(c, x, y, r, "Y", Color.rgb(240, 186, 30), down); break;
            case TouchLayout.LB: pill(c, x, y, r, "LB", down); break;
            case TouchLayout.RB: pill(c, x, y, r, "RB", down); break;
            case TouchLayout.LT: pill(c, x, y, r, "LT", down); break;
            case TouchLayout.RT: pill(c, x, y, r, "RT", down); break;
            case TouchLayout.BACK: pill(c, x, y, r, "BACK", down); break;
            case TouchLayout.START: pill(c, x, y, r, "START", down); break;
            case TouchLayout.L3: faceButton(c, x, y, r, "L3", Color.rgb(200, 200, 205), down); break;
            case TouchLayout.R3: faceButton(c, x, y, r, "R3", Color.rgb(200, 200, 205), down); break;
            default: break;
        }
        if (editMode && i == selected) {
            stroke.setColor(Ui.RED);
            stroke.setStrokeWidth(3f * density);
            if (isRound(i)) c.drawCircle(x, y, r + 6 * density, stroke);
            else {
                rect.set(x - r * 1.3f - 6 * density, y - r * 0.75f - 6 * density, x + r * 1.3f + 6 * density,
                        y + r * 0.75f + 6 * density);
                c.drawRoundRect(rect, r, r, stroke);
            }
        }
    }

    private void faceButton(Canvas c, float x, float y, float r, String label, int color, boolean down) {
        fill.setColor(Color.argb(a(down ? 0.8f : 0.45f), 12, 12, 16));
        c.drawCircle(x, y, r, fill);
        stroke.setColor(Color.argb(a(down ? 1f : 0.8f), Color.red(color), Color.green(color), Color.blue(color)));
        c.drawCircle(x, y, r, stroke);
        if (down) {
            fill.setColor(Color.argb(a(0.45f), Color.red(color), Color.green(color), Color.blue(color)));
            c.drawCircle(x, y, r, fill);
        }
        text.setColor(Color.argb(a(1f), Color.red(color), Color.green(color), Color.blue(color)));
        text.setTextSize(label.length() > 1 ? r * 0.75f : r * 1.0f);
        c.drawText(label, x, y - (text.descent() + text.ascent()) / 2f, text);
    }

    private void pill(Canvas c, float x, float y, float r, String label, boolean down) {
        float hw = r * 1.3f, hh = r * 0.75f;
        rect.set(x - hw, y - hh, x + hw, y + hh);
        fill.setColor(Color.argb(a(down ? 0.85f : 0.45f), down ? 70 : 12, down ? 72 : 12, down ? 80 : 16));
        c.drawRoundRect(rect, hh, hh, fill);
        stroke.setColor(Color.argb(a(0.7f), 255, 255, 255));
        c.drawRoundRect(rect, hh, hh, stroke);
        text.setColor(Color.argb(a(1f), 255, 255, 255));
        text.setTextSize(label.length() > 2 ? hh * 0.75f : hh * 0.95f);
        c.drawText(label, x, y - (text.descent() + text.ascent()) / 2f, text);
    }
}
