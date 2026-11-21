package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Touch layout editor: drag any control to move it; tap one and use THIS CONTROL to resize just that one, ALL BUTTONS
 * for everything, OPACITY for how see-through they are. DONE saves, CANCEL leaves without saving, RESET restores the
 * defaults.
 */
public class TouchEditActivity extends Activity {
    private TouchControls controls;
    private TouchLayout working;
    private SeekBar thisSize;
    private TextView selectedName;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        Ui.init(this);
        working = TouchLayout.load(this).copy();

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);
        ImageView art = new ImageView(this);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        int bg = getResources().getIdentifier("home_bg", "drawable", getPackageName());
        if (bg != 0) art.setImageResource(bg);
        art.setAlpha(0.35f);
        root.addView(art, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        controls = new TouchControls(this, working, true);
        controls.setEditListener(c -> {
            selectedName.setText(c >= 0 ? "SELECTED: " + TouchLayout.NAMES[c] : "TAP A CONTROL");
            if (c >= 0) thisSize.setProgress(toProgress(working.scale[c]));
        });
        root.addView(controls, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        root.addView(toolbar(), new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        setContentView(root);
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

    private static int toProgress(float scale) { return Math.round((scale - 0.5f) / 1.5f * 100f); }
    private static float fromProgress(int p) { return 0.5f + p / 100f * 1.5f; }

    private View toolbar() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(this, 14);
        box.setPadding(pad, pad, pad, pad);
        GradientDrawable bg = Ui.rect(0xE60B0B0E, Ui.dp(this, 8));
        bg.setStroke(Ui.dp(this, 1), 0x44FFFFFF);
        box.setBackground(bg);

        TextView title = Ui.text(this, "EDIT TOUCH CONTROLS", 18, Ui.WHITE, Ui.bold());
        box.addView(title);
        selectedName = Ui.text(this, "TAP A CONTROL", 12, Ui.RED, Ui.semi());
        selectedName.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 6));
        box.addView(selectedName);

        thisSize = slider(box, "THIS CONTROL", 33, p -> {
            int s = controls.getSelected();
            if (s >= 0) {
                working.scale[s] = fromProgress(p);
                controls.invalidate();
            }
        });
        SeekBar all = slider(box, "ALL BUTTONS", toProgress(working.size), p -> {
            working.size = fromProgress(p);
            controls.invalidate();
        });
        SeekBar opacity = slider(box, "OPACITY", Math.round(working.opacity * 100), p -> {
            working.opacity = Math.max(0.1f, p / 100f);
            controls.invalidate();
        });

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, Ui.dp(this, 10), 0, 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.rightMargin = Ui.dp(this, 6);
        row.addView(Ui.smallButton(this, "Done", true, v -> {
            working.save(this);
            Toast.makeText(this, "Touch layout saved", Toast.LENGTH_SHORT).show();
            finish();
        }), lp);
        row.addView(Ui.smallButton(this, "Cancel", false, v -> finish()), lp);
        row.addView(Ui.smallButton(this, "Reset", false, v -> {
            working = TouchLayout.defaults();
            controls.setLayout(working);
            controls.setSelected(-1);
            all.setProgress(toProgress(working.size));
            opacity.setProgress(Math.round(working.opacity * 100));
            selectedName.setText("TAP A CONTROL");
        }), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(row);
        TextView hint = Ui.body(this, "Drag the controls to move them. The game uses this layout from the next PLAY.",
                11, Ui.GREY);
        hint.setPadding(0, Ui.dp(this, 8), 0, 0);
        box.addView(hint);
        box.setMinimumWidth(Ui.dp(this, 300));
        return box;
    }

    interface Changed {
        void on(int progress);
    }

    private SeekBar slider(LinearLayout box, String label, int initial, Changed changed) {
        TextView l = Ui.label(this, label, 12);
        l.setPadding(0, Ui.dp(this, 6), 0, 0);
        box.addView(l);
        SeekBar s = new SeekBar(this);
        s.setMax(100);
        s.setProgress(Math.max(0, Math.min(100, initial)));
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int p, boolean fromUser) { if (fromUser) changed.on(p); }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        box.addView(s, new LinearLayout.LayoutParams(Ui.dp(this, 280), ViewGroup.LayoutParams.WRAP_CONTENT));
        return s;
    }
}
