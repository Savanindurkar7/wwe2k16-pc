package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.graphics.drawable.GradientDrawable;
import android.text.InputType;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.IOException;
import java.util.List;

/** OPTIONS: every setting, grouped in sections. Rows cycle with a tap or the D-pad's left / right. */
public class OptionsActivity extends Activity {
    private static final int REQ_DRIVER_ZIP = 21;

    private String section = Settings.S_GRAPHICS;
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        Ui.init(this);
        build();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    // Landscape only (manifest). Left: the title, the sections and the game files' status. Right: the section's
    // options in an outlined panel over the dimmed background, with HOME at the top right.
    private void build() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);
        ImageView art = new ImageView(this);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        int bg = getResources().getIdentifier("home_bg", "drawable", getPackageName());
        if (bg != 0) art.setImageResource(bg);
        art.setAlpha(0.28f);
        root.addView(art, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        int sideWidth = Math.max(Ui.dp(this, 230), Math.round(getResources().getDisplayMetrics().widthPixels * 0.2f));
        row.addView(sidebar(), new LinearLayout.LayoutParams(sideWidth, ViewGroup.LayoutParams.MATCH_PARENT));
        row.addView(contentArea(), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
        root.addView(row);
        setContentView(root);
    }

    private View sidebar() {
        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, 18);
        side.setPadding(pad, Ui.dp(this, 18), Ui.dp(this, 10), Ui.dp(this, 12));
        side.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0xF2000000, 0xB3000000}));

        SpannableString title = new SpannableString("WWE 2K16");
        title.setSpan(new ForegroundColorSpan(Ui.RED), 4, 8, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView t = Ui.text(this, "", 32, Ui.WHITE, Ui.bold());
        t.setText(title);
        side.addView(t);
        TextView sub = Ui.text(this, "XBOX 360 RECOMPILED", 10.5f, Ui.GREY, Ui.regular());
        sub.setLetterSpacing(0.3f);
        sub.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 14));
        side.addView(sub);

        LinearLayout nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.VERTICAL);
        for (String sec : Settings.SECTIONS) {
            boolean active = sec.equals(section);
            String label = sec.charAt(0) + sec.substring(1).toLowerCase(java.util.Locale.ROOT);
            if (Settings.S_DRIVER.equals(sec)) label = "GPU driver";
            // Compact, so all seven sections fit a phone held sideways without scrolling the menu.
            TextView item = Ui.text(this, label, 14, active ? 0xFF111114 : Ui.WHITE, Ui.semi());
            item.setGravity(Gravity.CENTER_VERTICAL);
            item.setPadding(Ui.dp(this, 14), Ui.dp(this, 5), Ui.dp(this, 8), Ui.dp(this, 5));
            item.setBackground(Ui.navBackground(this, active));
            item.setFocusable(true);
            item.setClickable(true);
            item.setOnClickListener(v -> {
                section = sec;
                build();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = Ui.dp(this, 3);
            nav.addView(item, lp);
            if (active) item.post(item::requestFocus);
        }
        ScrollView navScroll = new ScrollView(this);
        navScroll.setVerticalScrollBarEnabled(false);
        navScroll.addView(nav);
        side.addView(navScroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        // Game files status card.
        SharedPreferences prefs = Settings.prefs(this);
        String path = prefs.getString(Settings.KEY_GAME_PATH, "");
        String problem = GameFiles.checkSaved(path, prefs.getString(Settings.KEY_GAME_KIND, "folder"));
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(Ui.dp(this, 12), Ui.dp(this, 10), Ui.dp(this, 12), Ui.dp(this, 10));
        card.setBackground(Ui.rect(0xCC16161B, Ui.dp(this, 8)));
        TextView ok = Ui.text(this, problem == null ? "✓  Game files ready" : "✕  Game files needed", 14,
                problem == null ? Ui.GREEN : Ui.RED, Ui.semi());
        card.addView(ok);
        TextView where = Ui.body(this, path.isEmpty() ? "None selected" : new java.io.File(path).getName(), 11, Ui.GREY);
        where.setPadding(0, Ui.dp(this, 4), 0, 0);
        where.setMaxLines(2);
        card.addView(where);
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        cardLp.topMargin = Ui.dp(this, 8);
        side.addView(card, cardLp);
        TextView ver = Ui.text(this, BuildInfo.versionName(this), 10.5f, Ui.DIM, Ui.regular());
        ver.setPadding(0, Ui.dp(this, 8), 0, 0);
        side.addView(ver);
        return side;
    }

    private View contentArea() {
        LinearLayout area = new LinearLayout(this);
        area.setOrientation(LinearLayout.VERTICAL);
        area.setPadding(Ui.dp(this, 20), Ui.dp(this, 14), Ui.dp(this, 20), Ui.dp(this, 10));
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        String heading = Settings.S_DRIVER.equals(section) ? "GPU DRIVER" : section;
        TextView h = Ui.text(this, heading, 26, Ui.WHITE, Ui.bold());
        h.setLetterSpacing(0.03f);
        header.addView(h, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (!Settings.S_CREDITS.equals(section)) {
            TextView reset = Ui.smallButton(this, "Reset all", false, v -> new AlertDialog.Builder(this)
                    .setTitle("Reset all options?")
                    .setMessage("Every option goes back to its default. Your game files, drivers and touch layout stay.")
                    .setPositiveButton("Reset", (d, w) -> {
                        Settings.resetAll(this);
                        build();
                    })
                    .setNegativeButton("Cancel", null)
                    .show());
            LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            rl.rightMargin = Ui.dp(this, 8);
            header.addView(reset, rl);
        }
        TextView home = Ui.smallButton(this, "Home", false, v -> finish());
        home.setMinWidth(Ui.dp(this, 120));
        header.addView(home);
        area.addView(header);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        LinearLayout panel = (LinearLayout) sectionContent();
        GradientDrawable panelBg = Ui.rect(0xB3060608, Ui.dp(this, 12));
        panelBg.setStroke(Math.max(1, Ui.dp(this, 1)), 0x33FFFFFF);
        panel.setBackground(panelBg);
        scroll.addView(panel);
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        sl.topMargin = Ui.dp(this, 12);
        area.addView(scroll, sl);
        return area;
    }

    private View sectionContent() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, 18);
        p.setPadding(pad, Ui.dp(this, 8), pad, Ui.dp(this, 18));
        int gap = Ui.dp(this, 8);
        SharedPreferences prefs = Settings.prefs(this);
        if (Settings.S_CREDITS.equals(section)) {
            addCredits(p);
            return p;
        }
        List<Settings.Choice> choices = Settings.inSection(section);
        for (Settings.Choice c : choices) p.addView(choiceRow(c, prefs), Ui.fill(gap));
        if (Settings.S_CONTROLS.equals(section)) {
            p.addView(Ui.menuButton(this, "Edit touch layout", "Move and resize the on-screen controls", true,
                    v -> startActivity(new Intent(this, TouchEditActivity.class))), Ui.fill(Ui.dp(this, 14)));
        }
        if (Settings.S_DRIVER.equals(section)) addDriverRows(p, prefs);
        if (Settings.S_ADVANCED.equals(section)) addExtraArgs(p, prefs);
        if (Settings.S_PERFORMANCE.equals(section)) {
            TextView tip = Ui.body(this, "Best start for most phones: 720p, 30 FPS, texture quality Medium, texture filter "
                    + "2x, fast anti-aliasing off, CPU cores Auto, game speed fix and shader cache on.", 12.5f, Ui.GREY);
            tip.setPadding(0, Ui.dp(this, 16), 0, 0);
            p.addView(tip);
        }
        return p;
    }

    private void addCredits(LinearLayout p) {
        TextView name = Ui.text(this, Credits.PORT_BY, 30, Ui.WHITE, Ui.bold());
        name.setPadding(0, Ui.dp(this, 10), 0, 0);
        p.addView(name);
        TextView role = Ui.body(this, "Port of WWE 2K16 to Android  ·  " + Credits.GITHUB, 14, Ui.GREY);
        role.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 14));
        p.addView(role);
        for (String[] line : Credits.THANKS) {
            LinearLayout r = new LinearLayout(this);
            r.setOrientation(LinearLayout.VERTICAL);
            r.setPadding(Ui.dp(this, 14), Ui.dp(this, 9), Ui.dp(this, 14), Ui.dp(this, 9));
            r.setBackground(Ui.rect(0xCC141419, Ui.dp(this, 4)));
            r.addView(Ui.label(this, line[0], 14));
            TextView d = Ui.body(this, line[1], 12.5f, Ui.GREY);
            d.setPadding(0, Ui.dp(this, 3), 0, 0);
            r.addView(d);
            p.addView(r, Ui.fill(Ui.dp(this, 6)));
        }
        TextView disc = Ui.body(this, Credits.DISCLAIMER, 11.5f, Ui.DIM);
        disc.setPadding(0, Ui.dp(this, 14), 0, 0);
        p.addView(disc);
    }

    /** A setting row: title and value with arrows; the note for the current value below. */
    private View choiceRow(Settings.Choice c, SharedPreferences prefs) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, 14);
        row.setPadding(pad + Ui.dp(this, 4), Ui.dp(this, 11), pad, Ui.dp(this, 11));
        row.setBackground(Ui.menuBackground(this, false));
        row.setFocusable(true);
        row.setClickable(true);

        LinearLayout line = new LinearLayout(this);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = Ui.label(this, c.title, 16);
        line.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView left = Ui.text(this, "◀", 16, Ui.RED, Ui.bold());
        left.setPadding(Ui.dp(this, 12), Ui.dp(this, 4), Ui.dp(this, 10), Ui.dp(this, 4));
        TextView value = Ui.text(this, c.labels[c.index(prefs)], 17, Ui.WHITE, Ui.bold());
        value.setMinWidth(Ui.dp(this, 96));
        value.setGravity(Gravity.CENTER);
        TextView right = Ui.text(this, "▶", 16, Ui.RED, Ui.bold());
        right.setPadding(Ui.dp(this, 10), Ui.dp(this, 4), Ui.dp(this, 4), Ui.dp(this, 4));
        line.addView(left);
        line.addView(value);
        line.addView(right);
        row.addView(line);
        TextView note = Ui.body(this, c.notes[c.index(prefs)], 12, Ui.GREY);
        note.setPadding(0, Ui.dp(this, 5), Ui.dp(this, 8), 0);
        row.addView(note);

        Runnable refresh = () -> {
            int i = c.index(prefs);
            value.setText(c.labels[i]);
            note.setText(c.notes[i]);
        };
        View.OnClickListener next = v -> {
            step(c, prefs, +1);
            refresh.run();
        };
        row.setOnClickListener(next);
        right.setOnClickListener(next);
        left.setOnClickListener(v -> {
            step(c, prefs, -1);
            refresh.run();
        });
        row.setOnKeyListener((v, keyCode, event) -> {
            if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
            if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT) {
                step(c, prefs, -1);
                refresh.run();
                return true;
            }
            if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) {
                step(c, prefs, +1);
                refresh.run();
                return true;
            }
            return false;
        });
        return row;
    }

    private static void step(Settings.Choice c, SharedPreferences prefs, int delta) {
        int n = c.values.length;
        int i = ((c.index(prefs) + delta) % n + n) % n;
        prefs.edit().putString(c.key, c.values[i]).apply();
    }

    // --- GPU driver ------------------------------------------------------------------------------------------------

    private void addDriverRows(LinearLayout p, SharedPreferences prefs) {
        int gap = Ui.dp(this, 8);
        String current = prefs.getString(Settings.KEY_DRIVER, "");
        TextView info = Ui.body(this, "Custom Vulkan drivers (Turnip) can be faster on Snapdragon (Adreno) phones. "
                + "Import a driver zip in the adrenotools format, e.g. from Banners-Turnip or K11MCH1/AdrenoToolsDrivers on "
                + "GitHub. Pick the build for your GPU (A6xx/A7xx, or A8xx for Adreno 8xx). Mali phones (MediaTek, Exynos) "
                + "keep the System driver. If a driver crashes the game, the launcher switches back to System by itself.\n\n"
                + "Measured on an Adreno 810 (Nothing Phone 3a): Turnip A8xx gave about 19 FPS in a match against 6 to 11 "
                + "FPS with the System driver, but it builds shaders more slowly, so the first match stutters more.",
                12.5f, Ui.GREY);
        info.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 6));
        p.addView(info);

        p.addView(driverRow("System driver", "The phone's own Vulkan driver.", current.isEmpty(), v -> {
            prefs.edit().putString(Settings.KEY_DRIVER, "").apply();
            build();
        }, null), Ui.fill(gap));
        for (Drivers.Driver d : Drivers.list(this)) {
            p.addView(driverRow(d.name, d.library + (d.description.isEmpty() ? "" : " · " + d.description),
                    d.name.equals(current), v -> {
                        prefs.edit().putString(Settings.KEY_DRIVER, d.name).apply();
                        build();
                    }, v -> new AlertDialog.Builder(this)
                            .setTitle("Delete " + d.name + "?")
                            .setPositiveButton("Delete", (dlg, w) -> {
                                Drivers.delete(this, d.name);
                                if (d.name.equals(prefs.getString(Settings.KEY_DRIVER, "")))
                                    prefs.edit().putString(Settings.KEY_DRIVER, "").apply();
                                build();
                            })
                            .setNegativeButton("Cancel", null)
                            .show()), Ui.fill(gap));
        }
        p.addView(Ui.menuButton(this, "Import driver (.zip)", "Choose a Turnip / adrenotools driver zip", true, v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ_DRIVER_ZIP);
        }), Ui.fill(Ui.dp(this, 14)));
    }

    private View driverRow(String name, String detail, boolean selected, View.OnClickListener select,
                           View.OnClickListener delete) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int pad = Ui.dp(this, 14);
        row.setPadding(pad + Ui.dp(this, 4), Ui.dp(this, 10), pad, Ui.dp(this, 10));
        row.setBackground(Ui.menuBackground(this, false));
        row.setFocusable(true);
        row.setClickable(true);
        row.setOnClickListener(select);
        TextView mark = Ui.text(this, selected ? "●" : "○", 18, selected ? Ui.RED : Ui.DIM, Ui.bold());
        mark.setPadding(0, 0, Ui.dp(this, 12), 0);
        row.addView(mark);
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(Ui.label(this, name, 16));
        TextView d = Ui.body(this, detail, 12, Ui.GREY);
        d.setPadding(0, Ui.dp(this, 3), 0, 0);
        texts.addView(d);
        row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (delete != null) row.addView(Ui.smallButton(this, "Delete", false, delete));
        return row;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_DRIVER_ZIP || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        Toast.makeText(this, "Importing driver…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                Drivers.Driver d = Drivers.importZip(this, uri);
                handler.post(() -> {
                    Settings.prefs(this).edit().putString(Settings.KEY_DRIVER, d.name).apply();
                    Toast.makeText(this, "Driver \"" + d.name + "\" imported and selected", Toast.LENGTH_LONG).show();
                    build();
                });
            } catch (IOException e) {
                handler.post(() -> Toast.makeText(this, "Import failed: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "import-driver").start();
    }

    // --- Extra arguments -------------------------------------------------------------------------------------------

    private void addExtraArgs(LinearLayout p, SharedPreferences prefs) {
        TextView h = Ui.label(this, "Extra arguments", 16);
        h.setPadding(0, Ui.dp(this, 18), 0, Ui.dp(this, 4));
        p.addView(h);
        TextView info = Ui.body(this, "Separated by spaces. --name=value sets an SDK option (for example "
                + "--vulkan_async_skip_incomplete_frames=true), NAME=value sets an environment variable (for example "
                + "TU_DEBUG=sysmem or WWE2K16_LATE_FLIP=0). They win over the options above.", 12, Ui.GREY);
        p.addView(info);
        EditText edit = new EditText(this);
        edit.setText(prefs.getString(Settings.KEY_EXTRA_ARGS, ""));
        edit.setTextColor(Ui.WHITE);
        edit.setHintTextColor(Ui.DIM);
        edit.setHint("--option=value NAME=value");
        edit.setTypeface(android.graphics.Typeface.MONOSPACE);
        edit.setTextSize(14);
        edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        edit.setBackground(Ui.rect(Ui.ROW, Ui.dp(this, 3)));
        int pad = Ui.dp(this, 12);
        edit.setPadding(pad, pad, pad, pad);
        p.addView(edit, Ui.fill(Ui.dp(this, 8)));
        p.addView(Ui.smallButton(this, "Save arguments", true, v -> {
            prefs.edit().putString(Settings.KEY_EXTRA_ARGS, edit.getText().toString().trim()).apply();
            Toast.makeText(this, "Saved", Toast.LENGTH_SHORT).show();
        }), Ui.fill(Ui.dp(this, 8)));
    }
}
