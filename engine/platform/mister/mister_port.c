#define _GNU_SOURCE
// MiSTer hybrid core support, see mister_port.h

#include "mister_port.h"

#include <mister_hybrid.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define EXIT_TO_MENU 0
#define EXIT_RESTART 42 // danik_hybrid_launch.sh starts the game again
#define EXIT_RELOADED 43 // the same, after the core has settled

static const char *const TITLE = "Xash3D";

// OSD options of the game (CONF_STR in core/Xash3D.sv, status bits 24..63).
// The first entry of each is 0, the default.

// "O[25],Resolution,320x200,320x240;": the video mode of the game (the menu
// has its own, see VID_MiSTerMode() in client/vid_common.c). Applies at once.
// MISTER_HYBRID_HEIGHT=200|240 fixes it for a test.
int MiSTer_GameHeight(void)
{
    static int fixed = -1;

    if (fixed < 0)
        fixed = getenv("MISTER_HYBRID_HEIGHT") ? atoi(getenv("MISTER_HYBRID_HEIGHT")) : 0;
    if (fixed)
        return fixed == 240 ? 240 : 200;
    return MH_IsOpen() && MH_OSD_GAME_BITS(MH_OSDStatus(), 25, 1) ? 240 : 200;
}

// "D0P1O[81:77],HUD (Top)" and "D0P1O[86:82],HUD (Bottom)" (core/Xash3D.sv),
// greyed out with mask bit 0 while the game's mode is 320x200 (MiSTer_WaitFrame()).
// The game's library draws the HUD where it would on a screen of the rows
// between them (CL_GetScreenInfo() in client/dll_int/cl_game.c).
// MISTER_HYBRID_HUD=<top>,<bottom> fixes them for a test (the library).
void MiSTer_HudMargins(int height, int *top, int *bottom)
{
    int scale = height >= 480 ? 2 : 1;

    *top = *bottom = 0;
    if (height < 240 || !MH_IsOpen())
        return;
    MH_HudMargins(top, bottom);
    *top *= scale;
    *bottom *= scale;
}

static void HudLines(void)
{
    static int mask = -1;
    int m = MiSTer_GameHeight() == 240 ? 0 : 1;

    if (m != mask)
        MH_SetMenuMask(m);
    mask = m;
}

// "O[26],Lock to 30 FPS,Off,On;": a frame every second field, an even 30 a
// second where the renderer cannot keep up with every field. Off is still at
// most one frame per field: 60. MISTER_HYBRID_LOCK30=0|1 fixes it for a test.
static int Lock30(void)
{
    static int fixed = -2;

    if (fixed == -2)
        fixed = getenv("MISTER_HYBRID_LOCK30") ? atoi(getenv("MISTER_HYBRID_LOCK30")) : -1;
    if (fixed >= 0)
        return fixed != 0;
    return MH_OSD_GAME_BITS(MH_OSDStatus(), 26, 1);
}

// "P2O[27],God Mode", "P2O[28],No Clip", "P2O[29],Invisible to Monsters" and
// the triggers "P2T[56],Give All Weapons", "P2T[57],Long Jump Module"
// (core/Xash3D.sv), in the order of the MISTER_CHEAT_* bits. The triggers are
// read every frame (MiSTer_WaitFrame()) and kept for the server's next frame:
// one selected while no level runs (the menus, paused) is dropped after half
// a second without one.
// MISTER_HYBRID_CHEATS=<bits> fixes them for a test: the switches stay, the
// triggers are selected once there is a player.
static int TriggersPending, CheatsAsked, NotAsked;

static void PollTriggers(void)
{
    int triggers = MH_OSDTriggers() & 3;

    if (CheatsAsked)
        NotAsked = 0;
    else if (++NotAsked > 30)
        TriggersPending = 0;
    CheatsAsked = 0;
    TriggersPending |= triggers << 3;
}

int MiSTer_Cheats(int player)
{
    static int fixed = -2;
    int triggers;

    if (fixed == -2)
        fixed = getenv("MISTER_HYBRID_CHEATS") ? atoi(getenv("MISTER_HYBRID_CHEATS")) : -1;
    if (fixed >= 0) {
        if (!player)
            return fixed & MISTER_CHEAT_SWITCHES;
        triggers = fixed & ~MISTER_CHEAT_SWITCHES;
        fixed &= MISTER_CHEAT_SWITCHES;
        return fixed | triggers;
    }
    if (!MH_IsOpen())
        return 0;
    CheatsAsked = 1;
    triggers = player ? TriggersPending : 0;
    TriggersPending = 0;
    return (int)MH_OSD_GAME_BITS(MH_OSDStatus(), 27, 3) | triggers;
}

// "O[32:31],Rumble,On,Strong,Off;" (core/Xash3D.sv): the strengths below at
// On, twice them at Strong (bit 30 was Give All Weapons up to host version 12,
// and is left alone). What rumbles: the player's shots, the gauss charging
// and the egon's beam, damage taken, explosions nearby, and the screen shakes
// of the maps and monsters, all of it only while a level runs. A shot or a hit
// is a pulse of its own length; the strongest of what is going on is given to
// the motors every frame (RumbleUpdate()). MISTER_HYBRID_RUMBLELOG=1 prints
// every change of what they are given.
#define RUMBLE_PULSES 4

struct rumble {
    int strong, weak;
    long long until; // milliseconds, RumbleNow()
};
static struct rumble Pulses[RUMBLE_PULSES], Shake;
static int RumbleState, Egon, GaussSpin;
static int RumbleLog = -1;

// MISTER_HYBRID_RUMBLELOG=1: what made it rumble
static void RumbleWhy(const char *what, float value)
{
    if (RumbleLog < 0)
        RumbleLog = getenv("MISTER_HYBRID_RUMBLELOG") != NULL;
    if (RumbleLog)
        fprintf(stderr, "mister: rumble from %s %g\n", what, value);
}

// The player's weapons, by their events (events/<name>.sc of the client
// library); the gauss and the egon are MiSTer_RumbleEvent()'s own
static const struct {
    const char *name;
    unsigned char strong, weak, ms;
} WeaponRumble[] = {
    // Half-Life, and Blue Shift, which has the same
    { "crowbar", 0, 60, 50 },
    { "glock1", 30, 90, 60 },
    { "glock2", 30, 90, 60 },
    { "python", 160, 110, 120 },
    { "mp5", 50, 100, 60 },
    { "mp52", 140, 70, 140 }, // the grenade
    { "shotgun1", 140, 110, 120 },
    { "shotgun2", 190, 140, 170 },
    { "crossbow1", 70, 80, 80 },
    { "crossbow2", 70, 80, 80 },
    { "rpg", 170, 90, 200 },
    { "firehornet", 20, 70, 50 },
    { "snarkfire", 0, 40, 40 },
    // Opposing Force
    { "knife", 0, 50, 40 },
    { "pipewrench", 0, 70, 60 },
    { "eagle", 120, 100, 100 },
    { "m249", 80, 110, 60 },
    { "sniper", 180, 120, 150 },
    { "shock", 30, 90, 60 },
    { "spore", 100, 70, 110 },
    { "displacer", 190, 150, 250 },
    { "penguinfire", 0, 40, 40 },
};
#define RUMBLE_EGON_STRONG 60
#define RUMBLE_EGON_WEAK 110

static long long RumbleNow(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int Clamp255(float v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

// In the place of the pulse that ends first
static void RumblePulse(int strong, int weak, int ms)
{
    struct rumble *p = &Pulses[0];
    int i;

    for (i = 1; i < RUMBLE_PULSES; i++) {
        if (Pulses[i].until < p->until)
            p = &Pulses[i];
    }
    p->strong = strong;
    p->weak = weak;
    p->until = RumbleNow() + ms;
}

void MiSTer_RumbleEvent(const char *name, float fparam1, int iparam1, int bparam2)
{
    size_t len;
    unsigned i;

    if (strncmp(name, "events/", 7))
        return;
    RumbleWhy(name, fparam1);
    name += 7;
    len = strlen(name);
    if (len < 3 || strcmp(name + len - 3, ".sc"))
        return;
    len -= 3;
    if (len == 5 && !strncmp(name, "gauss", 5)) {
        // fparam1 the damage: 20 a primary shot, up to 200 fully charged;
        // bparam2: only the charge's end, no shot
        float f = fparam1 >= 200 ? 1 : fparam1 / 200;

        GaussSpin = 0;
        if (!bparam2 && fparam1 > 0)
            RumblePulse(Clamp255(60 + 140 * f), Clamp255(100 + 60 * f), 80 + (int)(220 * f));
        return;
    }
    if (len == 9 && !strncmp(name, "gaussspin", 9)) {
        // iparam1 the pitch of its sound, 110 to 250 as it charges, a step a
        // frame or two: in steps of 8, the controller is not told every one
        GaussSpin = bparam2 ? 0 : Clamp255(30 + (iparam1 - 110) * 90 / 140) & ~7;
        return;
    }
    if (len == 9 && !strncmp(name, "egon_fire", 9)) {
        Egon = 1;
        return;
    }
    if (len == 9 && !strncmp(name, "egon_stop", 9)) {
        Egon = 0;
        return;
    }
    for (i = 0; i < sizeof(WeaponRumble) / sizeof(WeaponRumble[0]); i++) {
        if (strlen(WeaponRumble[i].name) == len && !strncmp(name, WeaponRumble[i].name, len)) {
            RumblePulse(WeaponRumble[i].strong, WeaponRumble[i].weak, WeaponRumble[i].ms);
            return;
        }
    }
}

void MiSTer_RumbleDamage(int armor, int health)
{
    int total = armor + health;
    float f;

    if (total <= 0)
        return;
    RumbleWhy("damage", total);
    f = total >= 40 ? 1 : total / 40.0f;
    RumblePulse(Clamp255(70 + 110 * f), Clamp255(60 + 60 * f), 120 + (int)(200 * f));
}

void MiSTer_RumbleExplosion(float distance)
{
    float f = 1 - distance / 1200;

    if (f <= 0)
        return;
    RumbleWhy("an explosion at", distance);
    f *= f;
    RumblePulse(Clamp255(190 * f), Clamp255(120 * f), 150 + (int)(200 * f));
}

void MiSTer_RumbleShake(int strong, int weak, int ms)
{
    Shake.strong = strong;
    Shake.weak = weak;
    Shake.until = RumbleNow() + ms;
}

void MiSTer_RumbleGame(int state)
{
    RumbleState = state;
    if (state == MISTER_RUMBLE_NONE) {
        memset(Pulses, 0, sizeof(Pulses));
        memset(&Shake, 0, sizeof(Shake));
        Egon = GaussSpin = 0;
    }
}

static void RumbleUpdate(void)
{
    static int last_strong, last_weak;
    long long now = RumbleNow();
    int level = MH_OSD_GAME_BITS(MH_OSDStatus(), 31, 2); // 0 On, 1 Strong, 2 Off
    int strong = 0, weak = 0;
    int i;

    if (RumbleLog < 0)
        RumbleLog = getenv("MISTER_HYBRID_RUMBLELOG") != NULL;
    if (RumbleState == MISTER_RUMBLE_RUNNING && level < 2) {
        for (i = 0; i < RUMBLE_PULSES; i++) {
            if (Pulses[i].until > now) {
                strong = Pulses[i].strong > strong ? Pulses[i].strong : strong;
                weak = Pulses[i].weak > weak ? Pulses[i].weak : weak;
            }
        }
        if (Shake.until > now) {
            strong = Shake.strong > strong ? Shake.strong : strong;
            weak = Shake.weak > weak ? Shake.weak : weak;
        }
        if (Egon) {
            strong = RUMBLE_EGON_STRONG > strong ? RUMBLE_EGON_STRONG : strong;
            weak = RUMBLE_EGON_WEAK > weak ? RUMBLE_EGON_WEAK : weak;
        }
        weak = GaussSpin > weak ? GaussSpin : weak;
        strong = Clamp255(strong * (level + 1));
        weak = Clamp255(weak * (level + 1));
    }
    // every frame while it rumbles: the library keeps the motors going only
    // while it is called, and passes on at most 40 changes a second
    if (strong || weak || strong != last_strong || weak != last_weak) {
        MH_Rumble(strong, weak);
        if (RumbleLog && (strong != last_strong || weak != last_weak))
            fprintf(stderr, "mister: rumble %d %d\n", strong, weak);
    }
    last_strong = strong;
    last_weak = weak;
}

static uint32_t LastField;
static int FieldValid = 0;

// Development: see mister_port.h
static int LogEnabled = -1;
static int MarkNow;
static struct timespec MarkTime;
static double MarkSum[MISTER_T_COUNT];
static double MarkFrame[MISTER_T_COUNT]; // of the frame that is being made
static double MarkSlowest[MISTER_T_COUNT]; // of the frame that took longest
static double MarkSlowestWork;

#define PROFILE_MAX (1 << 20)
static uint32_t *ProfilePC;
static uint8_t *ProfileWhat;
static volatile unsigned ProfileCount;
static const char *ProfileFile;

static void ProfileSample(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    unsigned n = ProfileCount;

    (void)sig;
    (void)info;
    if (n < PROFILE_MAX) {
#ifdef __arm__
        ProfilePC[n] = uc->uc_mcontext.arm_pc;
#else
        ProfilePC[n] = (uint32_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
        ProfileWhat[n] = MarkNow;
        ProfileCount = n + 1;
    }
}

static void ProfileWrite(void)
{
    FILE *f = fopen(ProfileFile, "wb");
    FILE *maps = fopen("/proc/self/maps", "r");
    unsigned n = ProfileCount;
    char line[512];

    if (!f)
        return;
    // the memory map first, as text, for the libraries of the game
    while (maps && fgets(line, sizeof(line), maps))
        fputs(line, f);
    if (maps)
        fclose(maps);
    fprintf(f, "samples %u\n", n);
    fwrite(ProfilePC, 4, n, f);
    fwrite(ProfileWhat, 1, n, f);
    fclose(f);
}

static void ProfileStart(void)
{
    struct sigaction sa;
    struct sigevent ev;
    struct itimerspec its;
    clockid_t clock;
    timer_t timer;

    ProfileFile = getenv("MISTER_HYBRID_PROFILE");
    if (!ProfileFile)
        return;
    ProfilePC = malloc(PROFILE_MAX * 4);
    ProfileWhat = malloc(PROFILE_MAX);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ProfileSample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = syscall(SYS_gettid);
    if (pthread_getcpuclockid(pthread_self(), &clock) || timer_create(clock, &ev, &timer))
        return;
    its.it_interval.tv_sec = its.it_value.tv_sec = 0;
    its.it_interval.tv_nsec = its.it_value.tv_nsec = 1000000;
    timer_settime(timer, 0, &its, NULL);
    atexit(ProfileWrite);
}

void MiSTer_Mark(int what)
{
    struct timespec now;

    if (LogEnabled <= 0)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    MarkFrame[MarkNow] += (now.tv_sec - MarkTime.tv_sec) * 1e3 + (now.tv_nsec - MarkTime.tv_nsec) * 1e-6;
    MarkTime = now;
    MarkNow = what;
}

// Every 300 fields (5 seconds): the frames shown, how many of them came 1, 2,
// 3 and more fields after the one before, and the milliseconds per frame
static void FpsLog(unsigned fields)
{
    static const char *const names[MISTER_T_COUNT] = { "other", "wait", "input", "server", "client",
                                                       "render", "hud", "blit", "sound" };
    static unsigned frames, total, after[4];
    int i;

    if (LogEnabled < 0) {
        LogEnabled = getenv("MISTER_HYBRID_FPSLOG") != NULL;
        if (LogEnabled) {
            clock_gettime(CLOCK_MONOTONIC, &MarkTime);
            ProfileStart();
        }
    }
    if (!LogEnabled)
        return;
    {
        double work = 0;

        for (i = 0; i < MISTER_T_COUNT; i++) {
            MarkSum[i] += MarkFrame[i];
            if (i != MISTER_T_WAIT)
                work += MarkFrame[i];
        }
        if (work > MarkSlowestWork) {
            MarkSlowestWork = work;
            memcpy(MarkSlowest, MarkFrame, sizeof(MarkSlowest));
        }
        memset(MarkFrame, 0, sizeof(MarkFrame));
    }
    frames++;
    total += fields;
    after[fields >= 4 ? 3 : fields ? fields - 1 : 0]++;
    if (total >= 300) {
        fprintf(stderr, "mister: %u frames in %u fields (%.1f per second), after 1/2/3/more fields %u/%u/%u/%u, ms:",
                frames, total, frames / (total * MISTER_FIELD_TIME), after[0], after[1], after[2], after[3]);
        for (i = 0; i < MISTER_T_COUNT; i++) {
            fprintf(stderr, " %s %.2f", names[i], MarkSum[i] / frames);
            MarkSum[i] = 0;
        }
        fprintf(stderr, "\nmister: slowest frame %.1f ms:", MarkSlowestWork);
        for (i = 0; i < MISTER_T_COUNT; i++) {
            if (i != MISTER_T_WAIT && MarkSlowest[i] >= 0.5)
                fprintf(stderr, " %s %.1f", names[i], MarkSlowest[i]);
        }
        MarkSlowestWork = 0;
        fprintf(stderr, "\n");
        frames = total = 0;
        memset(after, 0, sizeof(after));
    }
}

void MiSTer_ResetFrames(void)
{
    FieldValid = 0;
}

// The menu's 640x480 (VID_MiSTerMode() in client/vid_common.c) takes a frame
// at every field while it is interlaced (host version 18, Danik 2026-10-28:
// every core's menus at 480i), 59.6 a second as at 31kHz; the game's own
// modes are never interlaced. The menu's animations and repeats go by the
// engine's time, which is the fields that passed (Host_Main()).
static int MenuScreen;

void MiSTer_MenuScreen(int on)
{
    MenuScreen = on != 0;
    if (MH_IsOpen())
        MH_SetFieldRate(MenuScreen);
}

unsigned MiSTer_WaitFrame(void)
{
    uint32_t field;
    unsigned fields;
    int lock;

    if (!MH_IsOpen())
        return 0;
    if (!FieldValid) {
        LastField = MH_FieldCounter();
        FieldValid = 1;
    }
    MiSTer_Mark(MISTER_T_WAIT);
    PollTriggers();
    RumbleUpdate();
    HudLines();
    // Lock to 30 FPS is the game's: the menu at a frame per field
    lock = Lock30() && !MenuScreen;
    // interlaced a frame takes two fields, and the core shows it at once; at
    // field rate (the menu, MiSTer_MenuScreen()) one field, as progressive
    if (!MH_Interlaced() || MH_FieldRate()) {
        // A frame drawn from one vblank to the next was shown at the next
        // only if it was ready by then: about 14ms of a field's 16.8 here,
        // and every tenth was not, so the frame before stayed a field longer
        // and this one was never shown. Each frame is drawn a field ahead
        // now (LastField is the field the last one is shown at), and the
        // library hands it to the core in the field before its own: one that
        // takes longer than a field is still in time.
        uint32_t target = LastField + (lock ? 2 : 1);

        MH_WaitField(target - 3); // not before the field two before its own
        field = MH_FieldCounter();
        if ((int32_t)(field + 1 - target) > 0)
            target = field + 1; // too late for it: the next vblank
        if (!MH_IsOpen())
            return 0;
        MH_PresentAtField(target);
        fields = target - LastField;
        LastField = target;
        MiSTer_Mark(MISTER_T_OTHER);
        FpsLog(fields);
        return fields;
    }
    MH_WaitFrame(LastField);
    if (lock)
        MH_WaitField(LastField + 1); // not before the second field after the last frame
    field = MH_FieldCounter();
    fields = field - LastField; // 0: the core is gone
    LastField = field;
    MiSTer_Mark(MISTER_T_OTHER);
    FpsLog(fields);
    return fields;
}

int MiSTer_Paced(void)
{
    return MH_IsOpen() && FieldValid;
}

// The player came through the list: quitting a game goes back to it
static int PickedFromList = 0;

void MiSTer_ShowError(const char *message)
{
    if (MH_Open())
        MH_UI_Message(TITLE, message, MH_UI_WAIT | MH_UI_ERROR);
}

// The games we build libraries for (build.sh), in the order of the list. Each
// is a menu entry (package/games.txt: the same ids and titles), the set name
// Xash3D_<id>. Every game takes what it does not have itself from Half-Life's
// folder, valve, which our package has as well (our libraries and
// extras.pk3): liblist.gam and halflife.wad are the game's own, in the Steam
// version and on the CD. Messages: 4 lines of 71, %s the game's folder.
static const char *const HalfLifeFiles[] = { "valve/liblist.gam", "valve/halflife.wad", NULL };
static const char *const OpposingForceFiles[] = { "valve/liblist.gam", "valve/halflife.wad",
                                                  "gearbox/liblist.gam", NULL };
static const char *const BlueShiftFiles[] = { "valve/liblist.gam", "valve/halflife.wad",
                                              "bshift/liblist.gam", NULL };
static const struct mh_game Games[] = {
    { "HL", "Half-Life", HalfLifeFiles, NULL,
      "Half-Life was not found. Copy its folder valve (Steam or CD), with liblist.gam "
      "and halflife.wad in it, into %s/." },
    { "OF", "HL: Opposing Force", OpposingForceFiles, NULL,
      "Opposing Force was not found. It needs the folders valve (Half-Life, with "
      "liblist.gam and halflife.wad) and gearbox (Opposing Force, with liblist.gam), "
      "from Steam or the CDs, in %s/." },
    { "BS", "HL: Blue Shift", BlueShiftFiles, NULL,
      "Blue Shift was not found. It needs the folders valve (Half-Life, with "
      "liblist.gam and halflife.wad) and bshift (Blue Shift, with liblist.gam), "
      "from Steam or the CD, in %s/." },
};
static const char *const GameDirs[] = { "valve", "gearbox", "bshift" };
#define NUM_GAMES ((int)(sizeof(Games) / sizeof(Games[0])))
#define NO_GAME "No game found. Copy the folder valve of Half-Life (Steam or CD), with " \
                "liblist.gam and halflife.wad in it, into %s/; gearbox " \
                "(Opposing Force) and bshift (Blue Shift) go next to it."

int MiSTer_SelectGame(void)
{
    const char *base = getenv("XASH3D_BASEDIR");
    int pick;

    // a game given on the command line, or no core: as on a PC
    if (getenv("XASH3D_GAME") || !MH_Open())
        return 1;
    // the data check's lines, then the game's logo with "Loading <title>..."
    // until the first frame (MH_GameLogo(), host version 17)
    pick = MH_PickGame(Games, NUM_GAMES, base, NO_GAME, &PickedFromList);
    if (pick < 0)
        return 0;
    setenv("XASH3D_GAME", GameDirs[pick], 1);
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
