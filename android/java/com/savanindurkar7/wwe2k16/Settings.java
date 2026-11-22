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
    public static final String S_CONTROLS = "CONTROLS";
    public static final String S_DRIVER = "GPU DRIVER";
    public static final String S_ADVANCED = "ADVANCED";
    public static final String S_CREDITS = "CREDITS";
    public static final String[] SECTIONS = {S_GRAPHICS, S_DISPLAY, S_PERFORMANCE, S_CONTROLS, S_DRIVER, S_ADVANCED, S_CREDITS};

    private static String[] a(String... s) { return s; }

    public static final Choice RENDERER = new Choice(S_GRAPHICS, "renderer", "RENDERER", 2,
            a("classic", "native", "native_plus"),
            a("CLASSIC", "NATIVE", "NATIVE+"),
            a("The emulated Xbox 360 GPU. The only renderer of the classic APK.",
              "Native APK: the SDK's native renderer draws the scene (from the WWE 2K14 port). Lower resolutions and "
              + "SGSR need it. The classic APK ignores this.",
              "Native APK: Native plus the faster resolve and texture paths. Fastest (about 40 FPS in matches at 480p "
              + "on an Adreno 810). Recommended."));

    public static final Choice FRAME_GEN = new Choice(S_GRAPHICS, "frame_gen", "FRAME GENERATION", 0,
            a("off", "on"),
            a("OFF", "ON (30 -> 60)"),
            a("Every frame the game draws is shown once.",
              "Native APK: the game runs at a steady 30 FPS and an in-between frame is made between every two, so the "
              + "screen shows 60 (the screen is set to 60 Hz). The GPU has room for the extra frames at 30, which it "
              + "did not at 40 (tests 2026-11-21: uncapped frame generation halved the game's own frames). Controls "
              + "respond at 30; fast moves can show small smears. Overrides FRAME RATE."));

    // (Key "crowd_v2": 20% is the new default for everyone; phone test 2026-11-22: crowd OFF about 44 FPS, 50% about 40.)
    public static final Choice CROWD = new Choice(S_GRAPHICS, "crowd_v2", "CROWD", 3,
            a("100", "70", "50", "20", "0"),
            a("FULL", "70%", "50%", "20%", "OFF"),
            a("The whole crowd. Native APK only (the classic APK always draws it).",
              "Native APK: 70% of the crowd, spread evenly; the same people stay from frame to frame.",
              "Native APK: half the crowd (about 40 FPS in a match on an Adreno 810).",
              "Native APK: a fifth of the crowd, spread evenly over the arena, so the seats do not look empty. Close to "
              + "crowd OFF for speed. Recommended.",
              "Native APK: no crowd at all (about 44 FPS, up to 60 in close shots)."));

    public static final Choice ENTRANCE_CROWD = new Choice(S_GRAPHICS, "entrance_crowd", "ENTRANCE CROWD", 1,
            a("same", "10", "0"),
            a("SAME AS MATCH", "10%", "OFF"),
            a("Native APK: entrances keep the CROWD setting.",
              "Native APK: during entrances and victory scenes 10% of the crowd (or CROWD if that is lower), back to "
              + "CROWD when the match starts. Entrances are the slowest part (about 40 FPS with the whole arena in view). "
              + "Recommended.",
              "Native APK: no crowd during entrances (fastest), the match keeps CROWD."));

    public static final Choice SHADOWS = new Choice(S_GRAPHICS, "shadows", "DYNAMIC SHADOWS", 0,
            a("on", "half", "off"),
            a("ON", "HALF RATE", "OFF"),
            a("Wrestler and prop shadows every frame.",
              "Native APK: the wrestlers' shadow maps drawn every other frame (the frame between reuses the last).",
              "Native APK: no moving shadows (baked arena lighting stays). Saves GPU time in every scene."));

    public static final Choice DOF = new Choice(S_GRAPHICS, "dof", "DEPTH OF FIELD", 0,
            a("on", "off"),
            a("ON", "OFF"),
            a("The game's blur of out-of-focus areas (entrance close-ups, replays).",
              "Native APK: no depth-of-field blur. Warning: on 2K16 this made the native renderer fall back in "
              + "matches (about 21 FPS instead of 40) in a test; keep ON unless you are testing."));

    // (Key "pass_trim_v2", OFF by default: phone test 2026-11-22 with crowd 20%: OFF about 53 FPS in a match, ON about
    // 42. The 2K14 port's frame-structure savings cost more than they save on 2K16 / the Adreno 810.)
    public static final Choice PASS_TRIM = new Choice(S_GRAPHICS, "pass_trim_v2", "RENDER PASS TRIM", 1,
            a("on", "off"),
            a("ON (TEST)", "OFF"),
            a("Native APK: the WWE 2K14 port's frame-structure savings. Slower on 2K16 in the phone test (about 42 FPS "
              + "instead of 53); only for testing.",
              "Native APK: the renderer without those savings. Faster on 2K16. Recommended."));

    public static final Choice RESOLUTION = new Choice(S_GRAPHICS, "resolution", "RESOLUTION", 1,
            a("360", "480", "540", "720", "1080", "1440"),
            a("360p", "480p", "540p", "720p", "1080p", "1440p"),
            a("Native APK with the NATIVE renderer: the scene drawn at 360p and scaled up (SGSR recommended). Else 720p.",
              "Native APK with the NATIVE renderer: the scene drawn at 480p and scaled up (SGSR recommended). Else 720p.",
              "Native APK with the NATIVE renderer: the scene drawn at 540p and scaled up (SGSR recommended). Else 720p.",
              "The game's own resolution. Best balance for phones.",
              "Drawn at 1440p (2x) and scaled to your screen: sharp, but only for the strongest phones.",
              "Drawn at 1440p (2x). Only for the strongest phones."));

    public static final Choice FRAME_RATE = new Choice(S_GRAPHICS, "frame_rate", "FRAME RATE", 0,
            a("60", "45", "30", "classic"),
            a("60 FPS", "45 FPS (STEADY)", "30 FPS", "CLASSIC"),
            a("Up to 60 FPS everywhere, entrances included. With the game speed fix the game keeps normal speed below 60.",
              "Capped at 45 FPS: much steadier than jumping between 35 and 60 (the screen is set to 90 Hz when it has "
              + "it, so every frame shows for the same time). The game keeps normal speed.",
              "Capped at 30 FPS: steadiest and coolest.",
              "Like the console: 60 in matches, 30 in entrances and cutscenes."));

    // (Key "msaa_v2": the old "msaa" setting could only be 2X; the new default is OFF for everyone.)
    public static final Choice MSAA = new Choice(S_GRAPHICS, "msaa_v2", "ANTI-ALIASING (MSAA)", 0,
            a("off", "game"),
            a("OFF (FAST)", "2X (GAME)"),
            a("Native APK: the game builds its 3D scene without MSAA, so the NATIVE renderer draws entrances and matches "
              + "itself: fixes the 5-9 FPS entrances and the black flicker frames in matches. Edges a little sharper "
              + "(SGSR or FXAA smooth them). The classic APK keeps 2x.",
              "The game's own 2x MSAA. With the NATIVE renderer entrances fall back to the slow path (5-9 FPS)."));

    public static final Choice FAST_AA = new Choice(S_GRAPHICS, "fast_aa", "FAST ANTI-ALIASING", 0,
            a("none", "fxaa", "fxaa_extreme"),
            a("OFF", "FXAA", "FXAA EXTREME"),
            a("No extra smoothing. Fastest.",
              "FXAA smooths edges for a small cost.",
              "Stronger FXAA, a little more cost."));

    public static final Choice UPSCALER = new Choice(S_GRAPHICS, "upscaler", "UPSCALER", 1,
            a("bilinear", "sgsr", "fsr"),
            a("BILINEAR", "SGSR", "FSR"),
            a("Plain scaling to your screen. Works in both APKs.",
              "Native APK: Snapdragon Game Super Resolution, sharp upscaling made for Adreno. The classic APK uses bilinear.",
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
            a("SGSR edge sharpness (native APK). Softest.", "SGSR edge sharpness (native APK).", "SGSR edge sharpness (native APK). Default.",
              "SGSR edge sharpness (native APK).", "SGSR edge sharpness (native APK). Sharpest."));

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

    public static final Choice TOUCH = new Choice(S_CONTROLS, "touch", "TOUCH CONTROLS", 0,
            a("auto", "on", "off"),
            a("AUTO", "ON", "OFF"),
            a("Shown on screen, hidden while a gamepad is connected.",
              "Always shown, even with a gamepad (both work at once).",
              "Never shown: play with a gamepad."));

    public static final Choice TOUCH_VIBRATE = new Choice(S_CONTROLS, "touch_vibrate", "BUTTON VIBRATION", 0,
            a("1", "0"),
            a("ON", "OFF"),
            a("A short buzz when you press an on-screen control.", "No buzz on presses."));

    public static final Choice RUMBLE = new Choice(S_CONTROLS, "rumble", "GAME RUMBLE", 0,
            a("1", "0"),
            a("ON", "OFF"),
            a("The phone vibrates when the game rumbles the controller (hits, slams, finishers).",
              "No rumble on the phone."));

    public static final Choice READBACK = new Choice(S_ADVANCED, "readback", "CHARACTER TEXTURE FIX", 0,
            a("auto", "always"),
            a("AUTO", "ALWAYS EXACT"),
            a("Native APK: exact GPU readback while wrestler textures are built (loads), off in play.",
              "Native APK: exact readback all the time. Use it if a wrestler or attire shows noise or black / magenta "
              + "patches; it costs speed."));

    // (Key "native_shortcuts_v2": ON is the new default for everyone. The spikes blamed on these shortcuts were
    // Turnip's (fixed by the Turnip skin fix); phone test T3, 2026-11-22: ALL ran 38-47 FPS with no glitches.)
    public static final Choice SHORTCUTS = new Choice(S_ADVANCED, "native_shortcuts_v2", "NATIVE SHORTCUTS", 0,
            a("all", "safe"),
            a("ON (FAST)", "OFF"),
            a("Native APK: geometry page trust and vertex endian baking on. Faster and smoother; no glitches with the "
              + "Turnip skin fix ON.",
              "Native APK: those shortcuts off (slower). Only for testing."));

    public static final Choice TURNIP_FIX = new Choice(S_ADVANCED, "turnip_skin_fix", "TURNIP SKIN FIX", 0,
            a("on", "off"),
            a("ON", "OFF"),
            a("With a Turnip driver: its shader compiler keeps the game's constants in memory instead of copying them into "
              + "fast registers (IR3_SHADER_DEBUG=nouboopt). Fixes the wrestlers' skin stretching into spikes in close-ups "
              + "(phone test A, 2026-11-21). No effect with the phone's own driver.",
              "Turnip's normal compiler: a little faster, but the skin spike glitch comes back. Only for testing."));

    // Phone tests 2026-11-22 (r13): CPU fast path ON 26-35 FPS in a match (a game thread at ~89%: worse), native frame
    // ON 42-57 (about 51, the best so far). New keys so the new defaults reach everyone.
    public static final Choice CP_FAST = new Choice(S_ADVANCED, "cp_fast_v2", "CPU FAST PATH", 1,
            a("on", "off"),
            a("ON (TEST)", "OFF"),
            a("Native APK, test: the WWE 2K14 port's command-processor speed-ups. Slower on 2K16 in the phone test "
              + "(about 30 FPS instead of 45); only for testing.",
              "Native APK: without those speed-ups. Recommended."));

    public static final Choice NATIVE_FRAME = new Choice(S_ADVANCED, "native_frame_v2", "NATIVE FRAME", 0,
            a("on", "off"),
            a("ON (FAST)", "OFF"),
            a("Native APK: the GPU command thread answers the game at once and draws behind it, so the game does not "
              + "wait for the drawing (about 51 FPS in a match instead of 43 in the phone test). Recommended.",
              "Native APK: answers and drawing together (slower)."));

    public static final Choice PERF_LOG = new Choice(S_ADVANCED, "perf_log", "PERFORMANCE LOG", 1,
            a("on", "off"),
            a("ON", "OFF"),
            a("For finding what limits the frame rate: every few seconds the log gets each thread's CPU use and the "
              + "GPU time of the scene. Costs 1-2 FPS. Send the logs after a match.",
              "No extra measuring."));

    public static final Choice VK_DEBUG = new Choice(S_ADVANCED, "vk_debug", "VULKAN DEBUG MESSAGES", 1,
            a("1", "0"),
            a("ON", "OFF"),
            a("Logs every driver message: slow, only for finding driver problems.",
              "Off: the Adreno driver alone sends about a million messages per match."));

    public static final Choice[] CHOICES = {
            RENDERER, RESOLUTION, FRAME_GEN, CROWD, ENTRANCE_CROWD, SHADOWS, DOF, PASS_TRIM, FRAME_RATE, MSAA, FAST_AA, UPSCALER, TEXTURES, FILTER,
            DISPLAY, SCREEN_SHAPE, VSYNC, SHARPNESS,
            CPU_CORES, SPEED_FIX, SHADER_CACHE, FRAME_MONITOR, PERF_HINT, SUSTAINED,
            TOUCH, TOUCH_VIBRATE, RUMBLE,
            READBACK, SHORTCUTS, CP_FAST, NATIVE_FRAME, TURNIP_FIX, PERF_LOG, VK_DEBUG,
    };

    public static final String KEY_EXTRA_ARGS = "extra_args";
    public static final String KEY_DRIVER = "gpu_driver";  // "" = the phone's driver, else the driver folder's name

    private Settings() {}

    private static Boolean nativeBuild;

    /** True in the native renderer APK (tools/android-build-k14.sh puts assets/sdk.txt = "k14" into it). */
    public static boolean isNativeBuild(Context c) {
        if (nativeBuild == null) {
            boolean k14 = false;
            try (java.io.InputStream in = c.getAssets().open("sdk.txt")) {
                byte[] b = new byte[16];
                int n = in.read(b);
                k14 = n > 0 && new String(b, 0, n).trim().equals("k14");
            } catch (java.io.IOException ignored) {
            }
            nativeBuild = k14;
        }
        return nativeBuild;
    }

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

    /** The SDK's VkPipelineCache files (TITLE.vkpc) and Mesa's own shader cache (under HOME = files/.cache). */
    static void clearShaderCaches(Context c) {
        File shareable = new File(userDataDir(c), "cache/shaders/shareable");
        File[] files = shareable.listFiles();
        if (files != null) for (File f : files) if (f.getName().endsWith(".vkpc")) f.delete();
        File[] caches = new File(c.getFilesDir(), ".cache").listFiles();
        if (caches != null) for (File f : caches) if (f.getName().startsWith("mesa_shader_cache")) deleteTree(f);
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File ch : children) deleteTree(ch);
        f.delete();
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
        final boolean nat = isNativeBuild(ctx);
        final String renderer = RENDERER.value(p);
        final boolean nativeRenderer = nat && !"classic".equals(renderer);
        if (nat) {
            l.env.put("WWE2K16_RENDERER", renderer);
            l.env.put("WWE2K16_NATIVE_SHORTCUTS", "all".equals(SHORTCUTS.value(p)) ? "1" : "0");
            l.env.put("WWE2K16_PASS_TRIM", "on".equals(PASS_TRIM.value(p)) ? "1" : "0");
            l.env.put("WWE2K16_CP_FAST", "on".equals(CP_FAST.value(p)) ? "1" : "0");
            l.env.put("WWE2K16_NATIVE_FRAME", "on".equals(NATIVE_FRAME.value(p)) ? "1" : "0");
            if ("on".equals(PERF_LOG.value(p))) {
                l.env.put("WWE2K16_THREAD_PROFILE", "1");
                cvar(l, "gpu_native_gpu_timers", "true");
            }
            // SDK scene levers from the WWE 2K14 port (keyed on shader hashes that 2K16 shares: its crowd draws are the
            // same three crowd shaders, the census shows).
            String crowd = CROWD.value(p);
            if (!"100".equals(crowd)) cvar(l, "gpu_scene_crowd_density", crowd);
            String entranceCrowd = ENTRANCE_CROWD.value(p);
            if (!"same".equals(entranceCrowd)) l.env.put("WWE2K16_ENTRANCE_CROWD", entranceCrowd);
            String shadows = SHADOWS.value(p);
            if ("off".equals(shadows)) cvar(l, "gpu_dynamic_shadows", "false");
            if ("half".equals(shadows)) cvar(l, "gpu_shadow_half_rate", "true");
            if ("off".equals(DOF.value(p))) cvar(l, "gpu_depth_of_field", "false");
            boolean frameGen = "on".equals(FRAME_GEN.value(p));
            if (frameGen) cvar(l, "present_frame_generation", "true");
            l.env.put("WWE2K16_FRAMEGEN", frameGen ? "1" : "0");
        }
        String res = RESOLUTION.value(p);
        int scale = ("1080".equals(res) || "1440".equals(res)) ? 2 : 1;
        cvar(l, "draw_resolution_scale_x", String.valueOf(scale));
        cvar(l, "draw_resolution_scale_y", String.valueOf(scale));
        if (nat) cvar(l, "resolution_scale", String.valueOf(scale));
        // Below 720p: the native renderer draws the scene smaller and scales it up (gpu_native_truth_draw_height).
        int height = Integer.parseInt(res);
        if (nativeRenderer && height < 720) cvar(l, "gpu_native_truth_draw_height", res);
        if (nat) {
            boolean sgsr = "sgsr".equals(UPSCALER.value(p));
            cvar(l, "present_effect", sgsr ? "sgsr" : "bilinear");
            // 0..100% -> SGSR's edge sharpness 1..2.
            double sharp = 1.0 + Integer.parseInt(SHARPNESS.value(p)) / 100.0;
            if (sgsr) cvar(l, "present_sgsr_edge_sharpness", String.valueOf(sharp));
        }

        String fr = FRAME_RATE.value(p);
        boolean fg = nat && "on".equals(FRAME_GEN.value(p));
        if (fg) fr = "30";  // frame generation v3: a steady 30 from the game, 60 on the screen
        l.env.put("WWE2K16_KEEP_60", "classic".equals(fr) ? "0" : "1");
        l.env.put("WWE2K16_FRAME_CAP", "30".equals(fr) ? "30" : "45".equals(fr) ? "45" : "0");
        // The screen's refresh rate (GameActivity): 60 Hz for frame generation (30 + 30 evenly), 90 Hz for 45 FPS.
        l.env.put("WWE2K16_DISPLAY_HZ", fg ? "60" : "45".equals(fr) ? "90" : "0");

        cvar(l, "swap_post_effect", FAST_AA.value(p));
        cvar(l, "texture_min_lod", TEXTURES.value(p));
        cvar(l, "anisotropic_override", FILTER.value(p));
        // MSAA OFF: the game's scene targets built 1x (src/k14_tiling_hooks.h, native APK only).
        if (nat && "off".equals(MSAA.value(p))) cvar(l, "wwe2k16_force_no_msaa", "true");

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
        cvar(l, nat ? "vulkan_disk_pipeline_cache" : "vulkan_pipeline_cache", cache ? "true" : "false");
        cvar(l, "async_shader_compilation", "true");
        l.env.put("WWE2K16_FRAME_MONITOR", FRAME_MONITOR.value(p));
        l.env.put("WWE2K16_PERF_HINT", PERF_HINT.value(p));
        l.env.put("WWE2K16_TOUCH", TOUCH.value(p));
        l.env.put("WWE2K16_TOUCH_VIBRATE", TOUCH_VIBRATE.value(p));
        l.env.put("WWE2K16_RUMBLE", RUMBLE.value(p));
        cvar(l, "vulkan_log_debug_messages", "1".equals(VK_DEBUG.value(p)) ? "true" : "false");

        File logs = logsDir(ctx);
        l.env.put("WWE2K16_LOG_DIR", logs.getAbsolutePath());
        l.env.put("WWE2K16_LOG_FILE", new File(logs, "wwe2k16.log").getAbsolutePath());
        cvar(l, "user_data_root", userDataDir(ctx).getAbsolutePath());
        if (nat) {
            // The 0.10 SDK names its log with --log_file (0.7 reads WWE2K16_LOG_FILE).
            cvar(l, "log_file", new File(logs, "wwe2k16.log").getAbsolutePath());
            l.env.put("WWE2K16_READBACK_ALWAYS", "always".equals(READBACK.value(p)) ? "1" : "0");
        }

        // Custom driver.
        String compiler = "";
        String driver = p.getString(KEY_DRIVER, "");
        if (!driver.isEmpty()) {
            Drivers.Driver d = Drivers.find(ctx, driver);
            if (d != null) {
                l.env.put("WWE2K16_DRIVER", d.library);
                l.env.put("WWE2K16_DRIVER_DIR", d.dir.getAbsolutePath() + "/");
                l.env.put("WWE2K16_DRIVER_TRIAL", Drivers.trialFile(ctx).getAbsolutePath());
                // Turnip on newer Adrenos is steadier without its tile-based fast path.
                l.env.put("TU_DEBUG", "sysmem");
                // Turnip skin fix: the bone matrices are read with an index known only while the shader runs; Turnip's
                // copy of uniform buffers into the constant registers ("UBO optimisation") gets those wrong on the
                // A8xx (spikes in close-ups). Without it: clean (test A, 2026-11-21).
                if (d.library.contains("freedreno") && "on".equals(TURNIP_FIX.value(p))) {
                    l.env.put("IR3_SHADER_DEBUG", "nouboopt");
                    compiler = "nouboopt";
                }
            }
        }
        // Shaders compiled with other compiler switches must not come back from the caches: when the switches change,
        // the pipeline cache and Mesa's shader cache start over once.
        if (!compiler.equals(p.getString("compiler_switches_applied", ""))) {
            clearShaderCaches(ctx);
            p.edit().putString("compiler_switches_applied", compiler).apply();
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
        // 0.7 takes the game folder / .iso as the first plain argument; 0.10 as --game_data_root.
        if (nat) cvar(l, "game_data_root", game);
        else l.args.add(game);

        for (Choice c : CHOICES) l.summary.append(c.title).append(": ").append(c.labels[c.index(p)]).append('\n');
        l.summary.append("BUILD: ").append(nat ? "native (WWE 2K14 SDK)" : "classic (ReXGlue 0.7)").append('\n');
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
