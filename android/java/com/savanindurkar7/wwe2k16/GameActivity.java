package com.savanindurkar7.wwe2k16;

import android.app.AlertDialog;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.InputDevice;
import android.view.ViewGroup;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;

/**
 * Runs the game (libmain.so: the recompiled game, the ReXGlue runtime and SDL) in its own process (":game"), so every
 * PLAY starts from a clean process and leaving the game can never leave half-shut-down native state behind.
 *
 * The launcher passes the game's command line (EXTRA_ARGS) and environment (EXTRA_ENV, "NAME=value"); the environment
 * is set here before SDL loads the library.
 */
public class GameActivity extends SDLActivity {
    public static final String EXTRA_ARGS = "wwe2k16.args";
    public static final String EXTRA_ENV = "wwe2k16.env";
    private static final String TAG = "WWE2K16";

    private boolean windowed;
    private TouchControls touch;
    private String touchMode = "auto";  // auto | on | off
    private boolean touchShown;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private InputManager inputManager;
    private Vibrator vibrator;
    private int lastRumble;

    private final InputManager.InputDeviceListener padListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { updateTouchVisibility(); }
        @Override public void onInputDeviceRemoved(int id) { updateTouchVisibility(); }
        @Override public void onInputDeviceChanged(int id) { updateTouchVisibility(); }
    };

    // The game's rumble, played on the phone's vibrator (WWE2K16_RUMBLE=1).
    private final Runnable rumblePoll = new Runnable() {
        @Override public void run() {
            if (touch != null && touchShown && vibrator != null) {
                int r = 0;
                try {
                    r = TouchControls.nativeGetRumble();
                } catch (UnsatisfiedLinkError ignored) {
                }
                int strength = Math.max(r >>> 16, r & 0xFFFF);
                if (strength > 2000 && lastRumble <= 2000) {
                    vibrator.vibrate(VibrationEffect.createOneShot(120, Math.max(1, Math.min(255, strength >> 8))));
                } else if (strength <= 2000 && lastRumble > 2000) {
                    vibrator.cancel();
                }
                lastRumble = strength;
            }
            handler.postDelayed(this, 100);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (savedInstanceState != null) {
            // Android rebuilt this activity after killing the game in the background: the game cannot resume, so end the
            // process and let the launcher come back.
            finish();
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
        applyEnvironment();
        // Landscape only: SDL may otherwise pick the orientation from its window size or its own hint.
        setenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight");
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if ("1".equals(System.getenv("WWE2K16_SUSTAINED"))) {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null && pm.isSustainedPerformanceModeSupported()) {
                getWindow().setSustainedPerformanceMode(true);
                Log.i(TAG, "sustained performance mode on");
            }
        }
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = windowed ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        applyDisplayMode();
        applyRefreshRate();
        setupTouchControls();
        setupFpsLabel();
    }

    // --- FPS label with frame generation ----------------------------------------------------------------------------
    // The game's own FPS counter is drawn by the SDK's UI layer, and while that layer has anything to draw every frame is
    // painted on Android's UI thread, which frame generation cannot keep up with (about 30 paints a second in total).
    // With frame generation on, the counter is this label instead (src/fps_overlay.cpp leaves the SDK's out), so the
    // game presents from its own thread and the in-between frames go out on time.

    static native long nativeGameFrames();

    private TextView fpsLabel;
    private long fpsLastFrames = -1, fpsLastMs;
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            if (fpsLabel == null) return;
            long now = android.os.SystemClock.uptimeMillis();
            long frames;
            try { frames = nativeGameFrames(); } catch (UnsatisfiedLinkError e) { return; }
            if (fpsLastFrames >= 0 && now > fpsLastMs) {
                float fps = (frames - fpsLastFrames) * 1000f / (now - fpsLastMs);
                fpsLabel.setText(String.format(java.util.Locale.US, "%.0f FPS  FG", fps));
            }
            fpsLastFrames = frames;
            fpsLastMs = now;
            handler.postDelayed(this, 500);
        }
    };

    private void setupFpsLabel() {
        if (!"1".equals(System.getenv("WWE2K16_FRAMEGEN"))) return;
        String monitor = System.getenv("WWE2K16_FRAME_MONITOR");
        if (monitor == null || "0".equals(monitor)) return;
        fpsLabel = new TextView(this);
        fpsLabel.setTextColor(Color.rgb(242, 245, 250));
        fpsLabel.setTextSize(13);
        fpsLabel.setBackgroundColor(Color.argb(210, 20, 23, 28));
        int pad = (int) (6 * getResources().getDisplayMetrics().density);
        fpsLabel.setPadding(pad * 2, pad, pad * 2, pad);
        fpsLabel.setText("-- FPS  FG");
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END);
        lp.topMargin = pad * 2;
        lp.rightMargin = pad * 3;
        addContentView(fpsLabel, lp);
        handler.postDelayed(fpsTick, 1000);
    }

    // --- Touch controls -------------------------------------------------------------------------------------------

    private void setupTouchControls() {
        String mode = System.getenv("WWE2K16_TOUCH");
        if (mode != null && !mode.isEmpty()) touchMode = mode;
        if ("off".equals(touchMode)) return;
        touch = new TouchControls(this, TouchLayout.load(this), false);
        touch.setVibrate(!"0".equals(System.getenv("WWE2K16_TOUCH_VIBRATE")));
        touch.setNativeReady(true);
        addContentView(touch, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(padListener, handler);
        if ("1".equals(System.getenv("WWE2K16_RUMBLE"))) {
            vibrator = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            if (vibrator != null && vibrator.hasVibrator()) handler.postDelayed(rumblePoll, 500);
            else vibrator = null;
        }
        updateTouchVisibility();
    }

    private static boolean gamepadConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return true;
        }
        return false;
    }

    private void updateTouchVisibility() {
        if (touch == null) return;
        boolean show = "on".equals(touchMode) || !gamepadConnected();
        if (show == touchShown) return;
        touchShown = show;
        touch.setVisibility(show ? View.VISIBLE : View.GONE);
        if (!show) touch.releaseAll();
        try {
            TouchControls.nativeSetEnabled(show);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "touch controls: game library not ready", e);
        }
    }

    private void applyEnvironment() {
        setenv("HOME", getFilesDir().getAbsolutePath());
        // The 0.10 SDK's SDL_main makes this the working folder (its logs\ and config file go there).
        setenv("WWE2K14_FILES_DIR", getFilesDir().getAbsolutePath());
        setenv("TMPDIR", getCacheDir().getAbsolutePath());
        setenv("WWE2K16_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir + "/");
        setenv("WWE2K16_TMP_DIR", getCacheDir().getAbsolutePath() + "/");
        ArrayList<String> env = getIntent().getStringArrayListExtra(EXTRA_ENV);
        if (env != null) {
            for (String entry : env) {
                int eq = entry.indexOf('=');
                if (eq > 0) setenv(entry.substring(0, eq), entry.substring(eq + 1));
            }
        }
        windowed = "windowed".equals(System.getenv("WWE2K16_DISPLAY"));
    }

    private static void setenv(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv " + name + " failed", e);
        }
    }

    private void applyDisplayMode() {
        View decor = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c == null) return;
            if (windowed) {
                c.show(WindowInsets.Type.systemBars());
            } else {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else if (!windowed) {
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    /**
     * Frame generation (WWE2K16_FRAMEGEN=1) shows up to twice the game's frames: it needs the screen's fastest mode
     * (90 / 120 Hz), which Android does not pick for a game by itself. The same resolution, the highest refresh rate.
     */
    private void applyRefreshRate() {
        if (!"1".equals(System.getenv("WWE2K16_FRAMEGEN")) || Build.VERSION.SDK_INT < 23) return;
        try {
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode();
            android.view.Display.Mode best = current;
            for (android.view.Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() == current.getPhysicalWidth()
                        && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) {
                    best = m;
                }
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            lp.preferredRefreshRate = best.getRefreshRate();
            getWindow().setAttributes(lp);
            Log.i(TAG, "frame generation: display mode " + best.getModeId() + " (" + best.getRefreshRate() + " Hz)");
        } catch (RuntimeException e) {
            Log.e(TAG, "frame generation: could not set the refresh rate", e);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyDisplayMode();
    }

    @Override
    protected String[] getLibraries() {
        // Classic APK: SDL is linked into libmain.so (static), the game library is the only one to load. Native APK
        // (WWE 2K14 SDK): libmain.so links the shared C++ library and the SDK runtime (SDL is inside the runtime); the
        // GPU plugin (librexgpu-xenos.so) is loaded by the runtime itself.
        if (Settings.isNativeBuild(this)) return new String[] {"c++_shared", "rexruntime", "main"};
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        ArrayList<String> args = getIntent().getStringArrayListExtra(EXTRA_ARGS);
        return args != null ? args.toArray(new String[0]) : new String[0];
    }

    @Override
    public void onBackPressed() {
        new AlertDialog.Builder(this)
                .setTitle("Quit to the launcher?")
                .setMessage("Unsaved progress in the current match is lost.")
                .setPositiveButton("Quit", (d, w) -> finish())
                .setNegativeButton("Keep playing", null)
                .show();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (touch != null) touch.releaseAll();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(padListener);
        super.onDestroy();
        // The launcher (below this activity) comes back; this process ends so the next PLAY starts clean.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
