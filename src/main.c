/**
 * @file main.c
 * @brief GoldHEN plugin entry point for Custom IME
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "plugin_common.h"
#include "ime_hook.h"
#include "overlay.h"

#include <GoldHEN.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Pad.h>

/* ─── Forward Declarations ────────────────────────────────────────── */

static int32_t load_required_modules(void);

/* ─── On-Screen Notification ─────────────────────────────────────── */

void notify(const char *fmt, ...) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type     = NotificationRequest;
    req.reqId    = 0;
    req.priority = 0;
    req.msgId    = 0;
    req.targetId = -1;
    req.userId   = -1;

    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ─── Plugin Entry ────────────────────────────────────────────────── */

int module_start(size_t argc, const void *args) {
    (void)argc;
    (void)args;

    LOG_INFO("=== %s v%d.%d.%d starting ===",
        PLUGIN_NAME,
        (PLUGIN_VER >> 16) & 0xFF,
        (PLUGIN_VER >> 8) & 0xFF,
        PLUGIN_VER & 0xFF);

    int32_t rc = load_required_modules();
    if (rc != IME_OK) {
        LOG_ERROR("Failed to load required modules (rc=%d)", rc);
        notify("Custom IME: module load FAILED (%d)", rc);
        return rc;
    }

    rc = ime_hook_install();
    if (rc != IME_OK) {
        LOG_ERROR("Failed to install IME hooks (rc=%d)", rc);
        notify("Custom IME: hook install FAILED (%d)", rc);
        return rc;
    }

    rc = overlay_init();
    if (rc != IME_OK) {
        LOG_WARN("Overlay init returned %d (non-fatal)", rc);
    }

    /* Load shell overlay into SceShellUI for PUI-based rendering.
     * Try multiple paths — kernel resolves paths outside app sandbox. */
    static const char *sovl_paths[] = {
        "/data/shell_overlay.prx",
        "/data/GoldHEN/plugins/shell_overlay.prx",
        "/user/data/shell_overlay.prx",
        "/user/data/GoldHEN/plugins/shell_overlay.prx",
        NULL
    };
    int sovl_rc = -1;
    for (int i = 0; sovl_paths[i]; i++) {
        LOG_INFO("Trying SOVL load: %s", sovl_paths[i]);
        sovl_rc = sys_sdk_proc_prx_load("SceShellUI",
                                         (char*)sovl_paths[i]);
        LOG_INFO("  -> rc=0x%08X (%d)", sovl_rc, sovl_rc);
        if (sovl_rc >= 0) break;
    }
    LOG_INFO("Plugin loaded successfully - IME hooks + overlay active");
    notify("Custom IME v%d.%d.%d loaded",
        (PLUGIN_VER >> 16) & 0xFF,
        (PLUGIN_VER >> 8) & 0xFF,
        PLUGIN_VER & 0xFF);
    return IME_OK;
}

int module_stop(size_t argc, const void *args) {
    (void)argc;
    (void)args;

    LOG_INFO("Plugin shutting down...");

    overlay_cleanup();

    int32_t rc = ime_hook_remove();
    if (rc != IME_OK) {
        LOG_WARN("Hook removal returned %d", rc);
    }

    LOG_INFO("=== %s stopped ===", PLUGIN_NAME);
    return IME_OK;
}

/* ─── Module Loading ──────────────────────────────────────────────── */

static int32_t load_required_modules(void) {
    int32_t rc;

    /* Load Common Dialog (internal system module) */
    rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_COMMON_DIALOG);
    if (rc < 0 && rc != (int32_t)0x80020133) {
        LOG_ERROR("Failed to load CommonDialog: 0x%08X", rc);
        return IME_ERROR_MODULE_LOAD;
    }
    LOG_DEBUG("CommonDialog: %s",
        rc == (int32_t)0x80020133 ? "already loaded" : "loaded");

    /* Load IME Dialog module */
    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_IME_DIALOG);
    if (rc < 0 && rc != (int32_t)0x80020133) {
        LOG_ERROR("Failed to load ImeDialog: 0x%08X", rc);
        return IME_ERROR_MODULE_LOAD;
    }
    LOG_DEBUG("ImeDialog: %s",
        rc == (int32_t)0x80020133 ? "already loaded" : "loaded");

    /* Load Pad module (likely already loaded by game) */
    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_PAD);
    if (rc < 0 && rc != (int32_t)0x80020133) {
        LOG_WARN("Failed to load Pad module: 0x%08X (non-fatal)", rc);
    } else {
        LOG_DEBUG("Pad: %s",
            rc == (int32_t)0x80020133 ? "already loaded" : "loaded");
    }

    /* Load UserService module (likely already loaded by game) */
    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_USER_SERVICE);
    if (rc < 0 && rc != (int32_t)0x80020133) {
        LOG_WARN("Failed to load UserService: 0x%08X (non-fatal)", rc);
    } else {
        LOG_DEBUG("UserService: %s",
            rc == (int32_t)0x80020133 ? "already loaded" : "loaded");
    }

    /* Load VideoOut module (likely already loaded by game) */
    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEO_OUT);
    if (rc < 0 && rc != (int32_t)0x80020133) {
        LOG_WARN("Failed to load VideoOut: 0x%08X (non-fatal)", rc);
    } else {
        LOG_DEBUG("VideoOut: %s",
            rc == (int32_t)0x80020133 ? "already loaded" : "loaded");
    }

    /* Initialize pad library (safe to call if already initialized) */
    rc = scePadInit();
    if (rc < 0 && rc != (int32_t)0x80920002) {
        LOG_WARN("scePadInit: 0x%08X (non-fatal)", rc);
    } else {
        LOG_DEBUG("scePadInit: %s",
            rc == (int32_t)0x80920002 ? "already initialized" : "OK");
    }

    return IME_OK;
}
