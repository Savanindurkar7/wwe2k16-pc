package com.savanindurkar7.wwe2k16;

/** The credits shown on the home screen and in OPTIONS > CREDITS. Edit here. */
final class Credits {
    private Credits() {}

    /** The port's author, shown first and on the home screen. */
    static final String PORT_BY = "SAVAN INDURKAR";
    static final String GITHUB = "github.com/Savanindurkar7";

    /** Short lines for the home screen's corner (label, value). */
    static final String[][] HOME = {
            {"PORT BY", PORT_BY},
            {"GITHUB", "SAVANINDURKAR7"},
    };

    /** Everyone whose work is in this app (shown in OPTIONS > CREDITS). */
    static final String[][] THANKS = {
            {"Android port", PORT_BY + " (" + GITHUB + ")"},
            {"WWE 2K16 PC recompilation", "Banshee (the PC port this Android version is built on)"},
            {"ReXGlue SDK", "The recompiler and runtime, with parts from the Xenia project"},
            {"WWE 2K14 recompilation", "Jarrod Feng: Android driver loader and SDK fixes (BSD 3-Clause)"},
            {"WWE '12 PC / Android", "iMrBurn: Android audio, timer, UI and thread fixes"},
            {"WWE 2K17 port", "Xav: the game speed fix method (time step from the real frame time)"},
            {"libadrenotools", "bylaws: custom Vulkan driver loading (BSD 2-Clause)"},
            {"SDL 3", "Window, input and audio on Android (zlib licence)"},
            {"FFmpeg", "Video and audio decoding (LGPL)"},
            {"Oswald font", "The Oswald Project Authors (SIL Open Font Licence)"},
    };

    static final String DISCLAIMER = "Fan-made and non-commercial. Not affiliated with or endorsed by WWE, 2K, "
            + "Take-Two, Yuke's or Visual Concepts. No game files are included: you need your own WWE 2K16 (Xbox 360).";
}
