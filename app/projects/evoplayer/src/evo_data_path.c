/*
 * Module: evo_data_path - see include/evo_data_path.h.
 */

#include "evo_data_path.h"
#include "evo_jailbreak.h"   /* evo_jailbreak_is_open() - (1) on non-app builds */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef EVO_APP_MODULE
/* libc's mkdir() is not in the clean-room libc.prx surface (imports.txt lists
 * sceKernelMkdir, not the POSIX name). Go straight to the libkernel export. */
int sceKernelMkdir(const char *path, int mode);
#endif

/* Resolved data root. Only cached once it is the durable root; while the app
 * sandbox is still shut resolve_root() keeps returning the fallback WITHOUT
 * caching, so a later self-unjail (or evo_data_path_rebind()) heals it. */
static const char *g_root;

static const char *resolve_root(void)
{
    if (g_root)
        return g_root;

#ifdef EVO_APP_MODULE
    if (!evo_jailbreak_is_open())
        return EVO_DATA_DIR;              /* "/download0/ps5twitch" - transient */
    g_root = "/data/ps5twitch";
#else
    /*
     * #90: a host tool can point the data root at a temp directory, to stage
     * files there with no console.
     *
     * Host builds only - inside the #else, so the app module cannot be talked
     * into writing somewhere else by an environment it does not control.
     */
    const char *over = getenv("EVO_DATA_DIR_OVERRIDE");
    if (over && *over) {
        static char buf[512];
        snprintf(buf, sizeof buf, "%s", over);
        g_root = buf;
        return g_root;
    }
    g_root = EVO_DATA_DIR;                /* "/data/ps5twitch" - always */
#endif
    return g_root;
}

void evo_data_path_rebind(void)
{
    g_root = NULL;
}

const char *evo_data_dir(void)
{
    return resolve_root();
}

const char *evo_data_path(const char *leaf)
{
    static _Thread_local char buf[512];
    const char *root = resolve_root();

    if (!leaf)
        leaf = "";
    while (*leaf == '/')
        leaf++;

    int n = snprintf(buf, sizeof buf, "%s/%s", root, leaf);
    if (n < 0 || (size_t)n >= sizeof buf)
        return root;

    return buf;
}

/* True when a file can be created in `dir`. A probe write rather than
 * access(): the app's libc has no access(), and with no stick /mnt/usb0 is a
 * bare mount point on a read-only root that a stat would still find. */
static int dir_writable(const char *dir)
{
    char probe[256];
    FILE *f;
    if (snprintf(probe, sizeof probe, "%s/.evo_probe", dir) >= (int)sizeof probe)
        return 0;
    f = fopen(probe, "w");
    if (!f)
        return 0;
    fclose(f);
    remove(probe);
    return 1;
}

const char *evo_diag_dir(void)
{
    static const char *dir;
    if (dir)
        return dir;
    if (dir_writable("/mnt/usb0")) {
        dir = "/mnt/usb0";
        return dir;
    }
#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)
    {
        /* Only once the sandbox is open: before that the data root is the
         * transient /download0 fallback, which isn't worth logging to. */
        static char dev[160];
        const char *root = evo_data_dir();
        if (strncmp(root, "/data/", 6) == 0 &&
            snprintf(dev, sizeof dev, "%s/dev", root) < (int)sizeof dev) {
            evo_mkdir(dev);
            if (dir_writable(dev)) {
                dir = dev;
                return dir;
            }
        }
    }
#endif
    return NULL;
}

int evo_mkdir(const char *path)
{
#ifdef EVO_APP_MODULE
    int r = sceKernelMkdir(path, 0777);
    if (r == 0 || r == (int)0x80020011 /* SCE_KERNEL_ERROR_EEXIST */)
        return 0;
    return r;
#else
    if (mkdir(path, 0777) == 0 || errno == EEXIST)
        return 0;
    return -1;
#endif
}
