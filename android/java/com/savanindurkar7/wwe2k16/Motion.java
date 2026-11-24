package com.savanindurkar7.wwe2k16;

import android.animation.Animator;
import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.SweepGradient;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.AccelerateDecelerateInterpolator;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.LinearInterpolator;

import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's motion (r19): small, smooth animations that make the menu feel alive without changing its look —
 * a slow pan-and-zoom on the hero art, a breathing PLAY button, cards that slide in when a page opens, and the glowing
 * border of the support card. All run on the render thread (view properties), cost almost nothing, stop when the
 * screen is left, and follow Android's "remove animations" setting.
 */
final class Motion {
    private Motion() {}

    /** Every animator started for the current page, cancelled when the page is rebuilt or left. */
    private static final List<Animator> running = new ArrayList<>();

    static void stopAll() {
        for (Animator a : running) a.cancel();
        running.clear();
    }

    private static <T extends Animator> T keep(T a) {
        running.add(a);
        return a;
    }

    /** Hero art: a slow zoom and drift (the "Ken Burns" look) over `ms`, alternating direction each slide. */
    static void kenBurns(View v, long ms, boolean reverse) {
        v.setPivotX(reverse ? v.getWidth() * 0.3f : v.getWidth() * 0.7f);
        v.setPivotY(v.getHeight() * 0.45f);
        float from = reverse ? 1.10f : 1.0f, to = reverse ? 1.0f : 1.10f;
        float dx = Ui.dp(v.getContext(), 18);
        ObjectAnimator sx = ObjectAnimator.ofFloat(v, View.SCALE_X, from, to);
        ObjectAnimator sy = ObjectAnimator.ofFloat(v, View.SCALE_Y, from, to);
        ObjectAnimator tx = ObjectAnimator.ofFloat(v, View.TRANSLATION_X, reverse ? dx : -dx, reverse ? -dx : dx);
        AnimatorSet set = new AnimatorSet();
        set.playTogether(sx, sy, tx);
        set.setDuration(ms);
        set.setInterpolator(new LinearInterpolator());
        keep(set).start();
    }

    /** A gentle, endless breathing scale (the PLAY button, the coffee cup). */
    static void breathe(View v, float amount, long periodMs) {
        ObjectAnimator sx = ObjectAnimator.ofFloat(v, View.SCALE_X, 1f, 1f + amount);
        ObjectAnimator sy = ObjectAnimator.ofFloat(v, View.SCALE_Y, 1f, 1f + amount);
        for (ObjectAnimator a : new ObjectAnimator[] {sx, sy}) {
            a.setRepeatCount(ValueAnimator.INFINITE);
            a.setRepeatMode(ValueAnimator.REVERSE);
            a.setDuration(periodMs / 2);
            a.setInterpolator(new AccelerateDecelerateInterpolator());
        }
        AnimatorSet set = new AnimatorSet();
        set.playTogether(sx, sy);
        keep(set).start();
    }

    /** A soft halo behind a button: grows and fades out, over and over (the PLAY button's "ready" pulse). */
    static void halo(View halo, long periodMs) {
        ObjectAnimator sx = ObjectAnimator.ofFloat(halo, View.SCALE_X, 1f, 1.18f);
        ObjectAnimator sy = ObjectAnimator.ofFloat(halo, View.SCALE_Y, 1f, 1.32f);
        ObjectAnimator al = ObjectAnimator.ofFloat(halo, View.ALPHA, 0.55f, 0f);
        for (ObjectAnimator a : new ObjectAnimator[] {sx, sy, al}) {
            a.setRepeatCount(ValueAnimator.INFINITE);
            a.setDuration(periodMs);
            a.setInterpolator(new DecelerateInterpolator());
        }
        AnimatorSet set = new AnimatorSet();
        set.playTogether(sx, sy, al);
        keep(set).start();
    }

    /** A small up-and-down float (the coffee cup's steam feel). */
    static void bob(View v, float dp, long periodMs) {
        ObjectAnimator a = ObjectAnimator.ofFloat(v, View.TRANSLATION_Y, 0f, -Ui.dp(v.getContext(), dp));
        a.setRepeatCount(ValueAnimator.INFINITE);
        a.setRepeatMode(ValueAnimator.REVERSE);
        a.setDuration(periodMs / 2);
        a.setInterpolator(new AccelerateDecelerateInterpolator());
        keep(a).start();
    }

    /** The children of `group` fade in and slide up one after another (a page opening). */
    static void staggerIn(ViewGroup group, long firstDelayMs) {
        float dy = Ui.dp(group.getContext(), 22);
        for (int i = 0; i < group.getChildCount(); i++) {
            View c = group.getChildAt(i);
            c.setAlpha(0f);
            c.setTranslationY(dy);
            c.animate().alpha(1f).translationY(0f).setStartDelay(firstDelayMs + i * 70L).setDuration(380)
                    .setInterpolator(new DecelerateInterpolator(1.6f)).start();
        }
    }

    /** A view that draws a rounded border whose light runs around it (the support card's "live" frame). */
    static final class GlowBorder extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Matrix matrix = new Matrix();
        private final RectF rect = new RectF();
        private final float radius, stroke;
        private float angle;
        private SweepGradient gradient;

        GlowBorder(Context c, float radiusPx, float strokePx) {
            super(c);
            radius = radiusPx;
            stroke = strokePx;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokePx);
            ValueAnimator spin = ValueAnimator.ofFloat(0f, 360f);
            spin.setDuration(4200);
            spin.setRepeatCount(ValueAnimator.INFINITE);
            spin.setInterpolator(new LinearInterpolator());
            spin.addUpdateListener(a -> {
                angle = (float) a.getAnimatedValue();
                invalidate();
            });
            keep(spin).start();
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            gradient = new SweepGradient(w / 2f, h / 2f,
                    new int[] {0x00FFDD00, 0xFFFFDD00, 0xFFFF8A00, 0x00FF8A00, 0x00FFDD00},
                    new float[] {0f, 0.18f, 0.32f, 0.5f, 1f});
            paint.setShader(gradient);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            if (gradient == null) return;
            matrix.setRotate(angle, getWidth() / 2f, getHeight() / 2f);
            gradient.setLocalMatrix(matrix);
            float h = stroke / 2f;
            rect.set(h, h, getWidth() - h, getHeight() - h);
            canvas.drawRoundRect(rect, radius, radius, paint);
        }
    }
}
