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
            a("Every frame the game draws is shown once. (r21: frame generation is switched off for everyone.)",
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

    // r17: AUTO adds the attire latch on Mali and other non-Adreno GPUs: the game builds a wrestler's attire on the GPU
    // (a 2048-wide copy) and compresses it on the CPU; on a phone whose frames run ahead of its loader the read-back
    // window closed before that copy and the wrestler came out pink / magenta hatched (Infinix X6871, Mali-G610,
    // 2026-11-22). The latch reads back every 2048-wide one-off copy, and the 1024-wide ones for 300 frames after it
    // (WWE 2K14 port, Session 95, "the purple Savage"). Adreno keeps the old AUTO.
    public static final Choice READBACK = new Choice(S_ADVANCED, "readback", "CHARACTER TEXTURE FIX", 0,
            a("auto", "latch", "always"),
            a("AUTO", "ATTIRE LATCH", "ALWAYS EXACT"),
            a("Native APK: exact GPU readback while wrestler textures are built (loads), off in play. On Mali and other "
              + "non-Adreno GPUs the attire latch is added (fixes the pink / magenta hatched wrestler).",
              "Native APK: the attire latch on any GPU: every attire the game builds on the GPU is read back when it is "
              + "built, wherever it lands. Almost free. Use it if one wrestler shows pink, magenta or black patches.",
              "Native APK: exact readback all the time. The strongest fix for wrestler / attire noise; it costs speed."));

    // r17: Mali profile (only on phones whose GPU is a Mali; nothing changes on Adreno). The WWE 2K14 port's phone
    // switches that are off by default because they were not measured on every GPU: geometry shaders avoided (on Mali
    // they are the second biggest GPU cost: the primitive types are expanded in the vertex shader instead), classic
    // render passes for the tile-based GPU, and vertices read as real attributes.
    public static final Choice MALI_BOOST = new Choice(S_ADVANCED, "mali_boost", "MALI BOOST", 0,
            a("all", "nogs", "off"),
            a("ON (MALI ONLY)", "NO GEOMETRY SHADERS ONLY", "OFF"),
            a("Mali phones: geometry shaders avoided, classic render passes for the tile-based GPU and vertices read as "
              + "real attributes (the WWE 2K14 port's phone switches). Faster on Mali; no effect on Adreno or others.",
              "Mali phones: only the geometry shaders avoided (if ON shows a glitch, this is the safest half).",
              "Mali phones: the renderer as before r17."));

    public static final Choice MALI_TEXTURES = new Choice(S_ADVANCED, "mali_textures", "MALI TEXTURES", 0,
            a("normal", "small"),
            a("NORMAL", "SMALL (ASTC / EAC)"),
            a("GPUs without the game's compressed formats (most Mali) get them unpacked to full size (4-8x the memory).",
              "Those textures are converted to ASTC / EAC instead, which Mali reads directly: less memory and bandwidth "
              + "(about +2 FPS in the Mali test), a little slower to load and slightly lossy. Only on GPUs that lack the "
              + "formats: no effect on Adreno."));

    // (Key "native_shortcuts_v3", OFF for everyone again, r21: with the phone's own drivers (Adreno 650 / 740, Mali)
    // the 1.2.1 testers got huge flat brown / blue triangles and stretched ropes near the camera in matches and replays
    // (2026-11-25 screenshots). The T3 test that found no glitches was on Turnip only.)
    public static final Choice SHORTCUTS = new Choice(S_ADVANCED, "native_shortcuts_v3", "NATIVE SHORTCUTS", 1,
            a("all", "safe"),
            a("ON (TEST)", "OFF (SAFE)"),
            a("Native APK, test: geometry page trust and vertex endian baking on. A little faster, but with some "
              + "drivers it draws huge flat triangles and stretched ropes near the camera.",
              "Native APK: those shortcuts off. No geometry glitches. Recommended."));

    public static final Choice TURNIP_FIX = new Choice(S_ADVANCED, "turnip_skin_fix", "TURNIP SKIN FIX", 0,
            a("on", "off"),
            a("ON", "OFF"),
            a("With a Turnip driver: its shader compiler keeps the game's constants in memory instead of copying them into "
              + "fast registers (IR3_SHADER_DEBUG=nouboopt). Fixes the wrestlers' skin stretching into spikes in close-ups "
              + "(phone test A, 2026-11-21). No effect with the phone's own driver.",
              "Turnip's normal compiler: a little faster, but the skin spike glitch comes back. Only for testing."));

    // Phone tests 2026-11-22 (r13): CPU fast path ON 26-35 FPS in a match (a game thread at ~89%: worse), native frame
    // ON 42-57 (about 51, the best so far). New keys so the new defaults reach everyone.
    // (Key "cp_fast_v3": SAFE is the new default. Phone test r13: ALL 26-35 FPS, a game thread spinning at ~89%.)
    public static final Choice CP_FAST = new Choice(S_ADVANCED, "cp_fast_v3", "CPU FAST PATH", 0,
            a("safe", "all", "off"),
            a("SAFE (FAST)", "ALL (TEST)", "OFF"),
            a("Native APK: the GPU command thread's own speed-ups from the WWE 2K14 port (register writes in bulk, "
              + "texture / sampler bindings reused, unchanged texture requests skipped), without the parts that made "
              + "the game wait (r13 test). Recommended.",
              "Native APK, test: all eleven speed-ups. Slower on 2K16 in the r13 test (about 30 FPS).",
              "Native APK: none of them."));

    public static final Choice CROWD_CULL = new Choice(S_ADVANCED, "crowd_cull", "CROWD CULLING", 0,
            a("on", "off"),
            a("ON (FAST)", "OFF"),
            a("Native APK: crowd members entirely outside the camera's view are not drawn (exact: nothing visible "
              + "changes). Fewer draws for the GPU command thread, mostly in entrances.",
              "Native APK: every crowd member the game sends is drawn."));

    public static final Choice HOT_CORES = new Choice(S_ADVANCED, "hot_cores", "BUSY THREADS ON OWN CORES", 0,
            a("on", "off"),
            a("ON (FAST)", "OFF"),
            a("The two busiest threads (the game's main thread and the GPU command thread) each get one of the "
              + "fastest cores to themselves; every other thread runs on the remaining cores. Checked every 2 s.",
              "Threads placed by CPU CORES only."));

    // r18: when the phone gets hot the game lowers its own frame cap for a while, so a long session stays steady
    // instead of collapsing (a Mali-G57 held ~58 FPS then fell to 10 FPS from heat at a 60 cap, vivo V2432,
    // 2026-11-23). It only ever lowers, never above your cap, and lifts again as the phone cools. A cool phone (most
    // Adreno sessions) never reaches it, so nothing changes there.
    // r19: how hard the phone is pushed. The 1.2.0 testers' phones ran smoothly but got hot: the busiest game threads
    // were pinned to the single fastest core (the one that heats most), the GPU thread spun on a core while waiting,
    // and the heat governor only acted once Android already reported the phone hot. BALANCED keeps the frame rate but
    // avoids those; COOL also lowers the cap. Applied on top of the other settings at launch (Settings.buildLaunch).
    public static final Choice PERF_MODE = new Choice(S_GRAPHICS, "perf_mode", "PERFORMANCE MODE", 1,
            a("max", "balanced", "cool"),
            a("MAX (HOTTER)", "BALANCED", "COOL (BATTERY)"),
            a("Everything for frame rate: the busiest threads on the fastest core, the GPU thread spins while waiting, "
              + "heat protection only when Android says the phone is hot. Runs hottest.",
              "Recommended. Same frame rate target, less heat: threads kept off the single hottest core, the GPU "
              + "thread sleeps while waiting, and heat protection acts early from Android's heat forecast.",
              "Coolest and longest battery: also caps the frame rate (45 on Adreno, 30 on Mali). For long sessions, "
              + "hot rooms or phones that heat up quickly."));

    public static final Choice THERMAL = new Choice(S_ADVANCED, "thermal_guard", "HEAT PROTECTION", 0,
            a("on", "off"),
            a("ON (COOLER)", "OFF"),
            a("Lowers the frame rate while the phone is hot, then lifts it as it cools: steadier long sessions, less "
              + "heat. Recommended, especially on Mali phones.",
              "The frame rate never changes for heat (the phone may get hot and throttle itself)."));

    public static final Choice GPU_WAIT = new Choice(S_ADVANCED, "gpu_wait", "GPU THREAD WAITING", 0,
            a("spin", "sleep"),
            a("SPIN (FAST)", "SLEEP (COOLER)"),
            a("The GPU command thread spins while it waits for the game: quickest answers, but one fast core stays "
              + "busy (more heat in long sessions).",
              "Native APK, test: it sleeps 50 microseconds per check instead. Less heat, so the frame rate may hold "
              + "better in long sessions; answers can come a little later."));

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
            PERF_MODE, RENDERER, RESOLUTION, CROWD, ENTRANCE_CROWD, SHADOWS, DOF, PASS_TRIM, FRAME_RATE, MSAA, FAST_AA, UPSCALER, TEXTURES, FILTER,
            DISPLAY, SCREEN_SHAPE, VSYNC, SHARPNESS,
            CPU_CORES, SPEED_FIX, SHADER_CACHE, FRAME_MONITOR, PERF_HINT, SUSTAINED,
            TOUCH, TOUCH_VIBRATE, RUMBLE,
            READBACK, MALI_BOOST, MALI_TEXTURES, SHORTCUTS, CP_FAST, CROWD_CULL, HOT_CORES, THERMAL, GPU_WAIT, NATIVE_FRAME, TURNIP_FIX, PERF_LOG, VK_DEBUG,
    };

    public static final String KEY_EXTRA_ARGS = "extra_args";
    public static final String KEY_DRIVER = "gpu_driver";  // "" = the phone's driver, else the driver folder's name

    // --- Simple / expert settings (r21) ----------------------------------------------------------------------------
    // Players changed renderer switches they did not need (NATIVE instead of NATIVE+, shadows ON on a Mali, a Turnip
    // driver on a Mali phone, ...) and then the game ran badly or crashed (1.2.1 logs, 2026-11-25). Normal players now see
    // only the settings below; everything else is hidden AND held at the tested value for their GPU at every launch
    // (presetValue). EXPERT MODE (asked with a warning) shows and uses every setting, the extra arguments too.
    public static final String KEY_EXPERT = "expert_mode";

    public static final Choice[] SIMPLE = {
            PERF_MODE, FRAME_RATE, RESOLUTION, CROWD, SHADOWS, TEXTURES, FILTER, UPSCALER,
            SCREEN_SHAPE, SHARPNESS, FRAME_MONITOR, DISPLAY,
            TOUCH, TOUCH_VIBRATE, RUMBLE,
    };

    public static boolean isExpert(Context c) {
        return prefs(c).getBoolean(KEY_EXPERT, false);
    }

    public static boolean isSimple(Choice c) {
        for (Choice s : SIMPLE) if (s == c) return true;
        return false;
    }

    /** The choices of a section a player sees: all in expert mode, the SIMPLE ones otherwise. */
    public static List<Choice> visibleInSection(Context ctx, String section) {
        List<Choice> out = new ArrayList<>();
        boolean expert = isExpert(ctx);
        for (Choice c : CHOICES) {
            if (expert ? c.section.equals(section) : (isSimple(c) && simpleSectionOf(c).equals(section))) out.add(c);
        }
        return out;
    }

    /** Where a simple setting is shown when expert mode is off (FRAME MONITOR moves to Display). */
    public static String simpleSectionOf(Choice c) {
        if (c == FRAME_MONITOR) return S_DISPLAY;
        return c.section;
    }

    /** The sections shown: Performance and Advanced only in expert mode. */
    public static String[] visibleSections(Context ctx) {
        if (isExpert(ctx)) return SECTIONS;
        return new String[] {S_GRAPHICS, S_DISPLAY, S_CONTROLS, S_DRIVER, S_CREDITS};
    }

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
        e.remove(KEY_QUALITY);
        e.apply();
        applyPreset(ctx, Gpu.family(ctx));  // r21: reset = the tested settings for this GPU
    }

    // --- GPU presets (r18, r19) -----------------------------------------------------------------------------------
    // The best settings differ by GPU. On first start (and once more after an update that improves them: the key's
    // version) the launcher detects the GPU (Gpu.family / Gpu.maliTier) and applies the matching preset; the player
    // can re-apply it any time from Graphics. Re-applying only sets the keys below.

    // r21: v3 = applied once more after 1.2.2 (shortcuts off, shadows off, Mali filter / textures / sustained mode).
    public static final String KEY_PRESET_APPLIED = "preset_applied_v3";

    private static void put(SharedPreferences.Editor e, Choice c, String value) {
        e.putString(c.key, value);
    }

    /** The tested value of every choice for this phone's GPU (choices not listed keep their default). */
    public static java.util.LinkedHashMap<Choice, String> presetValues(Context ctx, String family) {
        java.util.LinkedHashMap<Choice, String> m = new java.util.LinkedHashMap<>();
        boolean mali = Gpu.MALI.equals(family);
        int tier = mali ? Gpu.maliTier(ctx) : Gpu.MALI_NONE;
        int gen = Gpu.ADRENO.equals(family) ? Gpu.adrenoGeneration(ctx) : 0;
        // Shared base (the tested Adreno setup).
        m.put(RENDERER, "native_plus");
        m.put(RESOLUTION, "480");
        m.put(CROWD, "20");
        m.put(ENTRANCE_CROWD, "10");
        // Dynamic shadows OFF everywhere: every shadow caster's map is its own render pass (440-540 passes a frame with
        // shadows, about 95-110 without, 2026-11-22), the biggest single GPU cost and a big part of the heat.
        m.put(SHADOWS, "off");
        m.put(MSAA, "off");
        m.put(FRAME_GEN, "off");
        m.put(NATIVE_FRAME, "on");
        m.put(SHORTCUTS, "safe");
        m.put(TURNIP_FIX, "on");
        m.put(CROWD_CULL, "on");
        m.put(HOT_CORES, "on");
        m.put(PASS_TRIM, "off");
        m.put(READBACK, "auto");
        m.put(CP_FAST, "safe");
        m.put(THERMAL, "on");
        m.put(PERF_MODE, "balanced");
        m.put(UPSCALER, "sgsr");
        m.put(TEXTURES, "0");
        m.put(FAST_AA, "none");
        m.put(SPEED_FIX, "1");
        m.put(SHADER_CACHE, "1");
        m.put(PERF_HINT, "1");
        m.put(CPU_CORES, "auto");
        m.put(VSYNC, "1");
        m.put(PERF_LOG, "off");
        m.put(VK_DEBUG, "0");
        if (mali) {
            // The safe half of the Mali boost only (geometry shaders avoided); the other two switches and the ASTC
            // textures were suspects in the 1.2.0 Mali glitches (flat brown / orange scenes, stretched ropes).
            m.put(MALI_BOOST, "nogs");
            m.put(MALI_TEXTURES, "normal");
            m.put(GPU_WAIT, "sleep");
            m.put(DOF, "off");
            // r21: Android's sustained-performance mode: steady clocks instead of bursts that overheat and throttle.
            m.put(SUSTAINED, "1");
            if (tier == Gpu.MALI_LOW) {
                m.put(RESOLUTION, "360");
                m.put(FRAME_RATE, "30");
                m.put(CROWD, "0");
                m.put(ENTRANCE_CROWD, "0");
                m.put(FILTER, "0");
            } else {
                // r21: 2x filtering (floors and the ring mat were blurry with none: "texture was not good").
                m.put(FILTER, "2");
                m.put(FRAME_RATE, "45");
                if (tier == Gpu.MALI_HIGH) m.put(RESOLUTION, "540");
            }
        } else {
            // Adreno / others: 60 cap, DoF on — the r14-r16 setup (Nothing A059 ~50-55 in matches).
            m.put(FRAME_RATE, "60");
            m.put(GPU_WAIT, "spin");
            m.put(MALI_BOOST, "nogs");
            m.put(MALI_TEXTURES, "normal");
            m.put(DOF, "on");
            m.put(SUSTAINED, "0");
            m.put(FILTER, "3");
            if (gen > 0 && gen <= 6) m.put(FILTER, "2");
        }
        return m;
    }

    /** Applies the best-known settings for this phone's GPU (family: "mali", "adreno", others get the Adreno set). */
    public static void applyPreset(Context ctx, String family) {
        SharedPreferences.Editor e = prefs(ctx).edit();
        for (java.util.Map.Entry<Choice, String> en : presetValues(ctx, family).entrySet()) put(e, en.getKey(), en.getValue());
        e.putBoolean(KEY_PRESET_APPLIED, true);
        e.apply();
        enforceLocks(ctx);
    }

    /** The value a hidden setting is held at for a normal player: the GPU's tested value, else the default. */
    public static String presetValue(Context ctx, Choice c) {
        String v = presetValues(ctx, Gpu.family(ctx)).get(c);
        return v != null ? v : c.values[c.defaultIndex];
    }

    // --- Quality levels (r21): one tap instead of eight settings -----------------------------------------------------
    public static final String Q_SMOOTH = "smooth", Q_BALANCED = "balanced", Q_QUALITY = "quality";
    public static final String KEY_QUALITY = "quality_level";

    public static void applyQuality(Context ctx, String level) {
        String family = Gpu.family(ctx);
        java.util.LinkedHashMap<Choice, String> m = presetValues(ctx, family);
        SharedPreferences.Editor e = prefs(ctx).edit();
        for (Choice c : new Choice[] {RESOLUTION, CROWD, ENTRANCE_CROWD, SHADOWS, TEXTURES, FILTER, FRAME_RATE, DOF}) {
            String v = m.get(c);
            if (v != null) put(e, c, v);
        }
        boolean mali = Gpu.MALI.equals(family);
        if (Q_SMOOTH.equals(level)) {
            put(e, RESOLUTION, mali ? "360" : "480");
            put(e, CROWD, "0");
            put(e, ENTRANCE_CROWD, "0");
            put(e, SHADOWS, "off");
            put(e, FILTER, mali ? "0" : "2");
            put(e, DOF, "off");
            put(e, FRAME_RATE, mali ? "30" : "45");
        } else if (Q_QUALITY.equals(level)) {
            put(e, RESOLUTION, "540");
            put(e, CROWD, "50");
            put(e, ENTRANCE_CROWD, "10");
            put(e, SHADOWS, mali ? "off" : "half");
            put(e, FILTER, mali ? "2" : "4");
            put(e, TEXTURES, "0");
        }
        e.putString(KEY_QUALITY, level);
        e.apply();
        enforceLocks(ctx);
    }

    /** First start (or first start of a version with a better preset): apply it once. Returns the family, or null. */
    public static String applyPresetOnceForThisGpu(Context ctx) {
        SharedPreferences p = prefs(ctx);
        if (p.getBoolean(KEY_PRESET_APPLIED, false)) return null;
        String family = Gpu.family(ctx);
        applyPreset(ctx, family);
        return family;
    }

    // --- High-settings lock for Mali (r19) --------------------------------------------------------------------------
    // Settings that make the game unplayable on a Mali phone (single-digit FPS, overheating, glitches) cannot be chosen
    // there. The limits depend on the Mali tier (Gpu.maliTier). Adreno and other GPUs have no locks.

    /** Why `value` of `c` is locked on this phone, or null when it may be chosen. */
    public static String lockReason(Context ctx, Choice c, String value) {
        // r19: frame generation needs a GPU that paints the in-between frames in time: Adreno 7xx / 8xx. Elsewhere it
        // only lowered the real frame rate (and on a Mali-G52 lost the GPU device), so it is locked.
        // r21: frame generation is off for everyone (it lowered the real frame rate and smeared fast moves).
        if (c == FRAME_GEN && "on".equals(value)) return "Frame generation is switched off in this version";
        int tier = Gpu.maliTier(ctx);
        if (tier == Gpu.MALI_NONE) return null;
        String tierName = Gpu.maliTierName(tier) + " Mali";
        if (c == FRAME_GEN && "on".equals(value)) return "Frame generation is locked on Mali (it lowers the real FPS)";
        if (c == MSAA && !"off".equals(value)) return "MSAA is locked on Mali";
        if (c == MALI_BOOST && "all".equals(value)) return "The experimental Mali boost is locked (it caused glitches)";
        if (c == CP_FAST && "all".equals(value)) return "Locked on Mali";
        if (c == SHADOWS && "on".equals(value)) return "Full shadows are locked on Mali";
        if (c == SHADOWS && "half".equals(value) && tier < Gpu.MALI_HIGH) return "Shadows are locked on " + tierName;
        if (c == RESOLUTION) {
            int max = tier == Gpu.MALI_HIGH ? 540 : 480;
            if (parse(value) > max) return "Above " + max + "p is locked on " + tierName;
        }
        if (c == CROWD) {
            int max = tier == Gpu.MALI_HIGH ? 50 : 20;
            if (parse(value) > max) return "Crowd above " + max + "% is locked on " + tierName;
        }
        if (c == FRAME_RATE && "60".equals(value) && tier < Gpu.MALI_HIGH) {
            return "60 FPS is locked on " + tierName + " (it overheats and drops to a stutter)";
        }
        if (c == FILTER && parse(value) > 2 && tier < Gpu.MALI_HIGH) return "Locked on " + tierName;
        if (c == DOF && "on".equals(value) && tier == Gpu.MALI_LOW) return "Depth of field is locked on " + tierName;
        if (c == RENDERER && "classic".equals(value)) return "The classic renderer is locked on Mali (far slower)";
        return null;
    }

    private static int parse(String v) {
        try {
            return Integer.parseInt(v);
        } catch (Exception e) {
            return 0;
        }
    }

    /** True when at least one value of this choice is locked on this phone (shown on its row). */
    public static boolean hasLocks(Context ctx, Choice c) {
        for (String v : c.values) if (lockReason(ctx, c, v) != null) return true;
        return false;
    }

    /**
     * Moves every locked setting to the nearest allowed value (run at start and before each launch, so an old saved
     * value or an edited preferences file can't get past the lock). Returns how many were changed.
     */
    public static int enforceLocks(Context ctx) {
        SharedPreferences p = prefs(ctx);
        SharedPreferences.Editor e = p.edit();
        int changed = 0;
        for (Choice c : CHOICES) {
            String v = c.value(p);
            if (lockReason(ctx, c, v) == null) continue;
            int i = c.index(p);
            String pick = null;
            for (int d = 1; d < c.values.length && pick == null; d++) {  // the nearest allowed, lower first
                for (int sgn : new int[] {+1, -1}) {
                    int j = i + sgn * d;
                    if (j >= 0 && j < c.values.length && lockReason(ctx, c, c.values[j]) == null) {
                        pick = c.values[j];
                        break;
                    }
                }
            }
            if (pick != null) {
                e.putString(c.key, pick);
                changed++;
            }
        }
        if (changed > 0) e.apply();
        return changed;
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
        enforceLocks(ctx);  // r19: no locked value reaches the game, whatever is saved
        SharedPreferences p = prefs(ctx);
        // r21: a normal player's hidden settings are held at the tested value for this GPU (an old tweak, or one made
        // before this version, can no longer make the game slow or crash).
        final boolean expert = isExpert(ctx);
        if (!expert) {
            SharedPreferences.Editor hold = p.edit();
            for (Choice c : CHOICES) if (!isSimple(c)) hold.putString(c.key, presetValue(ctx, c));
            hold.putString(FRAME_GEN.key, "off");
            hold.apply();
        }
        Launch l = new Launch();
        // Resolution: the SDK draws at whole multiples of the game's 720p.
        final boolean nat = isNativeBuild(ctx);
        final String renderer = RENDERER.value(p);
        final boolean nativeRenderer = nat && !"classic".equals(renderer);
        if (nat) {
            l.env.put("WWE2K16_RENDERER", renderer);
            l.env.put("WWE2K16_NATIVE_SHORTCUTS", "all".equals(SHORTCUTS.value(p)) ? "1" : "0");
            l.env.put("WWE2K16_PASS_TRIM", "on".equals(PASS_TRIM.value(p)) ? "1" : "0");
            l.env.put("WWE2K16_CP_FAST", CP_FAST.value(p));
            if ("on".equals(CROWD_CULL.value(p))) cvar(l, "gpu_scene_crowd_cull", "2");
            // r19: BALANCED / COOL make the GPU thread sleep while it waits (a spinning wait keeps a core at 100%).
            if ("sleep".equals(GPU_WAIT.value(p)) || !"max".equals(PERF_MODE.value(p))) {
                cvar(l, "gpu_wait_reg_mem_sleep_us", "50");
            }
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
            boolean frameGen = false;  // r21: frame generation removed for everyone
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
        boolean fg = false;  // r21
        // r19: the player's own cap, restored if frame generation gives up for the session (SDK patch 0009).
        l.env.put("WWE2K16_FG_RESTORE_CAP", "45".equals(fr) ? "45" : "30".equals(fr) ? "30" : "0");
        if (fg) fr = "30";  // frame generation v3: a steady 30 from the game, 60 on the screen
        // r19: COOL caps the frame rate (45 on Adreno and others, 30 on Mali), never raising a lower choice.
        if ("cool".equals(PERF_MODE.value(p)) && !fg) {
            String coolCap = Gpu.isMali(ctx) ? "30" : "45";
            if ("60".equals(fr) || "classic".equals(fr) || ("45".equals(fr) && "30".equals(coolCap))) fr = coolCap;
        }
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
        // r19: 1 = MAX (the two fastest cores), 2 = BALANCED (a lone prime core skipped), 3 = COOL (and left idle).
        String perfMode = PERF_MODE.value(p);
        l.env.put("WWE2K16_HOT_CORES", !"on".equals(HOT_CORES.value(p)) ? "0"
                : "max".equals(perfMode) ? "1" : "cool".equals(perfMode) ? "3" : "2");
        l.env.put("WWE2K16_SPEED_FIX", SPEED_FIX.value(p));
        boolean cache = "1".equals(SHADER_CACHE.value(p));
        cvar(l, nat ? "vulkan_disk_pipeline_cache" : "vulkan_pipeline_cache", cache ? "true" : "false");
        cvar(l, "async_shader_compilation", "true");
        // r21: hold up to 8 frames (not 3) while a new shader compiles before showing one with a draw missing: fewer
        // half-drawn / black frames when a new scene starts (Mali phones compile slowly: up to 4.9 s for one shader).
        if (nat) cvar(l, "vulkan_async_skip_incomplete_frames_max", "8");
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
        // r17: the phone's GPU family (Gpu: read from the phone's OpenGL ES driver) for the GPU-specific choices below.
        final String gpuFamily = Gpu.family(ctx);
        final boolean mali = Gpu.MALI.equals(gpuFamily);
        l.env.put("WWE2K16_GPU_FAMILY", gpuFamily);
        // r18: heat protection. The game lowers its own cap when the phone reports it is hot (GameActivity feeds it the
        // Android thermal status). Default on; on Mali the governor is a touch earlier since those phones throttle hard.
        boolean coolMode = "cool".equals(PERF_MODE.value(p));
        l.env.put("WWE2K16_THERMAL_GUARD", "off".equals(THERMAL.value(p)) ? "0" : ((mali || coolMode) ? "2" : "1"));
        // r19: BALANCED / COOL also use Android's heat forecast, so the cap drops before the phone is hot.
        l.env.put("WWE2K16_THERMAL_FORECAST",
                !"off".equals(THERMAL.value(p)) && !"max".equals(PERF_MODE.value(p)) ? "1" : "0");
        if (nat) {
            // The 0.10 SDK names its log with --log_file (0.7 reads WWE2K16_LOG_FILE).
            cvar(l, "log_file", new File(logs, "wwe2k16.log").getAbsolutePath());
            String readback = READBACK.value(p);
            l.env.put("WWE2K16_READBACK_ALWAYS", "always".equals(readback) ? "1" : "0");
            if ("latch".equals(readback) || ("auto".equals(readback) && !Gpu.ADRENO.equals(gpuFamily))) {
                cvar(l, "readback_resolve_attire_latch", "300");
            }
            if (mali) {
                String boost = MALI_BOOST.value(p);
                if (!"off".equals(boost)) cvar(l, "gpu_quirks_mobile_no_gs", "true");
                if ("all".equals(boost)) {
                    cvar(l, "gpu_quirks_tiler_classic_passes", "true");
                    cvar(l, "gpu_quirks_vertex_input_phone", "true");
                }
            }
            // ASTC / EAC instead of full-size RGBA8 (the SDK applies it only when the GPU lacks the BC formats).
            if ("small".equals(MALI_TEXTURES.value(p))) {
                cvar(l, "vulkan_texture_transcode", "true");
                cvar(l, "vulkan_texture_transcode_eac", "true");
            }
        }

        // Custom driver.
        String compiler = "";
        String driver = p.getString(KEY_DRIVER, "");
        if (!driver.isEmpty()) {
            Drivers.Driver d = Drivers.find(ctx, driver);
            // r21: a driver for another GPU family is never used (Turnip on a Mali phone did nothing but confuse).
            if (d != null && Drivers.wrongFamily(ctx, d)) d = null;
            if (d != null) {
                // r21: the game reports whether the driver really took over (Drivers.STATUS_FILE): a Turnip package
                // that the phone silently replaced with its own driver (Adreno 650, Android 14) is switched away from.
                l.env.put("WWE2K16_DRIVER_STATUS", Drivers.statusFile(ctx).getAbsolutePath());
                if (d.isTurnip()) l.env.put("WWE2K16_DRIVER_EXPECT", "turnip");
                l.env.put("WWE2K16_DRIVER", d.library);
                l.env.put("WWE2K16_DRIVER_DIR", d.dir.getAbsolutePath() + "/");
                l.env.put("WWE2K16_DRIVER_TRIAL", Drivers.trialFile(ctx).getAbsolutePath());
                // Turnip on newer Adrenos is steadier without its tile-based fast path.
                if (d.isTurnip()) l.env.put("TU_DEBUG", "sysmem");
                // malisx2 (Mesa PanVK on Mali) has no MSAA: the game's scene must be built 1x.
                if (Gpu.MALI.equals(d.family) && nat) cvar(l, "wwe2k16_force_no_msaa", "true");
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
        String extra = expert ? p.getString(KEY_EXTRA_ARGS, "").trim() : "";
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
        l.summary.append("EXPERT MODE: ").append(expert ? "ON" : "OFF").append('\n');
        l.summary.append("BUILD: ").append(nat ? "native (WWE 2K14 SDK)" : "classic (ReXGlue 0.7)").append('\n');
        l.summary.append("GPU DRIVER: ").append(driver.isEmpty() ? "System" : driver).append('\n');
        l.summary.append("GPU: ").append(Gpu.renderer(ctx)).append(" (").append(gpuFamily).append(")\n");
        if (mali) l.summary.append("MALI TIER: ").append(Gpu.maliTierName(Gpu.maliTier(ctx))).append('\n');
        l.summary.append("DLC: ").append(Dlc.installed(ctx).size()).append(" installed, ").append(Dlc.waiting(ctx).size())
                .append(" waiting\n");
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
