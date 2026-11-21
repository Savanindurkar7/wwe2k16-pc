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
        setupTouchControls();
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

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyDisplayMode();
    }

    @Override
    protected String[] getLibraries() {
        // SDL is linked into libmain.so (static), so the game library is the only one to load.
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
