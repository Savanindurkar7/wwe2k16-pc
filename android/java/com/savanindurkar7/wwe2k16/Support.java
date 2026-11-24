package com.savanindurkar7.wwe2k16;

import android.app.Activity;
import android.content.Intent;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * "Support the port" (r19): the porter's Buy Me a Coffee page, as an animated card on the home screen and the credits
 * page. Tapping the card or its button opens https://buymeacoffee.com/catonix; the QR code (res/drawable-nodpi/
 * bmc_qr.png, the porter's own, decodes to the same address) lets someone pay from another phone.
 */
final class Support {
    static final String URL = "https://buymeacoffee.com/catonix";
    private static final int YELLOW = 0xFFFFDD00;

    private Support() {}

    static void open(Activity a) {
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(URL)));
        } catch (Exception e) {
            Toast.makeText(a, URL, Toast.LENGTH_LONG).show();
        }
    }

    /** The animated card. compact = the credits page's smaller version. */
    static View card(Activity a, boolean compact) {
        float radius = Ui.dp(a, 18);
        FrameLayout card = new FrameLayout(a);
        card.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0xFF1C1405, 0xFF120D04, 0xFF0B0803}));
        ((GradientDrawable) card.getBackground()).setCornerRadius(radius);
        card.setClipToOutline(true);
        card.setFocusable(true);
        card.setClickable(true);
        card.setOnClickListener(v -> open(a));

        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int p = Ui.dp(a, compact ? 16 : 20);
        row.setPadding(p, p, p, p);

        TextView cup = Ui.text(a, "☕", compact ? 34 : 44, YELLOW, Ui.bold());
        cup.setPadding(0, 0, Ui.dp(a, 16), 0);
        row.addView(cup);

        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        TextView kicker = Ui.text(a, "SUPPORT THE PORT", 12, YELLOW, Ui.semi());
        kicker.setLetterSpacing(0.24f);
        col.addView(kicker);
        TextView title = Ui.text(a, "Buy Catonix a coffee", compact ? 22 : 27, Ui.WHITE, Ui.bold());
        title.setPadding(0, Ui.dp(a, 4), 0, 0);
        col.addView(title);
        if (!compact) {
            TextView sub = Ui.body(a, "Enjoying WWE 2K16 on Android? A coffee keeps the fixes and updates coming. "
                    + "Tap here, or scan the code with another phone.", 13, 0xFFD9CFB8);
            sub.setPadding(0, Ui.dp(a, 6), Ui.dp(a, 12), 0);
            col.addView(sub);
        }
        TextView button = Ui.text(a, "☕  Buy me a coffee", compact ? 15 : 17, 0xFF1A1200, Ui.bold());
        button.setGravity(Gravity.CENTER);
        button.setPadding(Ui.dp(a, 20), Ui.dp(a, 9), Ui.dp(a, 22), Ui.dp(a, 9));
        button.setBackground(Ui.rect(YELLOW, Ui.dp(a, 22)));
        button.setFocusable(true);
        button.setClickable(true);
        button.setOnClickListener(v -> open(a));
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        bl.topMargin = Ui.dp(a, compact ? 10 : 14);
        col.addView(button, bl);
        row.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        // The QR code on a white tile, with its caption.
        LinearLayout qrCol = new LinearLayout(a);
        qrCol.setOrientation(LinearLayout.VERTICAL);
        qrCol.setGravity(Gravity.CENTER_HORIZONTAL);
        ImageView qr = new ImageView(a);
        int qrId = a.getResources().getIdentifier("bmc_qr", "drawable", a.getPackageName());
        if (qrId != 0) qr.setImageResource(qrId);
        qr.setScaleType(ImageView.ScaleType.FIT_CENTER);
        int tile = Ui.dp(a, compact ? 92 : 118);
        int qp = Ui.dp(a, 7);
        qr.setPadding(qp, qp, qp, qp);
        qr.setBackground(Ui.rect(0xFFFFFFFF, Ui.dp(a, 12)));
        qrCol.addView(qr, new LinearLayout.LayoutParams(tile, tile));
        TextView scan = Ui.text(a, "SCAN TO SUPPORT", 10.5f, 0xFFBFB49A, Ui.semi());
        scan.setLetterSpacing(0.18f);
        scan.setPadding(0, Ui.dp(a, 6), 0, 0);
        qrCol.addView(scan);
        row.addView(qrCol);

        card.addView(row, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        // The light running around the border.
        card.addView(new Motion.GlowBorder(a, radius, Ui.dp(a, 2.2f)), new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        Motion.bob(cup, 4, 2600);
        Motion.breathe(button, 0.045f, 2200);
        return card;
    }
}
