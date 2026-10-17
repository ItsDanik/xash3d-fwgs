#ifndef MISTER_PORT_H
#define MISTER_PORT_H

// MiSTer hybrid core: the game runs on the ARM, the FPGA core shows the
// picture at 15kHz and provides audio and input. SDL's "mister" drivers
// (hybrid/sdl2) do most of the work; this is what is specific to the game.
//
// Starting point for a port: copy both files into the game's source, build
// them when MISTER_HYBRID is on, and call them from the few places named
// below. Everything is a no-op without the core, so the PC build still runs.

#ifdef __cplusplus
extern "C" {
#endif

// Joystick buttons as SDL sees them: the buttons of the core's "J1," list
// (core/<Name>.sv) in order, then the two the SDL driver adds. Rename the
// first eight after the game's actions.
enum {
    MISTER_JOY_ACTION1,
    MISTER_JOY_ACTION2,
    MISTER_JOY_ACTION3,
    MISTER_JOY_ACTION4,
    MISTER_JOY_ACTION5,
    MISTER_JOY_ACTION6,
    MISTER_JOY_ACTION7,
    MISTER_JOY_MENU,
    MISTER_JOY_CORE_OK,
    MISTER_JOY_CORE_BACK,

    MISTER_JOY_MENU_OK = 28,
    MISTER_JOY_MENU_BACK = 29
};
// In the game's menus: confirm and go back
#define MISTER_JOY_OK_MASK ((1 << MISTER_JOY_CORE_OK) | (1 << MISTER_JOY_MENU_OK))
#define MISTER_JOY_BACK_MASK ((1 << MISTER_JOY_CORE_BACK) | (1 << MISTER_JOY_MENU_BACK))

// Frame pacing: call once per frame in place of the game's own wait. Sleeps
// until the core's next field and returns how many fields (1/59.64s) have
// passed since the last call, which is the game's clock. 0 without the core:
// use the original timing then.
unsigned MiSTer_WaitFrame(void);
// Seconds per field: 400 x 262 pixels at 6.25MHz
#define MISTER_FIELD_TIME (104800.0 / 6250000.0)
// Whether MiSTer_WaitFrame() is pacing the game (the core is there)
int MiSTer_Paced(void);
// The next call starts counting anew, after the game stood still (loading)
void MiSTer_ResetFrames(void);

// Problems the player can fix (missing data): shown on the screen until a
// button is pressed, in plain words that say which files go where
void MiSTer_ShowError(const char *message);
// Development, MISTER_HYBRID_FPSLOG=1: where the time of a frame goes. The
// engine says what it starts doing; the log has the milliseconds per frame of
// each. MISTER_HYBRID_PROFILE=<file> samples the program counter of the game
// thread as well, 1000 times per second of its CPU time, and writes the
// samples there on leaving (tools/profile.py reads them).
enum {
    MISTER_T_OTHER,
    MISTER_T_WAIT, // for the core's next field
    MISTER_T_INPUT,
    MISTER_T_SERVER,
    MISTER_T_CLIENT,
    MISTER_T_RENDER, // the 3D view
    MISTER_T_HUD, // what is drawn over it
    MISTER_T_BLIT, // to the window and the core
    MISTER_T_SOUND,
    MISTER_T_COUNT
};
void MiSTer_Mark(int what);

// Rows of the game's video mode, which is 320 wide: 200 or 240 (OSD option Resolution)
int MiSTer_GameHeight(void);
// The HUD margins of the OSD's CRT Options for a screen of `height` rows: the
// rows between the HUD and the top and bottom edge. 0 in 200 rows (the HUD
// lines are greyed out then), twice the OSD's in 480. Read every frame.
void MiSTer_HudMargins(int height, int *top, int *bottom);

// The OSD's Cheats page, for the server's frame: the MISTER_CHEAT_* switches
// that are on, and the triggers selected in this frame. `player`: 0 while
// there is none to give them to, and the triggers are dropped
#define MISTER_CHEAT_GOD      1 // God Mode
#define MISTER_CHEAT_NOCLIP   2 // No Clip
#define MISTER_CHEAT_NOTARGET 4 // Invisible to Monsters
#define MISTER_CHEAT_GIVE     8 // the trigger Give All Weapons
#define MISTER_CHEAT_LONGJUMP 16 // the trigger Long Jump Module
#define MISTER_CHEAT_SWITCHES 7
int MiSTer_Cheats(int player);

// Before the engine starts: the game of the menu entry the core was loaded
// from (MISTER_HYBRID_SET, Xash3D_<id>), or the one installed next to the
// binary, or the one the player picks from the list of those (MH_PickGame()).
// Then "Loading <title>..." until the first frame. Sets XASH3D_GAME. Returns 0
// to quit: the player left the list, or the game is not there (said on the
// screen until the player left the core)
int MiSTer_SelectGame(void);
// Exit code of the process after the player quit: the launcher starts the
// game again (back to the list of MiSTer_SelectGame) or returns to the menu
int MiSTer_ExitCode(void);

#ifdef __cplusplus
}
#endif

#endif
