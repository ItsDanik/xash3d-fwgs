// MiSTer hybrid core support, see mister_port.h

#include "mister_port.h"

#include <mister_hybrid.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define EXIT_TO_MENU 0
#define EXIT_RESTART 42 // danik_hybrid_launch.sh starts the game again
#define EXIT_RELOADED 43 // the same, after the core has settled

static const char *const TITLE = "Xash3D";

// OSD options of the game (CONF_STR in core/Xash3D.sv, status bits 24..63).
// The first entry of each is 0, the default.

// "O[24],Lock to 30 FPS,Off,On;": a frame every second field, an even 30 a
// second where the renderer cannot keep up with every field.
// MISTER_HYBRID_LOCK30=0|1 fixes it for a test.
static int Lock30(void)
{
    static int fixed = -2;

    if (fixed == -2)
        fixed = getenv("MISTER_HYBRID_LOCK30") ? atoi(getenv("MISTER_HYBRID_LOCK30")) != 0 : -1;
    if (fixed >= 0)
        return fixed;
    return MH_OSD_GAME_BITS(MH_OSDStatus(), 24, 1);
}

static uint32_t LastField;
static int FieldValid = 0;

// Development: MISTER_HYBRID_FPSLOG=1 prints the frames shown in every 300
// fields (5 seconds) and the longest wait for a frame, in fields
static void FpsLog(unsigned fields)
{
    static int enabled = -1;
    static unsigned frames, total, longest;

    if (enabled < 0)
        enabled = getenv("MISTER_HYBRID_FPSLOG") != NULL;
    if (!enabled)
        return;
    frames++;
    total += fields;
    if (fields > longest)
        longest = fields;
    if (total >= 300) {
        fprintf(stderr, "mister: %u frames in %u fields (%.1f per second), longest %u fields\n", frames, total,
                frames / (total * MISTER_FIELD_TIME), longest);
        frames = total = longest = 0;
    }
}

void MiSTer_ResetFrames(void)
{
    FieldValid = 0;
}

unsigned MiSTer_WaitFrame(void)
{
    uint32_t field;
    unsigned fields;

    if (!MH_IsOpen())
        return 0;
    if (!FieldValid) {
        LastField = MH_FieldCounter();
        FieldValid = 1;
    }
    MH_WaitFrame(LastField);
    if (Lock30())
        MH_WaitField(LastField + 1); // not before the second field after the last frame
    field = MH_FieldCounter();
    fields = field - LastField; // 0: the core is gone
    LastField = field;
    FpsLog(fields);
    return fields;
}

int MiSTer_Paced(void)
{
    return MH_IsOpen() && FieldValid;
}

// The player came through the list: quitting a game goes back to it
static int PickedFromList = 0;

int MiSTer_PickGame(const char *const *names, int count, int selected)
{
    int pick;

    if (!MH_Open())
        return selected;
    pick = MH_UI_Menu(TITLE, "Select a game:", names, count, selected);
    if (pick < 0 || pick >= count)
        return -1;
    PickedFromList = 1;
    MH_UI_Message(TITLE, "Loading...", 0);
    return pick;
}

void MiSTer_ShowError(const char *message)
{
    if (MH_Open())
        MH_UI_Message(TITLE, message, MH_UI_WAIT | MH_UI_ERROR);
}

// Folders of the games we build libraries for (build.sh), in the order of the list
static const struct {
    const char *dir, *name;
} Games[] = {
    { "valve", "Half-Life" },
    { "gearbox", "Half-Life: Opposing Force" },
    { "bshift", "Half-Life: Blue Shift" },
};
// our client library of a game: the MiSTer's, or this PC's for tests with fakecore
#ifdef __arm__
#define CLIENT_LIBRARY "cl_dlls/client_armv7hf.so"
#else
#define CLIENT_LIBRARY "cl_dlls/client_amd64.so"
#endif
#define NUM_GAMES ((int)(sizeof(Games) / sizeof(Games[0])))

static int HasFile(const char *base, const char *dir, const char *file)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s/%s", base, dir, file);
    return access(path, R_OK) == 0;
}

int MiSTer_SelectGame(void)
{
    const char *base = getenv("XASH3D_BASEDIR");
    const char *names[NUM_GAMES];
    int found[NUM_GAMES];
    int count = 0, pick = 0, i;

    // a game given on the command line, or no core: as on a PC
    if (getenv("XASH3D_GAME") || !MH_Open())
        return 1;
    if (!base)
        base = ".";
    // every game takes what it does not have itself from Half-Life's folder
    if (HasFile(base, "valve", "liblist.gam") && HasFile(base, "valve", "halflife.wad")) {
        for (i = 0; i < NUM_GAMES; i++) {
            if (HasFile(base, Games[i].dir, "liblist.gam") && HasFile(base, Games[i].dir, CLIENT_LIBRARY)) {
                found[count] = i;
                names[count++] = Games[i].name;
            }
        }
    }
    if (!count) {
        MiSTer_ShowError("Half-Life was not found.\n\n"
                         "Copy the folder \"valve\" of your Half-Life to games/Xash3D on the SD card, "
                         "over the one that is there. \"gearbox\" (Opposing Force) and \"bshift\" "
                         "(Blue Shift) go next to it.");
        return 0;
    }
    if (count > 1) {
        pick = MiSTer_PickGame(names, count, 0);
        if (pick < 0)
            return 0;
    }
    setenv("XASH3D_GAME", Games[found[pick]].dir, 1);
    return 1;
}

int MiSTer_ExitCode(void)
{
    // The player loaded the core again while the game ran
    if (MH_CoreReloaded())
        return EXIT_RELOADED;
    // Not if we are quitting because another core was loaded
    if (PickedFromList && MH_Open())
        return EXIT_RESTART;
    return EXIT_TO_MENU;
}
