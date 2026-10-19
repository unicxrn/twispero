/*
 * evo_sysui.c - see evo_sysui.h.
 */
#include "evo_sysui.h"
#include "evo_boot_log.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

int sceKernelOpenEventFlag(void *flag, const char *name);
int sceKernelPollEventFlag(void *flag, uint64_t pattern, uint32_t mode, uint64_t *result);
int sceKernelGetAppInfo(int pid, void *info);

#define FOCUS_FLAG_NAME    "SceShellCoreUtilAppFocus"
#define EVF_WAITMODE_OR    2u
#define SYSTEM_APP_SHELLUI 0x7u   /* what the focus reads while the system has it */

static int s_probed;
static void *s_focus_flag;          /* the kernel event flag's handle */
static int s_focus_opened;
static int s_focus_open_rc = 1;     /* the last open's result, for the log */
static int s_open_tries;
static uint32_t s_own_app_id;       /* 0 while unknown */
static uint64_t s_last_focus = ~0ull;
static int s_last_poll_rc = 1;

static void open_focus_flag(void)
{
    void *flag = NULL;
    const int rc = sceKernelOpenEventFlag(&flag, FOCUS_FLAG_NAME);
    if (rc != s_focus_open_rc)
        evo_boot_log("sysui: open %s rc=%#x", FOCUS_FLAG_NAME, (unsigned)rc);
    s_focus_open_rc = rc;
    if (rc == 0) {
        s_focus_flag = flag;
        s_focus_opened = 1;
    }
}

void evo_sysui_probe(void)
{
    if (s_probed)
        return;
    s_probed = 1;

    /* uint32 app id, uint64, then the title id: the layout ps5-payload-sdk
     * declares (app_info_t). */
    unsigned char info[256];
    memset(info, 0, sizeof info);
    const int pid = (int)getpid();
    const int rc = sceKernelGetAppInfo(pid, info);
    if (rc == 0)
        memcpy(&s_own_app_id, info, sizeof s_own_app_id);
    evo_boot_log("sysui: pid=%d app info rc=%#x app_id=%#x", pid, (unsigned)rc, (unsigned)s_own_app_id);

    open_focus_flag();
}

int evo_sysui_focus_lost(void)
{
    if (!s_focus_opened) {
        /* Now and then: the open may only work once the sandbox is lifted. */
        if ((s_open_tries++ % 120) == 0)
            open_focus_flag();
        if (!s_focus_opened)
            return -1;
    }
    uint64_t value = 0;
    const int rc = sceKernelPollEventFlag(s_focus_flag, ~0ull, EVF_WAITMODE_OR, &value);
    if (rc != s_last_poll_rc || (rc == 0 && value != s_last_focus)) {
        evo_boot_log("sysui: focus rc=%#x value=%#llx (own app id %#x)", (unsigned)rc,
                     (unsigned long long)value, (unsigned)s_own_app_id);
        s_last_poll_rc = rc;
        if (rc == 0)
            s_last_focus = value;
    }
    if (rc != 0)
        return -1;
    const uint32_t focused = (uint32_t)value;
    if (s_own_app_id)
        return focused != s_own_app_id ? 1 : 0;
    /* Own id unknown: only the system's own screen counts as away. */
    return focused == SYSTEM_APP_SHELLUI ? 1 : 0;
}
