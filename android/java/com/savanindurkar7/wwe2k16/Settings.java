package com.savanindurkar7.wwe2k16;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Every launcher setting in one place: its choices, default, the note shown under it, and how it reaches the game
 * (command-line cvars for the SDK, WWE2K16_* environment variables for the game's own code).
 *
 * Some choices need work that is not in this build yet; their notes say so instead of pretending.
 */
public final class Settings {
    public static final String PREFS = "wwe2k16_launcher";

    public static final String KEY_GAME_PATH = "game_path";
    public static final String KEY_GAME_KIND = "game_kind";  // "folder" or "iso"

    /** A setting with fixed choices (shown as a row with arrows). */
    public static final class Choice {
        public final String key, section, title;
        public final String[] values, labels, notes;
        public final int defaultIndex;

        Choice(String section, String key, String title, int defaultIndex, String[] values, String[] labels, String[] notes) {
            this.section = section;
            this.key = key;
            this.title = title;
            this.defaultIndex = defaultIndex;
            this.values = values;
            this.labels = labels;
            this.notes = notes;
        }

        public int index(SharedPreferences p) {
            String v = p.getString(key, values[defaultIndex]);
            for (int i = 0; i < values.length; i++) if (values[i].equals(v)) return i;
            return defaultIndex;
        }

        public String value(SharedPreferences p) { return values[index(p)]; }
    }

    public static final String S_GRAPHICS = "GRAPHICS";
    public static final String S_DISPLAY = "DISPLAY";
    public static final String S_PERFORMANCE = "PERFORMANCE";
    public static final String S_DRIVER = "GPU DRIVER";
    public static final String S_ADVANCED = "ADVANCED";
    public static final String[] SECTIONS = {S_GRAPHICS, S_DISPLAY, S_PERFORMANCE, S_DRIVER, S_ADVANCED};

    private static String[] a(String... s) { return s; }

    public static final Choice RESOLUTION = new Choice(S_GRAPHICS, "resolution", "RESOLUTION", 3,
            a("360", "480", "540", "720", "1080", "1440"),
            a("360p", "480p", "540p", "720p", "1080p", "1440p"),
            a("Below 720p comes with the native renderer (next update). This build draws at 720p.",
              "Below 720p comes with the native renderer (next update). This build draws at 720p.",
              "Below 720p comes with the native renderer (next update). This build draws at 720p.",
              "The game's own resolution. Best balance for phones.",
              "Drawn at 1440p (2x) and scaled to your screen: sharp, but only for the strongest phones.",
              "Drawn at 1440p (2x). Only for the strongest phones."));

    public static final Choice FRAME_RATE = new Choice(S_GRAPHICS, "frame_rate", "FRAME RATE", 0,
            a("60", "30", "classic"),
            a("60 FPS", "30 FPS", "CLASSIC"),
            a("60 FPS everywhere, entrances included. With the game speed fix the game keeps normal speed below 60.",
              "Capped at 30 FPS: steadier and cooler on phones that cannot hold 60.",
              "Like the console: 60 in matches, 30 in entrances and cutscenes."));

    public static final Choice MSAA = new Choice(S_GRAPHICS, "msaa", "ANTI-ALIASING (MSAA)", 0,
            a("game", "off"),
            a("2X (GAME)", "OFF"),
            a("The game's own 2x MSAA.",
              "Turning the game's MSAA off needs a hook in the game's render setup, found in the next research build. "
              + "This build keeps 2x."));

    public static final Choice FAST_AA = new Choice(S_GRAPHICS, "fast_aa", "FAST ANTI-ALIASING", 0,
            a("none", "fxaa", "fxaa_extreme"),
            a("OFF", "FXAA", "FXAA EXTREME"),
            a("No extra smoothing. Fastest.",
              "FXAA smooths edges for a small cost.",
              "Stronger FXAA, a little more cost."));

    public static final Choice UPSCALER = new Choice(S_GRAPHICS, "upscaler", "UPSCALER", 0,
            a("bilinear", "sgsr", "fsr"),
            a("BILINEAR", "SGSR", "FSR"),
            a("Plain scaling to your screen. Works now.",
              "Snapdragon Game Super Resolution comes with the native renderer update. This build uses bilinear.",
              "FSR needs AMD's FidelityFX library, which is not in the Android build yet. This build uses bilinear."));

    public static final Choice TEXTURES = new Choice(S_GRAPHICS, "texture_quality", "TEXTURE QUALITY", 0,
            a("0", "1", "2"),
            a("HIGH", "MEDIUM", "LOW"),
            a("Full texture detail.",
              "Skips the largest texture level: less memory traffic.",
              "Skips the two largest levels: fastest, blurrier up close."));

    public static final Choice FILTER = new Choice(S_GRAPHICS, "aniso", "TEXTURE FILTER", 2,
            a("0", "2", "3", "4", "5"),
            a("OFF", "2X", "4X", "8X", "16X"),
            a("No anisotropic filtering: fastest.", "Light filtering.", "Balanced.", "Sharper floors and ring mat.",
              "Sharpest, the most texture work."));

    public static final Choice DISPLAY = new Choice(S_DISPLAY, "display", "DISPLAY", 0,
            a("fullscreen", "windowed"),
            a("FULL SCREEN", "WINDOWED"),
            a("The game covers the whole screen, system bars hidden.",
              "The status and navigation bars stay visible; the game fits between them."));

    public static final Choice SCREEN_SHAPE = new Choice(S_DISPLAY, "screen_shape", "SCREEN SHAPE", 0,
            a("16:9", "stretch", "safe"),
            a("16:9", "STRETCH", "SAFE AREA"),
            a("The game's 16:9 picture with black bars on wide phones.",
              "Fills the whole screen; the picture is stretched on wide phones.",
              "16:9, kept clear of rounded corners and the camera cut-out."));

    public static final Choice VSYNC = new Choice(S_DISPLAY, "vsync", "VSYNC", 0,
            a("1", "0"),
            a("ON", "OFF"),
            a("No tearing. Recommended.", "Lowest latency; the picture can tear."));

    public static final Choice SHARPNESS = new Choice(S_DISPLAY, "sharpness", "SHARPNESS", 2,
            a("0", "25", "50", "75", "100"),
            a("0%", "25%", "50%", "75%", "100%"),
            a("Used by the SGSR / FSR upscalers (not active in this build).",
              "Used by the SGSR / FSR upscalers (not active in this build).",
              "Used by the SGSR / FSR upscalers (not active in this build).",
              "Used by the SGSR / FSR upscalers (not active in this build).",
              "Used by the SGSR / FSR upscalers (not active in this build)."));

    public static final Choice CPU_CORES = new Choice(S_PERFORMANCE, "cpu_cores", "CPU CORES", 1,
            a("all", "auto", "big"),
            a("ALL", "AUTO", "BIG CORES"),
            a("Android places the game's threads anywhere.",
              "The game stays off the slow little cores.",
              "Only the fastest cores: highest speed, more heat."));

    public static final Choice SPEED_FIX = new Choice(S_PERFORMANCE, "speed_fix", "GAME SPEED FIX", 0,
            a("1", "0"),
            a("ON", "OFF"),
            a("The game runs at normal speed even below 60 FPS (no slow motion).",
              "Console behaviour: below 60 FPS the game runs in slow motion."));

    public static final Choice SHADER_CACHE = new Choice(S_PERFORMANCE, "shader_cache", "SHADER CACHE", 0,
            a("1", "0"),
            a("ON", "OFF"),
            a("Shaders are built in the background and kept on the phone: far less stutter from the second match on.",
              "Shaders are built again every time (only for testing)."));

    public static final Choice FRAME_MONITOR = new Choice(S_PERFORMANCE, "frame_monitor", "FRAME MONITOR", 1,
            a("0", "1", "2"),
            a("OFF", "FPS", "FULL"),
            a("No overlay.", "Frames per second in the corner.",
              "FPS, frame time, worst frame, and the shaders / textures / disk reads behind a stutter."));

    public static final Choice PERF_HINT = new Choice(S_PERFORMANCE, "perf_hint", "PERFORMANCE HINT", 0,
            a("1", "0"),
            a("ON", "OFF"),
            a("Tells Android each frame's deadline so the CPU speeds up in time (Android 12 and newer).",
              "Android decides CPU speeds on its own."));

    public static final Choice SUSTAINED = new Choice(S_PERFORMANCE, "sustained", "SUSTAINED PERFORMANCE", 1,
            a("1", "0"),
            a("ON", "OFF"),
            a("Asks Android for steady speed instead of short bursts (phones that support it).",
              "Normal Android power behaviour."));

    public static final Choice VK_DEBUG = new Choice(S_ADVANCED, "vk_debug", "VULKAN DEBUG MESSAGES", 1,
            a("1", "0"),
            a("ON", "OFF"),
            a("Logs every driver message: slow, only for finding driver problems.",
              "Off: the Adreno driver alone sends about a million messages per match."));

    public static final Choice[] CHOICES = {
            RESOLUTION, FRAME_RATE, MSAA, FAST_AA, UPSCALER, TEXTURES, FILTER,
            DISPLAY, SCREEN_SHAPE, VSYNC, SHARPNESS,
            CPU_CORES, SPEED_FIX, SHADER_CACHE, FRAME_MONITOR, PERF_HINT, SUSTAINED,
            VK_DEBUG,
    };

    public static final String KEY_EXTRA_ARGS = "extra_args";
    public static final String KEY_DRIVER = "gpu_driver";  // "" = the phone's driver, else the driver folder's name

    private Settings() {}

    public static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static List<Choice> inSection(String section) {
        List<Choice> out = new ArrayList<>();
        for (Choice c : CHOICES) if (c.section.equals(section)) out.add(c);
        return out;
    }

    public static void resetAll(Context ctx) {
        SharedPreferences.Editor e = prefs(ctx).edit();
        for (Choice c : CHOICES) e.remove(c.key);
        e.remove(KEY_EXTRA_ARGS);
        e.remove(KEY_DRIVER);
        e.apply();
    }

    // --- Folders ---------------------------------------------------------------------------------------------------

    public static File logsDir(Context c) {
        File d = new File(c.getFilesDir(), "logs");
        d.mkdirs();
        return d;
    }

    public static File userDataDir(Context c) {
        File d = new File(c.getFilesDir(), "userdata");
        d.mkdirs();
        return d;
    }

    // --- Launch ----------------------------------------------------------------------------------------------------

    /** What the game process gets: command-line arguments (cvars, then the game path) and environment variables. */
    public static final class Launch {
        public final ArrayList<String> args = new ArrayList<>();
        public final LinkedHashMap<String, String> env = new LinkedHashMap<>();
        public final StringBuilder summary = new StringBuilder();
    }

    public static Launch buildLaunch(Context ctx) {
        SharedPreferences p = prefs(ctx);
        Launch l = new Launch();
        // Resolution: the SDK draws at whole multiples of the game's 720p.
        String res = RESOLUTION.value(p);
        int scale = ("1080".equals(res) || "1440".equals(res)) ? 2 : 1;
        cvar(l, "draw_resolution_scale_x", String.valueOf(scale));
        cvar(l, "draw_resolution_scale_y", String.valueOf(scale));

        String fr = FRAME_RATE.value(p);
        l.env.put("WWE2K16_KEEP_60", "classic".equals(fr) ? "0" : "1");
        l.env.put("WWE2K16_FRAME_CAP", "30".equals(fr) ? "30" : "0");

        cvar(l, "swap_post_effect", FAST_AA.value(p));
        cvar(l, "texture_min_lod", TEXTURES.value(p));
        cvar(l, "anisotropic_override", FILTER.value(p));
        // MSAA "off" and the SGSR / FSR upscalers are not available in this build (see their notes): nothing to pass.

        String shape = SCREEN_SHAPE.value(p);
        cvar(l, "present_letterbox", "stretch".equals(shape) ? "false" : "true");
        if ("safe".equals(shape)) {
            cvar(l, "present_safe_area_x", "94");
            cvar(l, "present_safe_area_y", "94");
        }
        l.env.put("WWE2K16_VSYNC", VSYNC.value(p));

        l.env.put("WWE2K16_CPU_CORES", CPU_CORES.value(p));
        l.env.put("WWE2K16_SPEED_FIX", SPEED_FIX.value(p));
        boolean cache = "1".equals(SHADER_CACHE.value(p));
        cvar(l, "vulkan_pipeline_cache", cache ? "true" : "false");
        cvar(l, "async_shader_compilation", "true");
        l.env.put("WWE2K16_FRAME_MONITOR", FRAME_MONITOR.value(p));
        l.env.put("WWE2K16_PERF_HINT", PERF_HINT.value(p));
        cvar(l, "vulkan_log_debug_messages", "1".equals(VK_DEBUG.value(p)) ? "true" : "false");

        File logs = logsDir(ctx);
        l.env.put("WWE2K16_LOG_DIR", logs.getAbsolutePath());
        l.env.put("WWE2K16_LOG_FILE", new File(logs, "wwe2k16.log").getAbsolutePath());
        cvar(l, "user_data_root", userDataDir(ctx).getAbsolutePath());

        // Custom driver.
        String driver = p.getString(KEY_DRIVER, "");
        if (!driver.isEmpty()) {
            Drivers.Driver d = Drivers.find(ctx, driver);
            if (d != null) {
                l.env.put("WWE2K16_DRIVER", d.library);
                l.env.put("WWE2K16_DRIVER_DIR", d.dir.getAbsolutePath() + "/");
                l.env.put("WWE2K16_DRIVER_TRIAL", Drivers.trialFile(ctx).getAbsolutePath());
                // Turnip on newer Adrenos is steadier without its tile-based fast path.
                l.env.put("TU_DEBUG", "sysmem");
            }
        }

        // Extra arguments: "--cvar=value" go to the SDK, "NAME=value" become environment variables. They come last, so
        // they win over everything above.
        String extra = p.getString(KEY_EXTRA_ARGS, "").trim();
        if (!extra.isEmpty()) {
            for (String word : extra.split("\\s+")) {
                if (word.startsWith("--")) {
                    l.args.add(word);
                } else {
                    int eq = word.indexOf('=');
                    if (eq > 0) l.env.put(word.substring(0, eq), word.substring(eq + 1));
                }
            }
        }

        String game = p.getString(KEY_GAME_PATH, "");
        l.args.add(game);

        for (Choice c : CHOICES) l.summary.append(c.title).append(": ").append(c.labels[c.index(p)]).append('\n');
        l.summary.append("GPU DRIVER: ").append(driver.isEmpty() ? "System" : driver).append('\n');
        l.summary.append("EXTRA ARGUMENTS: ").append(extra).append('\n');
        l.summary.append("GAME: ").append(game).append('\n');
        return l;
    }

    private static void cvar(Launch l, String name, String value) {
        l.args.add("--" + name + "=" + value);
    }

    public static Map<String, String> describe(Context ctx) {
        SharedPreferences p = prefs(ctx);
        Map<String, String> m = new LinkedHashMap<>();
        for (Choice c : CHOICES) m.put(c.title, c.labels[c.index(p)]);
        return m;
    }
}
