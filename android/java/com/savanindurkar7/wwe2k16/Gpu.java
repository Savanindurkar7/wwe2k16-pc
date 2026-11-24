package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;

import java.util.Locale;

/**
 * Which GPU this phone has (r17): read once from the phone's own OpenGL ES driver (GL_RENDERER, e.g. "Adreno (TM) 810"
 * or "Mali-G610 MC6") and kept until the system is updated. The launcher uses it for GPU-specific defaults (the Mali
 * profile) and to point at the right built-in driver; the game gets it as WWE2K16_GPU_FAMILY.
 */
public final class Gpu {
    public static final String ADRENO = "adreno", MALI = "mali", POWERVR = "powervr", XCLIPSE = "xclipse", OTHER = "other";

    private Gpu() {}

    private static String cachedRenderer;

    /** The renderer string, e.g. "Mali-G610 MC6"; "" when it could not be read. */
    public static String renderer(Context c) {
        if (cachedRenderer != null) return cachedRenderer;
        SharedPreferences p = Settings.prefs(c);
        String stamp = Build.FINGERPRINT;
        if (stamp.equals(p.getString("gpu_renderer_stamp", null))) {
            cachedRenderer = p.getString("gpu_renderer", "");
            return cachedRenderer;
        }
        String r = query();
        if (r == null) r = "";
        p.edit().putString("gpu_renderer", r).putString("gpu_renderer_stamp", stamp).apply();
        cachedRenderer = r;
        return r;
    }

    public static String family(Context c) {
        return familyOf(renderer(c));
    }

    static String familyOf(String renderer) {
        String r = renderer.toLowerCase(Locale.ROOT);
        if (r.contains("adreno")) return ADRENO;
        if (r.contains("mali") || r.contains("immortalis")) return MALI;
        if (r.contains("powervr")) return POWERVR;
        if (r.contains("xclipse") || r.contains("samsung")) return XCLIPSE;
        if (r.isEmpty()) {
            // No GL answer: guess from the chip maker (Qualcomm = Adreno; MediaTek, Samsung Exynos, Google Tensor = Mali).
            String soc = Build.VERSION.SDK_INT >= 31 ? (Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL) : Build.HARDWARE;
            soc = soc.toLowerCase(Locale.ROOT);
            if (soc.contains("qualcomm") || soc.contains("qcom") || soc.startsWith("sm") || soc.contains("qti")) return ADRENO;
            if (soc.contains("mediatek") || soc.startsWith("mt") || soc.contains("google") || soc.contains("tensor")) return MALI;
        }
        return OTHER;
    }

    public static boolean isMali(Context c) {
        return MALI.equals(family(c));
    }

    /** r19: Mali performance tiers for the high-settings lock and presets. 0 = not a Mali. */
    public static final int MALI_NONE = 0, MALI_LOW = 1, MALI_STANDARD = 2, MALI_HIGH = 3;

    /**
     * Tier of this phone's Mali GPU, from its name ("Mali-G52 MC2", "Mali-G610 MC6", "Immortalis-G715"):
     *   HIGH     Immortalis, Mali-G7xx / G8xx / G9xx, or G610 / G615 / G620 with 6+ cores
     *   STANDARD G68 / G76 / G77 / G78, smaller G610 / G615 builds (3-5 cores)
     *   LOW      G31 / G51 / G52 / G57 / G71 / G72, and 1-2 core builds of anything newer (a G615 MC2 overheated to
     *            25 FPS, a G52 ran 8 FPS, a G57 fell to 10 FPS from heat in the 2026-11 tests)
     */
    public static int maliTier(Context c) {
        if (!isMali(c)) return MALI_NONE;
        return maliTierOf(renderer(c));
    }

    static int maliTierOf(String r) {
        String l = r.toLowerCase(Locale.ROOT);
        int cores = 0;
        java.util.regex.Matcher mc = java.util.regex.Pattern.compile("mc(\\d+)").matcher(l);
        if (mc.find()) cores = Integer.parseInt(mc.group(1));
        int model = 0;
        java.util.regex.Matcher mm = java.util.regex.Pattern.compile("g(\\d{2,3})").matcher(l);
        if (mm.find()) model = Integer.parseInt(mm.group(1));
        if (l.contains("immortalis")) return MALI_HIGH;
        if (model >= 700) return cores > 0 && cores <= 2 ? MALI_STANDARD : MALI_HIGH;
        if (model >= 600) {  // G610 / G615 / G620
            if (cores >= 6) return MALI_HIGH;
            if (cores > 2) return MALI_STANDARD;
            return MALI_LOW;
        }
        if (model == 68 || model == 76 || model == 77 || model == 78) return cores > 0 && cores <= 2 ? MALI_LOW : MALI_STANDARD;
        return MALI_LOW;  // G31 / G51 / G52 / G57 / G71 / G72 and unknown
    }

    public static String maliTierName(int tier) {
        return tier == MALI_HIGH ? "high-end" : tier == MALI_STANDARD ? "mid-range" : "entry-level";
    }

    /** "Adreno 810"-style generation for picking a Turnip build: 6, 7, 8 or 0 when not an Adreno. */
    public static int adrenoGeneration(Context c) {
        String r = renderer(c);
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("Adreno[^0-9]*([0-9])([0-9]{2})").matcher(r);
        return m.find() ? Integer.parseInt(m.group(1)) : 0;
    }

    /** A 1x1 offscreen OpenGL ES context, only to read GL_RENDERER. */
    private static String query() {
        EGLDisplay display = EGL14.EGL_NO_DISPLAY;
        EGLContext ctx = EGL14.EGL_NO_CONTEXT;
        EGLSurface surface = EGL14.EGL_NO_SURFACE;
        try {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            if (display == EGL14.EGL_NO_DISPLAY) return null;
            int[] version = new int[2];
            if (!EGL14.eglInitialize(display, version, 0, version, 1)) return null;
            int[] attribs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                    EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, count, 0) || count[0] == 0) return null;
            ctx = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            if (ctx == EGL14.EGL_NO_CONTEXT) return null;
            surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                    new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            if (surface == EGL14.EGL_NO_SURFACE) return null;
            if (!EGL14.eglMakeCurrent(display, surface, surface, ctx)) return null;
            return GLES20.glGetString(GLES20.GL_RENDERER);
        } catch (Throwable t) {
            return null;
        } finally {
            if (display != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
                if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, ctx);
                EGL14.eglTerminate(display);
            }
        }
    }
}
