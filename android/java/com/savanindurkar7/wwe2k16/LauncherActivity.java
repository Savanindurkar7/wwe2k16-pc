package com.savanindurkar7.wwe2k16;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Map;

/**
 * The home screen: PLAY, SELECT EXTRACTED GAME FOLDER, SELECT ISO, OPTIONS and LOGS over the background art, with the
 * game files' status. Landscape puts the menu in a panel on the left; portrait puts it under the picture.
 */
public class LauncherActivity extends Activity {
    private static final int REQ_FOLDER = 11;
    private static final int REQ_ISO = 12;
    private static final int REQ_LEGACY_STORAGE = 13;

    private Runnable afterPermission;
    private String page = "home";
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        Ui.init(this);
        checkDriverTrial();
        build();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        build();
        if (afterPermission != null && hasStorageAccess()) {
            Runnable r = afterPermission;
            afterPermission = null;
            r.run();
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        build();
    }

    @Override
    public void onBackPressed() {
        if (!"home".equals(page)) {
            page = "home";
            build();
            return;
        }
        super.onBackPressed();
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    // --- Layout ----------------------------------------------------------------------------------------------------

    private boolean landscape() {
        return getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
    }

    private void build() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);

        // Background art (BG.jpg from the repository, packed by the build); a dark red gradient if it is missing.
        ImageView art = new ImageView(this);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        int bg = getResources().getIdentifier("home_bg", "drawable", getPackageName());
        if (bg != 0) art.setImageResource(bg);
        else art.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {0xFF3A0006, 0xFF000000}));

        boolean land = landscape();
        LinearLayout panel = homeOrPage();
        if (land) {
            root.addView(art, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            // Shade from the left so the menu reads over any picture.
            View shade = new View(this);
            shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                    new int[] {0xF2000000, 0xB3000000, 0x00000000}));
            root.addView(shade, new FrameLayout.LayoutParams(Math.round(getResources().getDisplayMetrics().widthPixels * 0.62f),
                    ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START));
            ScrollView scroll = scroller(panel);
            int width = Math.max(Ui.dp(this, 340), Math.round(getResources().getDisplayMetrics().widthPixels * 0.42f));
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START);
            root.addView(scroll, lp);
        } else {
            LinearLayout column = new LinearLayout(this);
            column.setOrientation(LinearLayout.VERTICAL);
            int artHeight = Math.round(getResources().getDisplayMetrics().widthPixels * 9f / 16f);
            FrameLayout top = new FrameLayout(this);
            top.addView(art, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            View fade = new View(this);
            fade.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] {0x00000000, 0xFF000000}));
            top.addView(fade, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, artHeight / 3, Gravity.BOTTOM));
            column.addView(top, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, artHeight));
            column.addView(scroller(panel), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
            root.addView(column);
        }
        setContentView(root);
        View first = panel.findViewWithTag("first");
        if (first != null) first.requestFocus();
    }

    private ScrollView scroller(View content) {
        ScrollView s = new ScrollView(this);
        s.setFillViewport(true);
        s.setVerticalScrollBarEnabled(false);
        s.addView(content);
        return s;
    }

    private LinearLayout homeOrPage() {
        return "logs".equals(page) ? logsPage() : homePage();
    }

    private LinearLayout column() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, landscape() ? 28 : 20);
        p.setPadding(pad, Ui.dp(this, landscape() ? 26 : 8), pad, Ui.dp(this, 20));
        return p;
    }

    private void addHeader(LinearLayout p) {
        TextView title = Ui.text(this, "WWE 2K16", landscape() ? 46 : 40, Ui.WHITE, Ui.bold());
        title.setLetterSpacing(0.03f);
        p.addView(title);
        TextView sub = Ui.text(this, "XBOX 360 RECOMPILED  ·  ANDROID", 13, Ui.RED, Ui.semi());
        sub.setLetterSpacing(0.18f);
        sub.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 14));
        p.addView(sub);
    }

    private LinearLayout homePage() {
        LinearLayout p = column();
        addHeader(p);

        SharedPreferences prefs = Settings.prefs(this);
        String path = prefs.getString(Settings.KEY_GAME_PATH, "");
        String kind = prefs.getString(Settings.KEY_GAME_KIND, "folder");
        String problem = hasStorageAccess() ? GameFiles.checkSaved(path, kind) : (path.isEmpty()
                ? "Select your game folder or .iso first." : "Allow file access so the game files can be read.");
        boolean ready = problem == null;

        // Status chip.
        TextView status = Ui.text(this, ready ? "GAME FILES READY" : "GAME FILES NEEDED", 13, Ui.WHITE, Ui.semi());
        status.setLetterSpacing(0.1f);
        status.setPadding(Ui.dp(this, 10), Ui.dp(this, 5), Ui.dp(this, 10), Ui.dp(this, 5));
        status.setBackground(Ui.rect(ready ? Ui.alpha(Ui.GREEN, 0.85f) : Ui.alpha(Ui.RED, 0.85f), Ui.dp(this, 3)));
        LinearLayout.LayoutParams chip = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        p.addView(status, chip);
        TextView detail = Ui.body(this, ready ? (("iso".equals(kind) ? "Disc image: " : "Game folder: ") + path) : problem,
                12.5f, Ui.GREY);
        detail.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 14));
        p.addView(detail);

        int gap = Ui.dp(this, 8);
        View play = Ui.menuButton(this, "Play", ready ? "Start WWE 2K16" : "Select your game files first", true, v -> play());
        play.setTag("first");
        p.addView(play, Ui.fill(0));
        p.addView(Ui.menuButton(this, "Select extracted game folder", "The folder with default.xex inside",
                false, v -> withStorageAccess(this::pickFolder)), Ui.fill(gap));
        p.addView(Ui.menuButton(this, "Select ISO", "An Xbox 360 WWE 2K16 disc image (.iso)", false,
                v -> withStorageAccess(this::pickIso)), Ui.fill(gap));
        p.addView(Ui.menuButton(this, "Options", "Graphics, display, performance, GPU driver", false,
                v -> startActivity(new Intent(this, OptionsActivity.class))), Ui.fill(gap));
        p.addView(Ui.menuButton(this, "Logs", "Save or share the logs of the last run", false, v -> {
            page = "logs";
            build();
        }), Ui.fill(gap));

        TextView foot = Ui.body(this, "Version " + BuildInfo.versionName(this)
                + "\nYour own WWE 2K16 (Xbox 360) files are needed: none come with this app."
                + "\nFan-made, not affiliated with WWE, 2K or Yuke's.", 11, Ui.DIM);
        foot.setPadding(0, Ui.dp(this, 18), 0, 0);
        p.addView(foot);
        return p;
    }

    private LinearLayout logsPage() {
        LinearLayout p = column();
        addHeader(p);
        TextView h = Ui.label(this, "Logs", 24);
        p.addView(h);
        TextView info = Ui.body(this, "The zip goes to Download/" + Logs.FOLDER + ". It holds the game log of the last two "
                + "runs, the per-frame log (frames.csv), the settings used, your phone's details and Android's record "
                + "of how the game ended (with the crash trace on Android 12+).", 12.5f, Ui.GREY);
        info.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 12));
        p.addView(info);
        int gap = Ui.dp(this, 8);
        View save = Ui.menuButton(this, "Save logs to Download", null, true, v -> saveLogs(false));
        save.setTag("first");
        p.addView(save, Ui.fill(0));
        p.addView(Ui.menuButton(this, "Share logs", "Save, then send the zip with any app", false, v -> saveLogs(true)),
                Ui.fill(gap));
        p.addView(Ui.menuButton(this, "Clear logs", null, false, v -> {
            Logs.clear(this);
            Toast.makeText(this, "Logs cleared", Toast.LENGTH_SHORT).show();
            build();
        }), Ui.fill(gap));
        p.addView(Ui.menuButton(this, "Back", null, false, v -> {
            page = "home";
            build();
        }), Ui.fill(gap));
        String tail = Logs.tail(this, 12);
        TextView last = Ui.body(this, tail.isEmpty() ? "No game log yet." : "Last lines of the game log:\n" + tail, 10.5f,
                Ui.DIM);
        last.setTypeface(android.graphics.Typeface.MONOSPACE);
        last.setPadding(0, Ui.dp(this, 16), 0, 0);
        p.addView(last);
        return p;
    }

    // --- Actions ---------------------------------------------------------------------------------------------------

    private void saveLogs(boolean share) {
        Toast.makeText(this, "Saving logs…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                Uri uri = Logs.saveZip(this);
                handler.post(() -> {
                    Toast.makeText(this, "Saved in Download/" + Logs.FOLDER, Toast.LENGTH_LONG).show();
                    if (share) {
                        Intent send = new Intent(Intent.ACTION_SEND);
                        send.setType("application/zip");
                        send.putExtra(Intent.EXTRA_STREAM, uri);
                        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        startActivity(Intent.createChooser(send, "Share WWE 2K16 logs"));
                    }
                });
            } catch (IOException e) {
                handler.post(() -> Toast.makeText(this, "Could not save the logs: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "save-logs").start();
    }

    private void play() {
        withStorageAccess(() -> {
            SharedPreferences prefs = Settings.prefs(this);
            String problem = GameFiles.checkSaved(prefs.getString(Settings.KEY_GAME_PATH, ""),
                    prefs.getString(Settings.KEY_GAME_KIND, "folder"));
            if (problem != null) {
                Toast.makeText(this, problem, Toast.LENGTH_LONG).show();
                return;
            }
            Logs.rotate(this);
            Settings.Launch launch = Settings.buildLaunch(this);
            StringBuilder summary = new StringBuilder(launch.summary);
            summary.append("\nArguments:\n");
            for (String a : launch.args) summary.append("  ").append(a).append('\n');
            summary.append("Environment:\n");
            for (Map.Entry<String, String> e : launch.env.entrySet())
                summary.append("  ").append(e.getKey()).append('=').append(e.getValue()).append('\n');
            Logs.writeSettings(this, summary.toString());
            if (launch.env.containsKey("WWE2K16_DRIVER")) writeTrial(prefs.getString(Settings.KEY_DRIVER, ""));

            Intent game = new Intent(this, GameActivity.class);
            game.putStringArrayListExtra(GameActivity.EXTRA_ARGS, launch.args);
            ArrayList<String> env = new ArrayList<>();
            for (Map.Entry<String, String> e : launch.env.entrySet()) env.add(e.getKey() + "=" + e.getValue());
            env.add("WWE2K16_SUSTAINED=" + Settings.SUSTAINED.value(prefs));
            env.add("WWE2K16_DISPLAY=" + Settings.DISPLAY.value(prefs));
            game.putStringArrayListExtra(GameActivity.EXTRA_ENV, env);
            startActivity(game);
        });
    }

    private void writeTrial(String driver) {
        try (FileOutputStream out = new FileOutputStream(Drivers.trialFile(this))) {
            out.write(driver.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    /** A trial file still there means the last run with a custom driver never drew 600 frames: back to the phone's. */
    private void checkDriverTrial() {
        File trial = Drivers.trialFile(this);
        if (!trial.isFile()) return;
        //noinspection ResultOfMethodCallIgnored
        trial.delete();
        SharedPreferences prefs = Settings.prefs(this);
        String driver = prefs.getString(Settings.KEY_DRIVER, "");
        if (driver.isEmpty()) return;
        prefs.edit().putString(Settings.KEY_DRIVER, "").apply();
        Toast.makeText(this, "The last run with the driver \"" + driver + "\" ended before the game got going, so the "
                + "launcher switched back to the phone's driver. Pick it again in Options to retry.", Toast.LENGTH_LONG).show();
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, REQ_FOLDER);
    }

    private void pickIso() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_ISO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        String path, problem, kind;
        if (requestCode == REQ_FOLDER) {
            path = GameFiles.treeUriToPath(uri);
            problem = GameFiles.checkFolder(path);
            kind = "folder";
        } else if (requestCode == REQ_ISO) {
            path = GameFiles.documentUriToPath(uri);
            problem = GameFiles.checkIso(path);
            kind = "iso";
        } else {
            return;
        }
        if (problem != null) {
            Toast.makeText(this, problem, Toast.LENGTH_LONG).show();
            return;
        }
        Settings.prefs(this).edit().putString(Settings.KEY_GAME_PATH, path).putString(Settings.KEY_GAME_KIND, kind).apply();
        Toast.makeText(this, "Game files ready", Toast.LENGTH_SHORT).show();
        build();
    }

    // --- Storage access --------------------------------------------------------------------------------------------

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** The game reads its files by path, which needs "All files access" (Android 11+) or storage permission (10). */
    private void withStorageAccess(Runnable action) {
        if (hasStorageAccess()) {
            action.run();
            return;
        }
        afterPermission = action;
        if (Build.VERSION.SDK_INT >= 30) {
            Toast.makeText(this, "Allow \"All files access\" so the game can read its files, then come back.",
                    Toast.LENGTH_LONG).show();
            try {
                startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE}, REQ_LEGACY_STORAGE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_LEGACY_STORAGE && hasStorageAccess() && afterPermission != null) {
            Runnable r = afterPermission;
            afterPermission = null;
            r.run();
        }
    }
}
