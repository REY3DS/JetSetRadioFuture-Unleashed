/*
 * jsrf_switch.c -- the engine on a Nintendo Switch.
 *
 * libnx owns startup and main() lives in main.c, which comes here with
 *   argv[1]  the game folder (default.xbe and Media/), on the SD card
 *
 * What the Linux launcher, the iPhone app and port/android/jsrf_android.c do
 * before the engine starts is done here: the environment (before the
 * runtime's constructors run), the log, and the folders. Then host_main runs
 * on a thread with a 16 MB stack -- recompiled functions nest as deeply as
 * the guest's calls, and libnx's main thread stack is a megabyte.
 *
 * The SD card layout is
 *
 *   sdmc:/switch/JSRFUnleashed/default.xbe
 *   sdmc:/switch/JSRFUnleashed/Media/...
 *   sdmc:/switch/JSRFUnleashed/save/        (the engine's cache and images)
 *   sdmc:/switch/JSRFUnleashed/settings     (key=value, as on every host)
 *   sdmc:/switch/JSRFUnleashed/Logs/        (last-run.log, previous-run.log)
 *
 * The game's own saves stay in the game folder under UDATA, as on hardware.
 *
 * Output goes to nxlink when this .nro was launched by it (nxlink -s), and to
 * Logs/last-run.log otherwise: a console with no host attached still leaves
 * the lines on the card.
 */
#include <switch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef JSRF_VERSION
#define JSRF_VERSION "dev"
#endif

/* Where the game lives on the card when argv says nothing. */
#define SWITCH_ROOT_DEFAULT "sdmc:/switch/JSRFUnleashed"

static void mkdir_p(const char *path)
{
    char buf[1024];
    char *p;
    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    mkdir(buf, 0755);
}

static int mkdir_ok(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path, 0755) == 0;
}

/* key=value lines (the file the engine's F9/F10 keys write to as well). */
static int cfg_get_int(const char *file, const char *key, int def)
{
    char line[512];
    size_t kl = strlen(key);
    int v = def;
    FILE *f = fopen(file, "r");
    if (!f) return def;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
    fclose(f);
    return v;
}

/* The environment the Linux launcher and the Android app set; anything the
 * player set in the launcher (or on the command line) wins. A constructor so
 * that it is in place before the runtime's own constructors read it. */
static const char *const k_env[][2] = {
    { "RECOMP_USB", "1" }, { "RECOMP_GL", "1" }, { "RECOMP_PB_EXEC", "1" },
    { "RECOMP_VBLANK", "1" }, { "RECOMP_AC97_READY_NOTRAP", "1" },
    { "RECOMP_TRACE_BUDGET", "0" }, { "RECOMP_GL_READBACK", "0" }, { "RECOMP_FPS", "60" },
    { "RECOMP_ABI_RESTORE", "1" }, { "RECOMP_LOCK_IDLE", "1" },
    { "RECOMP_GL_CURTAIN", "skip" }, { "RECOMP_MUTE", "0" }, { "RECOMP_WINDOW", "1" },
    { "RECOMP_FULLSCREEN", "1" },
};

__attribute__((constructor(101))) static void switch_env(void)
{
    size_t i;
    for (i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        setenv(k_env[i][0], k_env[i][1], 0);
}

struct host_call { int (*host)(void); int rc; };

static void *host_thread(void *p)
{
    struct host_call *c = (struct host_call *)p;
    c->rc = c->host();
    return NULL;
}

int nv_switch_main(int argc, char **argv, int (*host)(void));
int nv_switch_main(int argc, char **argv, int (*host)(void))
{
    const char *root = argc > 1 && argv[1] && argv[1][0] ? argv[1]
                     : getenv("JSRF_SWITCH_ROOT");
    char save[1024], cfg[1024], cache[1024], logs[1024], last[1100], prev[1100];
    struct host_call call;
    pthread_attr_t a;
    pthread_t t;
    int nx = -1;

    if (!root || !root[0]) root = SWITCH_ROOT_DEFAULT;

    mkdir_p(root);
    snprintf(save,  sizeof save,  "%s/save",  root);
    snprintf(cache, sizeof cache, "%s/cache", root);
    snprintf(cfg,   sizeof cfg,   "%s/settings", root);
    mkdir_p(save);
    mkdir_p(cache);
    mkdir_p("sdmc:/switch");

    /* The log: nxlink when it is there, the card otherwise. */
    if (socketInitializeDefault() == 0)
        nx = nxlinkStdio();
    if (nx < 0) {
        snprintf(logs, sizeof logs, "%s/Logs", root);
        mkdir_p(logs);
        snprintf(last, sizeof last, "%s/last-run.log", logs);
        snprintf(prev, sizeof prev, "%s/previous-run.log", logs);
        rename(last, prev);
        if (freopen(last, "w", stdout) != NULL) {
            if (!freopen(last, "a", stderr))
                fprintf(stderr, "[SWITCH] cannot redirect stderr: %s\n", strerror(errno));
        } else {
            fprintf(stderr, "[SWITCH] cannot open %s: %s\n", last, strerror(errno));
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    {
        int s = cfg_get_int(cfg, "scale", 2);
        char scale[8];
        snprintf(scale, sizeof scale, "%d", s >= 1 && s <= 4 ? s : 2);
        setenv("RECOMP_GL_SCALE", scale, 0);
    }
    setenv("JSRF_GAME_DIR", root, 1);
    setenv("JSRF_SAVE_DIR", save, 1);
    setenv("JSRF_SETTINGS_FILE", cfg, 1);
    setenv("TMPDIR", cache, 1);
    if (chdir(root) != 0)
        fprintf(stderr, "[SWITCH] cannot enter %s: %s\n", root, strerror(errno));
    fprintf(stderr, "[SWITCH] JSRF Unleashed %s\n[SWITCH] game: %s\n[SWITCH] log: %s\n",
            JSRF_VERSION, root, nx >= 0 ? "nxlink" : last);

    call.host = host;
    call.rc = 1;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&t, &a, host_thread, &call) != 0) {
        fprintf(stderr, "[SWITCH] no thread for the engine\n");
        pthread_attr_destroy(&a);
        return 1;
    }
    pthread_attr_destroy(&a);
    pthread_join(t, NULL);
    fprintf(stderr, "[SWITCH] the engine returned %d\n", call.rc);
    return call.rc;
}
