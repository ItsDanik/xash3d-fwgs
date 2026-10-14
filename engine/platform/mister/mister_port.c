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
    lock = Lock30();
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
