/**
 * @file main.c
 * @brief SceShellUI overlay — Full ThumbGrid via PUI widgets + IPC reader
 *
 * This PRX is injected into the SceShellUI process via GoldHEN's
 * sys_sdk_proc_prx_load. It attaches to the existing Mono runtime,
 * finds the PUI "Game" overlay scene, and creates the full ThumbGrid
 * IME using PUI Panel/Label widgets.
 *
 * Reads game-side state from file-backed shared memory (thumbgrid_ipc.bin)
 * and updates widget properties at ~30Hz.
 *
 * Widget tree:
 *   RootWidget (Game scene)
 *     grid_panel (Panel) — master container
 *       title_label (Label)
 *       text_label (Label)
 *       cell_panels[9] (Panel) — 3x3 cell backgrounds
 *         cell_labels[9] (Label) — character labels
 *       status_label (Label)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* Typedefs required because libmonovm.h uses bare type names */
typedef struct MonoDomain MonoDomain;
typedef struct MonoAssembly MonoAssembly;
typedef struct MonoImage MonoImage;
typedef struct MonoClass MonoClass;
typedef struct MonoMethod MonoMethod;
typedef struct MonoObject MonoObject;
typedef struct MonoString MonoString;
typedef struct MonoThread MonoThread;
typedef struct MonoType MonoType;
typedef struct MonoArray MonoArray;
typedef struct MonoVTable MonoVTable;
typedef struct MonoProperty MonoProperty;
typedef struct MonoClassField MonoClassField;
typedef struct MonoArrayType MonoArrayType;
typedef struct MonoMethodSignature MonoMethodSignature;
typedef struct MonoMethodDesc MonoMethodDesc;
typedef struct MonoGenericParam MonoGenericParam;
typedef struct MonoGenericClass MonoGenericClass;
typedef struct MonoGenericContainer MonoGenericContainer;
typedef struct MonoCustomMod MonoCustomMod;
typedef struct MonoMarshalType MonoMarshalType;
typedef struct MonoClassRuntimeInfo MonoClassRuntimeInfo;
typedef struct MonoClassExt MonoClassExt;
typedef struct MonoThreadsSync MonoThreadsSync;
typedef struct MonoRuntimeGenericContext MonoRuntimeGenericContext;
typedef struct MonoEvent MonoEvent;
typedef struct MonoArrayBounds MonoArrayBounds;
typedef struct MonoMethodNormal MonoMethodNormal;
typedef struct MonoMethodPInvoke MonoMethodPInvoke;
typedef struct MonoMethodInflated MonoMethodInflated;
typedef struct MonoGenericInst MonoGenericInst;
typedef struct MonoGenericContext MonoGenericContext;

#include <orbis/libmonovm.h>
#include <orbis/libkernel.h>

/* Mono API functions missing from OpenOrbis libmonovm.h */
extern MonoProperty *mono_class_get_properties(MonoClass *klass, void **iter);
extern const char   *mono_property_get_name(MonoProperty *prop);

#include "thumbgrid_ipc.h"

/* ─── File-based logging ────────────────────────────────────────── */

#define SOVL_LOG_PATH "/user/data/sovl_log.txt"

static int g_log_fd = -1;

static void sovl_log_open(void) {
    if (g_log_fd >= 0) return;
    g_log_fd = sceKernelOpen(SOVL_LOG_PATH,
                              0x0601,  /* O_WRONLY|O_CREAT|O_TRUNC */
                              0666);
}

static void sovl_log_write(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0 && g_log_fd >= 0) {
        sceKernelWrite(g_log_fd, buf, len);
        sceKernelFsync(g_log_fd);
    }
}

#define LOG(fmt, ...)  sovl_log_write("[SOVL] " fmt "\n", ##__VA_ARGS__)
#define LOGI(fmt, ...) sovl_log_write("[SOVL] OK: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) sovl_log_write("[SOVL] WARN: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) sovl_log_write("[SOVL] ERR: " fmt "\n", ##__VA_ARGS__)

/* ─── PS4 page size for mprotect ──────────────────────────────── */

#define PS4_PAGE_SIZE   0x4000
#define PAGE_ALIGN(addr) ((void*)((uintptr_t)(addr) & ~(PS4_PAGE_SIZE - 1)))

#define PROT_READ    0x01
#define PROT_WRITE   0x02
#define PROT_EXEC    0x04

#ifndef MAP_SHARED
#define MAP_SHARED  0x0001
#endif
#ifndef MAP_FAILED
#define MAP_FAILED  ((void *)-1)
#endif

/* ─── Notification helper ─────────────────────────────────────── */

static void sovl_notify(const char *fmt, ...) {
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

/* ─── PUI namespace candidates ────────────────────────────────── */

static const char *NS_CANDIDATES[] = {
    "Sce.PlayStation.PUI.UI2",
    "Sce.PlayStation.HighLevel.UI2",
    NULL
};

static const char *PUI_ASM_NAMES[] = {
    "Sce.PlayStation.PUI",
    "Sce.PlayStation.PUI.dll",
    "Sce.PlayStation.HighLevel.UI2",
    "Sce.PlayStation.HighLevel.UI2.dll",
    NULL
};

static const char *APP_ASM_NAMES[] = {
    "app",
    "app.exe",
    NULL
};

static const char *LM_NS_CANDIDATES[] = {
    "Sce.Vsh.ShellUI.AppSystem",
    "Sce.PlayStation.PUI",
    "Sce.PlayStation.PUI.UI2",
    "Sce.PlayStation.HighLevel.UI2",
    "",
    NULL
};

/* ─── Layout Constants (1920x1080) ───────────────────────────── */

#define BORDER_W          2.0f    /* White border thickness */
#define PAD_OUTER        18.0f    /* Padding inside border */
#define TITLE_BAR_H      32.0f
#define TITLE_GAP         8.0f    /* Space below title */
#define TEXT_BAR_H       42.0f    /* Taller text field */
#define TEXT_BORDER_W     1.0f    /* Text field border */
#define TEXT_GAP         14.0f    /* Gap between text field and grid */
#define CHAR_WIDTH_EST   18.0f    /* Estimated glyph width for highlight positioning */
#define CELL_W          260.0f    /* Wider cells for character spacing */
#define CELL_H          120.0f    /* Taller cells for character spacing */
#define CELL_GAP         10.0f    /* Wide gaps between cells */
#define STATUS_BAR_H     38.0f    /* Match Done button height */
#define STATUS_GAP       10.0f    /* Gap between grid and status */
#define DONE_W          160.0f    /* Wide enough for "R2 Done" */
#define DONE_H           42.0f    /* Tall enough for text + padding */
#define L2_W            160.0f    /* Wide enough for "L2 Shift" */
#define L3_W            160.0f    /* Wide enough for "L3 á" */

/* Computed sizes */
#define GRID_3x3_W     (CELL_W * 3 + CELL_GAP * 2)
#define GRID_3x3_H     (CELL_H * 3 + CELL_GAP * 2)

/* Inner content width = grid width + horizontal padding */
#define CONTENT_W      (GRID_3x3_W + PAD_OUTER * 2)
/* Total inner height */
#define CONTENT_H      (TITLE_BAR_H + TITLE_GAP + TEXT_BAR_H + TEXT_BORDER_W*2 + \
                        TEXT_GAP + GRID_3x3_H + STATUS_GAP + STATUS_BAR_H + PAD_OUTER*2)

/* Outer panel (with border) */
#define GRID_PANEL_W   (CONTENT_W + BORDER_W * 2)
#define GRID_PANEL_H   (CONTENT_H + BORDER_W * 2)

/* Default position: centered, lower third */
#define DEFAULT_X      ((int)((1920.0f - GRID_PANEL_W) / 2.0f))
#define DEFAULT_Y      ((int)(1080.0f * 2.0f / 3.0f - GRID_PANEL_H / 2.0f))

/* ─── Special character constants (must match thumbgrid.h) ───── */

#define TG_SPECIAL_BKSP    '\x02'
#define TG_SPECIAL_SPACE   '\x03'
#define TG_SPECIAL_ACCENT  '\x04'
#define TG_SPECIAL_SELALL  '\x05'
#define TG_SPECIAL_EXIT    '\x06'
#define TG_SPECIAL_CUT     '\x07'
#define TG_SPECIAL_COPY    '\x08'
#define TG_SPECIAL_PASTE   '\x09'
#define TG_SPECIAL_CAPS    '\x0A'

/* ─── Global state ────────────────────────────────────────────── */

static MonoDomain   *g_domain    = NULL;
static MonoImage    *g_pui_image = NULL;
static MonoImage    *g_app_image = NULL;
static const char   *g_pui_ns   = NULL;

/* PUI class cache */
static MonoClass *g_cls_widget = NULL;
static MonoClass *g_cls_label  = NULL;
static MonoClass *g_cls_panel  = NULL;

/* Widget objects */
static MonoObject *g_border_panel    = NULL;  /* White border behind grid */
static MonoObject *g_grid_panel      = NULL;  /* Dark main container */
static MonoObject *g_title_label     = NULL;
static MonoObject *g_text_border     = NULL;  /* White border around text field */
static MonoObject *g_text_bg         = NULL;  /* Dark text field background */
static MonoObject *g_text_highlight  = NULL;  /* Blue selection highlight behind text */
static MonoObject *g_text_label      = NULL;
static MonoObject *g_status_label    = NULL;
static MonoObject *g_done_panel      = NULL;  /* Cyan "Done" button */
static MonoObject *g_done_label      = NULL;
static MonoObject *g_l3_panel        = NULL;  /* Grey "L3 á" button */
static MonoObject *g_l3_label        = NULL;
static MonoObject *g_l2_panel        = NULL;  /* Grey "L2 ⇧" button */
static MonoObject *g_l2_label        = NULL;
static MonoObject *g_cell_panels[9]        = {NULL};
static MonoObject *g_cell_btn_labels[9][4] = {{NULL}};  /* [cell][button] */

/* GC handles to prevent collection */
#define MAX_GC_HANDLES 64
static uint32_t g_gc_handles[MAX_GC_HANDLES];
static int      g_gc_count = 0;

/* Property setters (cached after discovery) */
static MonoMethod *g_set_text     = NULL;
static MonoMethod *g_set_x        = NULL;
static MonoMethod *g_set_y        = NULL;
static MonoMethod *g_set_width    = NULL;
static MonoMethod *g_set_height   = NULL;
static MonoMethod *g_set_visible  = NULL;
static MonoMethod *g_set_alpha    = NULL;
static MonoMethod *g_get_width   = NULL;  /* Width property getter (for text measuring) */

/* Hidden label for measuring text pixel width via FitWidthToText */
static MonoObject *g_measure_label = NULL;
static float       g_avg_char_w    = CHAR_WIDTH_EST;  /* Self-calibrating avg */
static uint32_t    g_measure_len   = 0;   /* Char count set on measure label last cycle */

/* Background color setter — discovered at runtime */
static MonoMethod *g_set_bg_color     = NULL;
static MonoMethod *g_set_text_color   = NULL;
static MonoMethod *g_set_font_size    = NULL;

/* Property name strings discovered */
static const char *g_prop_bg_color    = NULL;
static const char *g_prop_text_color  = NULL;
static const char *g_prop_font_size   = NULL;

/* UIColor struct — PUI uses RGBA floats (0.0–1.0) */
typedef struct {
    float r, g, b, a;
} PUIColor;

/* IPC state */
static volatile ThumbGridSharedState *g_ipc_map = NULL;
static int                      g_ipc_fd  = -1;
static ThumbGridSharedState           g_cached_state;
static volatile bool            g_running = false;
static volatile bool            g_initialized = false;

/* ─── Helper: try to open assembly by multiple names ──────────── */

static MonoAssembly *try_open_assembly(MonoDomain *domain,
                                        const char **names,
                                        const char *label)
{
    for (int i = 0; names[i]; i++) {
        MonoAssembly *a = mono_domain_assembly_open(domain, names[i]);
        if (a) return a;
    }
    return NULL;
}

static MonoAssembly *try_open_assembly_sandbox(MonoDomain *domain,
                                                const char *label)
{
    const char *sandbox = sceKernelGetFsSandboxRandomWord();
    if (!sandbox || sandbox[0] == '\0') return NULL;

    static const char *path_fmts[] = {
        "/%s/common/lib/Sce.PlayStation.PUI.dll",
        "/%s/common/lib/Sce.PlayStation.HighLevel.UI2.dll",
        "/%s/psm/Application/Sce.PlayStation.PUI.dll",
        "/%s/psm/Application/Sce.PlayStation.HighLevel.UI2.dll",
        NULL
    };

    char path[256];
    for (int i = 0; path_fmts[i]; i++) {
        snprintf(path, sizeof(path), path_fmts[i], sandbox);
        MonoAssembly *a = mono_domain_assembly_open(domain, path);
        if (a) return a;
    }
    return NULL;
}

/* ─── Helper: find a class across namespace candidates ────────── */

static MonoClass *find_class_multi_ns(MonoImage *image,
                                       const char **namespaces,
                                       const char *class_name,
                                       const char **out_ns)
{
    for (int i = 0; namespaces[i]; i++) {
        MonoClass *cls = mono_class_from_name(image, namespaces[i], class_name);
        if (cls) {
            if (out_ns) *out_ns = namespaces[i];
            return cls;
        }
    }
    return NULL;
}

/* ─── Helper: method search ───────────────────────────────────── */

static int count_methods(MonoClass *klass) {
    void *iter = NULL;
    int count = 0;
    while (mono_class_get_methods(klass, &iter) != NULL) count++;
    return count;
}

static void dump_methods_log(MonoClass *klass, const char *tag) {
    char buf[800];
    int pos = 0;
    void *iter = NULL;
    MonoMethod *m;
    int count = 0;
    while ((m = mono_class_get_methods(klass, &iter)) != NULL && count < 40) {
        const char *name = mono_method_get_name(m);
        if (!name) name = "?";
        int need = snprintf(buf + pos, sizeof(buf) - pos,
                           "%s%s", count ? "," : "", name);
        if (pos + need >= (int)sizeof(buf) - 1) break;
        pos += need;
        count++;
    }
    LOG("%s: %s", tag, buf);
}

static MonoMethod *find_method_in_hierarchy(MonoClass *klass,
                                             const char *name, int nargs) {
    MonoClass *cls = klass;
    while (cls) {
        MonoMethod *m = mono_class_get_method_from_name(cls, name, nargs);
        if (m) return m;
        cls = cls->parent;
    }
    return NULL;
}

/* ─── Helper: invoke a static method with MonoString arg ──────── */

static MonoObject *invoke_static_string(MonoMethod *method,
                                         const char *str_arg)
{
    MonoString *ms = mono_string_new(g_domain, str_arg);
    void *args[] = { ms };
    MonoObject *exc = NULL;
    MonoObject *result = mono_runtime_invoke(method, NULL, args, &exc);
    if (exc) {
        LOGW("  exception during invoke (str='%s')", str_arg);
    }
    return result;
}

/* ─── Helper: GC pinning ─────────────────────────────────────── */

static void gc_pin(MonoObject *obj) {
    if (!obj || g_gc_count >= MAX_GC_HANDLES) return;
    g_gc_handles[g_gc_count++] = mono_gchandle_new(obj, 1);
}

/* ─── Helper: property setters ────────────────────────────────── */

static void set_float_prop(MonoClass *cls, MonoObject *obj,
                            const char *name, float val) {
    MonoProperty *prop = mono_class_get_property_from_name(cls, name);
    if (!prop) return;
    MonoMethod *setter = mono_property_get_set_method(prop);
    if (!setter) return;
    void *args[] = { &val };
    mono_runtime_invoke(setter, obj, args, NULL);
}

static void set_bool_prop(MonoClass *cls, MonoObject *obj,
                           const char *name, bool val) {
    MonoProperty *prop = mono_class_get_property_from_name(cls, name);
    if (!prop) return;
    MonoMethod *setter = mono_property_get_set_method(prop);
    if (!setter) return;
    uint32_t bval = val ? 1 : 0;
    void *args[] = { &bval };
    mono_runtime_invoke(setter, obj, args, NULL);
}

static void set_int_prop(MonoClass *cls, MonoObject *obj,
                          const char *name, int32_t val) {
    MonoProperty *prop = mono_class_get_property_from_name(cls, name);
    if (!prop) return;
    MonoMethod *setter = mono_property_get_set_method(prop);
    if (!setter) return;
    void *args[] = { &val };
    mono_runtime_invoke(setter, obj, args, NULL);
}

static void set_text_prop(MonoObject *obj, const char *text) {
    if (!g_set_text || !obj) return;
    MonoString *ms = mono_string_new(g_domain, text);
    void *args[] = { ms };
    mono_runtime_invoke(g_set_text, obj, args, NULL);
}

/* Set label HorizontalAlignment: 0=Left, 1=Center, 2=Right */
static void set_label_halign(MonoObject *label, int32_t align) {
    if (!label || !g_cls_label) return;
    set_int_prop(g_cls_label, label, "HorizontalAlignment", align);
}

/* Set label VerticalAlignment: 0=Top, 1=Center, 2=Bottom */
static void set_label_valign(MonoObject *label, int32_t align) {
    if (!label || !g_cls_label) return;
    set_int_prop(g_cls_label, label, "VerticalAlignment", align);
}

static void set_text_prop_u16(MonoObject *obj, const uint16_t *text,
                               uint32_t len) {
    if (!g_set_text || !obj) return;
    /* Build ASCII string with accent approximation for PUI */
    char buf[512];
    uint32_t n = len;
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = text[i];
        if (ch < 128) {
            buf[i] = (char)ch;
        } else {
            /* Map common accented Latin to base letter */
            switch (ch) {
            case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E3: case 0x00E4: buf[i] = 'a'; break;
            case 0x00C1: case 0x00C0: case 0x00C2: case 0x00C3: case 0x00C4: buf[i] = 'A'; break;
            case 0x00E9: case 0x00E8: case 0x00EA: case 0x00EB: buf[i] = 'e'; break;
            case 0x00C9: case 0x00C8: case 0x00CA: case 0x00CB: buf[i] = 'E'; break;
            case 0x00ED: case 0x00EC: case 0x00EE: case 0x00EF: buf[i] = 'i'; break;
            case 0x00CD: case 0x00CC: case 0x00CE: case 0x00CF: buf[i] = 'I'; break;
            case 0x00F3: case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6: buf[i] = 'o'; break;
            case 0x00D3: case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6: buf[i] = 'O'; break;
            case 0x00FA: case 0x00F9: case 0x00FB: case 0x00FC: buf[i] = 'u'; break;
            case 0x00DA: case 0x00D9: case 0x00DB: case 0x00DC: buf[i] = 'U'; break;
            case 0x00F1: buf[i] = 'n'; break;
            case 0x00D1: buf[i] = 'N'; break;
            default: buf[i] = '?'; break;
            }
        }
    }
    buf[n] = '\0';
    set_text_prop(obj, buf);
}

/* ─── Stage 5: Patch CheckRunningOnMainThread ─────────────────── */

static bool patch_main_thread_check(MonoImage *image, const char *ns) {
    static const char *class_names[] = {
        "UISystem", "Application", "UIContext", NULL
    };
    static const char *method_names[] = {
        "CheckRunningOnMainThread",
        "checkRunningOnMainThread",
        "IsMainThread",
        NULL
    };

    for (int ci = 0; class_names[ci]; ci++) {
        MonoClass *cls = mono_class_from_name(image, ns, class_names[ci]);
        if (!cls) continue;

        for (int mi = 0; method_names[mi]; mi++) {
            MonoMethod *method = mono_class_get_method_from_name(
                cls, method_names[mi], -1);
            if (!method) continue;

            void *native = mono_aot_get_method(g_domain, method);
            if (!native) native = mono_compile_method(method);
            if (!native) continue;

            void *page = PAGE_ALIGN(native);
            sceKernelMprotect(page, PS4_PAGE_SIZE,
                              PROT_READ | PROT_WRITE | PROT_EXEC);
            *(volatile uint8_t *)native = 0xC3;
            sceKernelMprotect(page, PS4_PAGE_SIZE, PROT_READ | PROT_EXEC);
            return true;
        }
    }
    return false;
}

/* ─── Scene finding ───────────────────────────────────────────── */

static MonoObject *find_game_scene(void) {
    MonoClass *lm_cls = NULL;
    MonoImage *images[] = { g_app_image, g_pui_image, NULL };
    const char *found_in = NULL;

    static const char *mgr_names[] = { "LayerManager", "SceneManager", NULL };
    for (int mi = 0; mgr_names[mi] && !lm_cls; mi++) {
        for (int img_i = 0; images[img_i]; img_i++) {
            lm_cls = find_class_multi_ns(images[img_i], LM_NS_CANDIDATES,
                                          mgr_names[mi], NULL);
            if (lm_cls) {
                found_in = mgr_names[mi];
                break;
            }
        }
    }

    if (!lm_cls) {
        LOG("S6: no LayerMgr found");
        return NULL;
    }

    LOG("S6: found %s (%d methods)", found_in, count_methods(lm_cls));

    MonoMethod *find_scene = mono_class_get_method_from_name(
        lm_cls, "FindContainerSceneByPath", 1);
    if (!find_scene)
        find_scene = mono_class_get_method_from_name(lm_cls, "FindScene", 1);
    if (!find_scene)
        find_scene = mono_class_get_method_from_name(lm_cls, "GetScene", 1);
    if (!find_scene) {
        LOG("S6: no Find method on %s", found_in);
        return NULL;
    }

    static const char *scene_names[] = {
        "Game", "game", "Overlay", "overlay",
        "System", "system", "Dialog", "dialog", NULL
    };
    for (int i = 0; scene_names[i]; i++) {
        MonoObject *scene = invoke_static_string(find_scene, scene_names[i]);
        if (scene) {
            LOG("S6: scene '%s' found", scene_names[i]);
            return scene;
        }
    }

    LOG("S6: no scene found");
    return NULL;
}

/* ─── Phase 1: Property Discovery ─────────────────────────────── */

/**
 * Enumerate properties on Widget/Label/Panel to discover how to set
 * background color, text color, font size, etc.
 */
static void discover_properties(void) {
    LOG("=== Property Discovery ===");

    /* Discover Widget properties */
    if (g_cls_widget) {
        LOG("Widget properties:");
        void *iter = NULL;
        MonoProperty *prop;
        while ((prop = mono_class_get_properties(g_cls_widget, &iter)) != NULL) {
            const char *name = mono_property_get_name(prop);
            if (name) LOG("  W.%s", name);
        }
    }

    /* Discover Label properties */
    if (g_cls_label) {
        LOG("Label properties:");
        void *iter = NULL;
        MonoProperty *prop;
        while ((prop = mono_class_get_properties(g_cls_label, &iter)) != NULL) {
            const char *name = mono_property_get_name(prop);
            if (name) LOG("  L.%s", name);
        }
    }

    /* Discover Panel properties */
    if (g_cls_panel) {
        LOG("Panel properties:");
        void *iter = NULL;
        MonoProperty *prop;
        while ((prop = mono_class_get_properties(g_cls_panel, &iter)) != NULL) {
            const char *name = mono_property_get_name(prop);
            if (name) LOG("  P.%s", name);
        }
    }

    /* Cache property setters for common properties */
    /* Try various names for background color */
    static const char *bg_names[] = {
        "BackgroundColor", "Background", "BackColor", "BgColor", NULL
    };
    for (int i = 0; bg_names[i] && !g_prop_bg_color; i++) {
        MonoClass *try_cls = g_cls_panel ? g_cls_panel : g_cls_widget;
        if (!try_cls) continue;
        MonoProperty *p = mono_class_get_property_from_name(try_cls, bg_names[i]);
        if (p) {
            g_prop_bg_color = bg_names[i];
            g_set_bg_color = mono_property_get_set_method(p);
            LOG("Found bg color: %s", bg_names[i]);
        }
    }
    /* Also check Widget hierarchy */
    if (!g_prop_bg_color && g_cls_widget) {
        for (int i = 0; bg_names[i]; i++) {
            MonoProperty *p = mono_class_get_property_from_name(g_cls_widget, bg_names[i]);
            if (p) {
                g_prop_bg_color = bg_names[i];
                g_set_bg_color = mono_property_get_set_method(p);
                LOG("Found bg color on Widget: %s", bg_names[i]);
                break;
            }
        }
    }

    /* Try various names for text color */
    static const char *tc_names[] = {
        "TextColor", "ForegroundColor", "ForeColor", "Color", "FontColor", NULL
    };
    if (g_cls_label) {
        for (int i = 0; tc_names[i] && !g_prop_text_color; i++) {
            MonoProperty *p = mono_class_get_property_from_name(g_cls_label, tc_names[i]);
            if (p) {
                g_prop_text_color = tc_names[i];
                g_set_text_color = mono_property_get_set_method(p);
                LOG("Found text color: %s", tc_names[i]);
            }
        }
    }

    /* Try various names for font size (NOT "Font" — that's a Font object, not a float) */
    static const char *fs_names[] = {
        "FontSize", "TextSize", "Size", NULL
    };
    if (g_cls_label) {
        for (int i = 0; fs_names[i] && !g_prop_font_size; i++) {
            MonoProperty *p = mono_class_get_property_from_name(g_cls_label, fs_names[i]);
            if (p) {
                g_prop_font_size = fs_names[i];
                g_set_font_size = mono_property_get_set_method(p);
                LOG("Found font size: %s", fs_names[i]);
            }
        }
    }

    /* Cache standard setters */
    if (g_cls_label) {
        MonoProperty *p;
        p = mono_class_get_property_from_name(g_cls_label, "Text");
        if (p) g_set_text = mono_property_get_set_method(p);
    }
    /* For position/size, try on Widget first then Label */
    MonoClass *w_cls = g_cls_widget ? g_cls_widget : g_cls_label;
    if (w_cls) {
        MonoProperty *p;
        p = mono_class_get_property_from_name(w_cls, "X");
        if (p) g_set_x = mono_property_get_set_method(p);
        p = mono_class_get_property_from_name(w_cls, "Y");
        if (p) g_set_y = mono_property_get_set_method(p);
        p = mono_class_get_property_from_name(w_cls, "Width");
        if (p) {
            g_set_width = mono_property_get_set_method(p);
            g_get_width = mono_property_get_get_method(p);
        }
        p = mono_class_get_property_from_name(w_cls, "Height");
        if (p) g_set_height = mono_property_get_set_method(p);
        p = mono_class_get_property_from_name(w_cls, "Visible");
        if (p) g_set_visible = mono_property_get_set_method(p);
        else {
            p = mono_class_get_property_from_name(w_cls, "IsVisible");
            if (p) g_set_visible = mono_property_get_set_method(p);
        }
        p = mono_class_get_property_from_name(w_cls, "Alpha");
        if (p) g_set_alpha = mono_property_get_set_method(p);
        else {
            p = mono_class_get_property_from_name(w_cls, "Opacity");
            if (p) g_set_alpha = mono_property_get_set_method(p);
        }
    }

    LOG("Setter cache: text=%p x=%p y=%p w=%p h=%p vis=%p alpha=%p",
        (void*)g_set_text, (void*)g_set_x, (void*)g_set_y,
        (void*)g_set_width, (void*)g_set_height,
        (void*)g_set_visible, (void*)g_set_alpha);
    LOG("Setter cache: bg=%p(%s) tc=%p(%s) fs=%p(%s)",
        (void*)g_set_bg_color, g_prop_bg_color ? g_prop_bg_color : "none",
        (void*)g_set_text_color, g_prop_text_color ? g_prop_text_color : "none",
        (void*)g_set_font_size, g_prop_font_size ? g_prop_font_size : "none");

    /* Try to find UIColor class to confirm it exists */
    static const char *color_names[] = { "UIColor", "Color", NULL };
    static const char *color_ns[] = {
        "Sce.PlayStation.PUI.UI2", "Sce.PlayStation.HighLevel.UI2",
        "Sce.PlayStation.PUI", "", NULL
    };
    for (int ni = 0; color_ns[ni]; ni++) {
        for (int ci = 0; color_names[ci]; ci++) {
            MonoClass *cc = mono_class_from_name(g_pui_image, color_ns[ni], color_names[ci]);
            if (cc) {
                LOG("Found color type: %s.%s", color_ns[ni], color_names[ci]);
            }
        }
    }
}

/* ─── Widget creation helpers ─────────────────────────────────── */

static void set_widget_pos(MonoObject *obj, float x, float y,
                            float w, float h)
{
    if (!obj) return;
    MonoClass *cls = obj->vtable ? obj->vtable->klass : NULL;
    if (!cls) return;

    set_float_prop(cls, obj, "X", x);
    set_float_prop(cls, obj, "Y", y);
    set_float_prop(cls, obj, "Width", w);
    set_float_prop(cls, obj, "Height", h);
}

static float get_widget_width(MonoObject *obj) {
    if (!obj || !g_get_width) return 0.0f;
    MonoObject *result = mono_runtime_invoke(g_get_width, obj, NULL, NULL);
    if (!result) return 0.0f;
    return *(float *)mono_object_unbox(result);
}

static void set_widget_visible(MonoObject *obj, bool visible) {
    if (!obj || !g_set_visible) return;
    uint32_t bval = visible ? 1 : 0;
    void *args[] = { &bval };
    mono_runtime_invoke(g_set_visible, obj, args, NULL);
}

static void set_widget_alpha(MonoObject *obj, float alpha) {
    if (!obj || !g_set_alpha) return;
    void *args[] = { &alpha };
    mono_runtime_invoke(g_set_alpha, obj, args, NULL);
}

/* ─── Color helpers (PUI UIColor = RGBA float struct) ─────────── */

static void set_panel_bg(MonoObject *panel, float r, float g, float b, float a) {
    if (!g_set_bg_color || !panel) return;
    PUIColor color = { r, g, b, a };
    void *args[] = { &color };
    MonoObject *exc = NULL;
    mono_runtime_invoke(g_set_bg_color, panel, args, &exc);
    if (exc) {
        LOGW("BackgroundColor setter exception");
        g_set_bg_color = NULL;  /* disable further attempts */
    }
}

static void set_label_color(MonoObject *label, float r, float g, float b, float a) {
    if (!g_set_text_color || !label) return;
    PUIColor color = { r, g, b, a };
    void *args[] = { &color };
    MonoObject *exc = NULL;
    mono_runtime_invoke(g_set_text_color, label, args, &exc);
    if (exc) {
        LOGW("TextColor setter exception");
        g_set_text_color = NULL;
    }
}

/* PS4 Dark Theme Colors — PUI UIColor RGBA floats 0.0-1.0
 * UIColor alpha = 1.0 (opaque fill).
 * Widget.Alpha on grid_panel controls overall semi-transparency. */
#define COL_GRID_BG_R      0.08f   /* Main panel background — dark charcoal */
#define COL_GRID_BG_G      0.08f
#define COL_GRID_BG_B      0.10f
#define COL_GRID_BG_A      1.00f   /* fully opaque fill */

#define COL_CELL_R          0.16f   /* Unselected cell — dark gray */
#define COL_CELL_G          0.16f
#define COL_CELL_B          0.20f
#define COL_CELL_A          1.00f

#define COL_CELL_SEL_R      0.25f   /* Selected cell — lighter with blue tint */
#define COL_CELL_SEL_G      0.32f
#define COL_CELL_SEL_B      0.40f
#define COL_CELL_SEL_A      1.00f

#define COL_TEXT_R          1.00f   /* White text */
#define COL_TEXT_G          1.00f
#define COL_TEXT_B          1.00f
#define COL_TEXT_A          1.00f

#define COL_DIM_R           0.63f   /* Dimmer text for status */
#define COL_DIM_G           0.63f
#define COL_DIM_B           0.66f
#define COL_DIM_A           1.00f

#define COL_BORDER_R        0.45f   /* White/light border around panel */
#define COL_BORDER_G        0.45f
#define COL_BORDER_B        0.48f
#define COL_BORDER_A        1.00f

#define COL_TEXT_BG_R       0.05f   /* Text field inner background (darker) */
#define COL_TEXT_BG_G       0.05f
#define COL_TEXT_BG_B       0.07f
#define COL_TEXT_BG_A       1.00f

#define COL_DONE_R          0.00f   /* Cyan "Done" button */
#define COL_DONE_G          0.55f
#define COL_DONE_B          0.70f
#define COL_DONE_A          1.00f

#define COL_L2_R            0.35f   /* Grey "L2 Shift" button */
#define COL_L2_G            0.35f
#define COL_L2_B            0.38f
#define COL_L2_A            1.00f

static MonoObject *create_panel(void) {
    if (!g_cls_panel) return NULL;
    MonoObject *obj = mono_object_new(g_domain, g_cls_panel);
    if (!obj) return NULL;
    mono_runtime_object_init(obj);
    gc_pin(obj);
    return obj;
}

static MonoObject *create_label(const char *text) {
    if (!g_cls_label) return NULL;
    MonoObject *obj = mono_object_new(g_domain, g_cls_label);
    if (!obj) return NULL;
    mono_runtime_object_init(obj);
    gc_pin(obj);
    if (text) set_text_prop(obj, text);
    return obj;
}

static bool add_child(MonoObject *parent, MonoObject *child) {
    if (!parent || !child) return false;
    MonoClass *cls = parent->vtable ? parent->vtable->klass : NULL;
    if (!cls) return false;

    static const char *add_names[] = {
        "AppendChild", "InsertChildBelow",
        "AddChildLast", "AddChildFirst", "AddChild", NULL
    };
    MonoMethod *method = NULL;
    for (int i = 0; add_names[i]; i++) {
        method = find_method_in_hierarchy(cls, add_names[i], 1);
        if (method) break;
    }
    if (!method) return false;

    MonoObject *exc = NULL;
    void *args[] = { child };
    mono_runtime_invoke(method, parent, args, &exc);
    return (exc == NULL);
}

/* ─── Build widget tree ───────────────────────────────────────── */

static const char *special_label(char c) {
    switch (c) {
    case TG_SPECIAL_BKSP:   return "Del";
    case TG_SPECIAL_SPACE:  return "Space";
    case TG_SPECIAL_ACCENT: return "\xc2\xb4";  /* ´ U+00B4 acute accent */
    case TG_SPECIAL_SELALL: return "Select";
    case TG_SPECIAL_EXIT:   return "Exit";
    case TG_SPECIAL_CUT:    return "Cut";
    case TG_SPECIAL_COPY:   return "Copy";
    case TG_SPECIAL_PASTE:  return "Paste";
    case TG_SPECIAL_CAPS:   return "CAPS";
    default: return "?";
    }
}

static bool is_special_char(char c) {
    return (c == TG_SPECIAL_BKSP || c == TG_SPECIAL_SPACE ||
            c == TG_SPECIAL_ACCENT || c == TG_SPECIAL_SELALL ||
            c == TG_SPECIAL_EXIT || c == TG_SPECIAL_CUT ||
            c == TG_SPECIAL_COPY || c == TG_SPECIAL_PASTE ||
            c == TG_SPECIAL_CAPS);
}

/**
 * Format a single button label.
 * When accent mode is on, accentable letters show their accented UTF-8 form.
 */
static void format_btn_label(char ch, char *buf, int bufsize, bool accent) {
    if (is_special_char(ch)) {
        const char *sl = special_label(ch);
        snprintf(buf, bufsize, "%s", sl);
    } else if (accent && ch >= 32 && ch < 127) {
        /* Map accentable chars to UTF-8 accented forms */
        const char *acc = NULL;
        switch (ch) {
        case 'a': acc = "\xc3\xa1"; break; /* á */
        case 'e': acc = "\xc3\xa9"; break; /* é */
        case 'i': acc = "\xc3\xad"; break; /* í */
        case 'o': acc = "\xc3\xb3"; break; /* ó */
        case 'u': acc = "\xc3\xba"; break; /* ú */
        case 'n': acc = "\xc3\xb1"; break; /* ñ */
        case 'A': acc = "\xc3\x81"; break; /* Á */
        case 'E': acc = "\xc3\x89"; break; /* É */
        case 'I': acc = "\xc3\x8d"; break; /* Í */
        case 'O': acc = "\xc3\x93"; break; /* Ó */
        case 'U': acc = "\xc3\x9a"; break; /* Ú */
        case 'N': acc = "\xc3\x91"; break; /* Ñ */
        default: break;
        }
        if (acc) {
            snprintf(buf, bufsize, "%s", acc);
        } else {
            buf[0] = ch;
            buf[1] = '\0';
        }
    } else if (ch >= 32 && ch < 127) {
        buf[0] = ch;
        buf[1] = '\0';
    } else {
        buf[0] = '?';
        buf[1] = '\0';
    }
}

static bool build_widget_tree(MonoObject *root) {
    LOG("S7: Building widget tree...");

    float px = (float)DEFAULT_X;
    float py = (float)DEFAULT_Y;

    /* ── 1. White border panel (added to root FIRST = behind grid) ── */
    g_border_panel = create_panel();
    if (g_border_panel) {
        set_widget_pos(g_border_panel, px, py, GRID_PANEL_W, GRID_PANEL_H);
        set_widget_alpha(g_border_panel, 0.88f);
        set_panel_bg(g_border_panel,
                     COL_BORDER_R, COL_BORDER_G, COL_BORDER_B, COL_BORDER_A);
        add_child(root, g_border_panel);
    }

    /* ── 2. Dark main container (on top of border) ── */
    g_grid_panel = create_panel();
    if (!g_grid_panel) {
        LOGW("S7: Panel creation failed, trying Label");
        g_grid_panel = create_label("");
    }
    if (!g_grid_panel) {
        LOGE("S7: Cannot create grid container");
        return false;
    }

    /* Inset by border width */
    set_widget_pos(g_grid_panel,
                   px + BORDER_W, py + BORDER_W,
                   GRID_PANEL_W - BORDER_W * 2,
                   GRID_PANEL_H - BORDER_W * 2);
    set_widget_alpha(g_grid_panel, 0.88f);
    set_panel_bg(g_grid_panel,
                 COL_GRID_BG_R, COL_GRID_BG_G, COL_GRID_BG_B, COL_GRID_BG_A);
    add_child(root, g_grid_panel);
    LOG("S7: grid bg set (bg_setter=%p)", (void*)g_set_bg_color);

    /* All children positioned relative to grid_panel's top-left */
    float cur_y = PAD_OUTER;
    float content_left = PAD_OUTER;
    float inner_w = GRID_PANEL_W - BORDER_W * 2 - PAD_OUTER * 2;

    /* ── 3. Title label ── */
    g_title_label = create_label("ThumbGrid");
    if (g_title_label) {
        set_widget_pos(g_title_label, content_left, cur_y,
                       inner_w, TITLE_BAR_H);
        set_label_color(g_title_label,
                        COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, COL_TEXT_A);
        add_child(g_grid_panel, g_title_label);
    }
    cur_y += TITLE_BAR_H + TITLE_GAP;

    /* ── 4. Text field with border ── */
    /* White border panel */
    g_text_border = create_panel();
    if (g_text_border) {
        set_widget_pos(g_text_border, content_left, cur_y,
                       inner_w, TEXT_BAR_H + TEXT_BORDER_W * 2);
        set_panel_bg(g_text_border,
                     COL_BORDER_R, COL_BORDER_G, COL_BORDER_B, COL_BORDER_A);
        add_child(g_grid_panel, g_text_border);
    }
    /* Dark inner background */
    g_text_bg = create_panel();
    if (g_text_bg) {
        set_widget_pos(g_text_bg,
                       content_left + TEXT_BORDER_W,
                       cur_y + TEXT_BORDER_W,
                       inner_w - TEXT_BORDER_W * 2,
                       TEXT_BAR_H);
        set_panel_bg(g_text_bg,
                     COL_TEXT_BG_R, COL_TEXT_BG_G, COL_TEXT_BG_B, COL_TEXT_BG_A);
        add_child(g_grid_panel, g_text_bg);
    }
    /* Selection highlight (behind text, initially hidden) */
    g_text_highlight = create_panel();
    if (g_text_highlight) {
        set_widget_pos(g_text_highlight,
                       content_left + TEXT_BORDER_W + 6.0f,
                       cur_y + TEXT_BORDER_W + 2.0f,
                       0.0f, TEXT_BAR_H - 4.0f);
        set_panel_bg(g_text_highlight,
                     0.16f, 0.40f, 0.72f, 0.85f);  /* Blue highlight */
        set_widget_visible(g_text_highlight, false);
        add_child(g_grid_panel, g_text_highlight);
    }
    /* Text label on top */
    g_text_label = create_label("");
    if (g_text_label) {
        set_widget_pos(g_text_label,
                       content_left + TEXT_BORDER_W + 6.0f,
                       cur_y + TEXT_BORDER_W + 2.0f,
                       inner_w - TEXT_BORDER_W * 2 - 12.0f,
                       TEXT_BAR_H - 4.0f);
        set_label_color(g_text_label,
                        COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, COL_TEXT_A);
        add_child(g_grid_panel, g_text_label);
    }
    /* Hidden measure label — used to compute text pixel width via FitWidthToText */
    g_measure_label = create_label("");
    if (g_measure_label) {
        set_widget_pos(g_measure_label, -9999.0f, -9999.0f, 0.0f,
                       TEXT_BAR_H - 4.0f);
        set_widget_visible(g_measure_label, false);
        if (g_cls_label)
            set_bool_prop(g_cls_label, g_measure_label,
                          "FitWidthToText", true);
        add_child(g_grid_panel, g_measure_label);
    }
    cur_y += TEXT_BAR_H + TEXT_BORDER_W * 2 + TEXT_GAP;

    /* ── 5. 3x3 cell grid ── */
    float grid_x = content_left + (inner_w - GRID_3x3_W) / 2.0f;

    /* Sub-label diamond positions within a cell.
     * Labels are centered text, so offset from cell edges by ~12px. */
    #define BTN_LBL_W  110.0f
    #define BTN_LBL_H  32.0f
    #define BTN_PAD_X  12.0f   /* Minimum horizontal inset from cell edge */
    #define BTN_PAD_Y  10.0f   /* Minimum vertical inset from cell edge */
    static const float btn_offsets[4][2] = {
        { (CELL_W - BTN_LBL_W) / 2.0f,  BTN_PAD_Y },                         /* Triangle: top */
        { CELL_W - BTN_LBL_W - BTN_PAD_X, (CELL_H - BTN_LBL_H) / 2.0f },    /* Circle: right */
        { (CELL_W - BTN_LBL_W) / 2.0f,  CELL_H - BTN_LBL_H - BTN_PAD_Y },   /* Cross: bottom */
        { BTN_PAD_X,                      (CELL_H - BTN_LBL_H) / 2.0f },      /* Square: left */
    };

    for (int cell = 0; cell < 9; cell++) {
        int row = cell / 3;
        int col = cell % 3;

        float cx = grid_x + (float)col * (CELL_W + CELL_GAP);
        float cy = cur_y + (float)row * (CELL_H + CELL_GAP);

        /* Cell panel (dark background) */
        g_cell_panels[cell] = create_panel();
        if (g_cell_panels[cell]) {
            set_widget_pos(g_cell_panels[cell], cx, cy, CELL_W, CELL_H);
            set_panel_bg(g_cell_panels[cell],
                         COL_CELL_R, COL_CELL_G, COL_CELL_B, COL_CELL_A);
            add_child(g_grid_panel, g_cell_panels[cell]);
        }

        /* 4 button labels in diamond layout */
        for (int btn = 0; btn < 4; btn++) {
            g_cell_btn_labels[cell][btn] = create_label("?");
            if (g_cell_btn_labels[cell][btn]) {
                float lx = cx + btn_offsets[btn][0];
                float ly = cy + btn_offsets[btn][1];
                set_widget_pos(g_cell_btn_labels[cell][btn],
                               lx, ly, BTN_LBL_W, BTN_LBL_H);
                set_label_color(g_cell_btn_labels[cell][btn],
                                COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, COL_TEXT_A);
                set_label_halign(g_cell_btn_labels[cell][btn], 1); /* Center */
                set_label_valign(g_cell_btn_labels[cell][btn], 1); /* Center */
                add_child(g_grid_panel, g_cell_btn_labels[cell][btn]);
            }
        }
    }
    cur_y += GRID_3x3_H + STATUS_GAP;

    /* ── 6. Status bar: [abc] on left, L3/L2/R2 buttons on right ── */
    g_status_label = create_label("[abc]");
    if (g_status_label) {
        set_widget_pos(g_status_label, content_left, cur_y,
                       inner_w - DONE_W - L2_W - L3_W - 24.0f, STATUS_BAR_H);
        set_label_color(g_status_label,
                        COL_DIM_R, COL_DIM_G, COL_DIM_B, COL_DIM_A);
        set_label_valign(g_status_label, 1);  /* Center vertically */
        add_child(g_grid_panel, g_status_label);
    }

    /* ── 7a. "L3 á" grey button ── */
    g_l3_panel = create_panel();
    if (g_l3_panel) {
        float l3_x = content_left + inner_w - DONE_W - L2_W - L3_W - 16.0f;
        float l3_y = cur_y + (STATUS_BAR_H - DONE_H) / 2.0f;
        set_widget_pos(g_l3_panel, l3_x, l3_y, L3_W, DONE_H);
        set_panel_bg(g_l3_panel,
                     COL_L2_R, COL_L2_G, COL_L2_B, COL_L2_A);
        add_child(g_grid_panel, g_l3_panel);
    }
    g_l3_label = create_label("L3 \xC3\xA1");
    if (g_l3_label) {
        float l3_x = content_left + inner_w - DONE_W - L2_W - L3_W - 16.0f;
        float l3_y = cur_y + (STATUS_BAR_H - DONE_H) / 2.0f;
        set_widget_pos(g_l3_label, l3_x, l3_y, L3_W, DONE_H);
        set_label_color(g_l3_label,
                        COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, COL_TEXT_A);
        set_label_halign(g_l3_label, 1);
        set_label_valign(g_l3_label, 1);
        if (g_cls_widget)
            set_bool_prop(g_cls_widget, g_l3_label,
                          "IsFontWeightEnhanced", true);
        add_child(g_grid_panel, g_l3_label);
    }

    /* ── 7b. "L2 ⇧" grey button ── */
    g_l2_panel = create_panel();
    if (g_l2_panel) {
        float l2_x = content_left + inner_w - DONE_W - L2_W - 8.0f;
        float l2_y = cur_y + (STATUS_BAR_H - DONE_H) / 2.0f;
        set_widget_pos(g_l2_panel, l2_x, l2_y, L2_W, DONE_H);
        set_panel_bg(g_l2_panel,
                     COL_L2_R, COL_L2_G, COL_L2_B, COL_L2_A);
        add_child(g_grid_panel, g_l2_panel);
    }
    g_l2_label = create_label("L2 \xE2\x87\xA7");
    if (g_l2_label) {
        float l2_x = content_left + inner_w - DONE_W - L2_W - 8.0f;
        float l2_y = cur_y + (STATUS_BAR_H - DONE_H) / 2.0f;
        set_widget_pos(g_l2_label, l2_x, l2_y, L2_W, DONE_H);
        set_label_color(g_l2_label,
                        COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, COL_TEXT_A);
        set_label_halign(g_l2_label, 1);  /* Center */
        set_label_valign(g_l2_label, 1);  /* Center */
        if (g_cls_widget)
            set_bool_prop(g_cls_widget, g_l2_label,
                          "IsFontWeightEnhanced", true);
        add_child(g_grid_panel, g_l2_label);
    }

    /* ── 8. "R2 Done" cyan button on right ── */
    g_done_panel = create_panel();
    if (g_done_panel) {
        float done_x = content_left + inner_w - DONE_W;
        float done_y = cur_y + (STATUS_BAR_H - DONE_H) / 2.0f;
        set_widget_pos(g_done_panel, done_x, done_y, DONE_W, DONE_H);
        set_panel_bg(g_done_panel,
                     COL_DONE_R, COL_DONE_G, COL_DONE_B, COL_DONE_A);
        add_child(g_grid_panel, g_done_panel);
    }
    g_done_label = create_label("R2  Done");
    if (g_done_label) {
        float done_x = content_left + inner_w - DONE_W;
        float done_y = cur_y + (STATUS_BAR_H - DONE_H) / 2.0f;
        set_widget_pos(g_done_label, done_x, done_y, DONE_W, DONE_H);
        set_label_color(g_done_label,
                        COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, COL_TEXT_A);
        set_label_halign(g_done_label, 1);  /* Center */
        set_label_valign(g_done_label, 1);  /* Center */
        if (g_cls_widget)
            set_bool_prop(g_cls_widget, g_done_label,
                          "IsFontWeightEnhanced", true);
        add_child(g_grid_panel, g_done_label);
    }

    LOG("S7: Widget tree built (gc=%d/%d)", g_gc_count, MAX_GC_HANDLES);

    /* Start hidden */
    set_widget_visible(g_grid_panel, false);
    if (g_border_panel) set_widget_visible(g_border_panel, false);

    return true;
}

/* ─── IPC Reader ──────────────────────────────────────────────── */

static bool ipc_reader_open(void) {
    if (g_ipc_map) return true;

    /* Try multiple paths — /user/data/ first since it's shared across
     * process sandboxes (game and SceShellUI can both access it) */
    static const char *ipc_paths[] = {
        "/user/data/thumbgrid_ipc.bin",
        "/data/thumbgrid_ipc.bin",
        "/tmp/thumbgrid_ipc.bin",
        NULL
    };
    static int log_count = 0;

    for (int i = 0; ipc_paths[i]; i++) {
        g_ipc_fd = sceKernelOpen(ipc_paths[i], 0x0002 /* O_RDWR */, 0);
        if (g_ipc_fd >= 0) {
            LOG("IPC reader: opened %s (fd=%d)", ipc_paths[i], g_ipc_fd);
            goto opened;
        }
    }
    /* Log error codes on first failure */
    if (log_count == 0) {
        for (int i = 0; ipc_paths[i]; i++) {
            int fd = sceKernelOpen(ipc_paths[i], 0x0002, 0);
            LOG("IPC try %s -> fd=%d", ipc_paths[i], fd);
            if (fd >= 0) sceKernelClose(fd);
        }
        log_count = 1;
    }
    return false;

opened:;

    void *addr = NULL;
    int rc = sceKernelMmap(0, TG_IPC_FILE_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, g_ipc_fd, 0, &addr);
    if (rc < 0 || addr == MAP_FAILED || !addr) {
        sceKernelClose(g_ipc_fd);
        g_ipc_fd = -1;
        return false;
    }

    g_ipc_map = (volatile ThumbGridSharedState *)addr;
    memset(&g_cached_state, 0, sizeof(g_cached_state));

    /* Clear stale state from previous sessions.
     * - ime_active=0: prevents grid appearing immediately on game start
     * - sequence=0: ensures even value so reader protocol works.
     *   If a previous game crashed mid-write, sequence could be stuck
     *   at odd, which makes thumbgrid_ipc_read reject ALL reads permanently. */
    ThumbGridSharedState *stale = (ThumbGridSharedState *)addr;
    stale->sequence = 0;
    stale->ime_active = 0;

    LOG("IPC reader: mapped at %p (cleared stale state, seq reset)", addr);
    return true;
}

static void ipc_reader_close(void) {
    if (g_ipc_map) {
        sceKernelMunmap((void *)g_ipc_map, TG_IPC_FILE_SIZE);
        g_ipc_map = NULL;
    }
    if (g_ipc_fd >= 0) {
        sceKernelClose(g_ipc_fd);
        g_ipc_fd = -1;
    }
}

/* ─── Update widgets from IPC state ──────────────────────────── */

static void update_widgets(const ThumbGridSharedState *state) {
    /* Show/hide grid based on ime_active */
    if (state->ime_active && !g_cached_state.ime_active) {
        if (g_border_panel) set_widget_visible(g_border_panel, true);
        set_widget_visible(g_grid_panel, true);
        LOG("Grid shown");
    } else if (!state->ime_active && g_cached_state.ime_active) {
        set_widget_visible(g_grid_panel, false);
        if (g_border_panel) set_widget_visible(g_border_panel, false);
        LOG("Grid hidden");
    }

    if (!state->ime_active) {
        g_cached_state = *state;
        return;
    }

    /* Update position if changed */
    if (state->offset_x != g_cached_state.offset_x ||
        state->offset_y != g_cached_state.offset_y) {
        float px = (float)(DEFAULT_X + state->offset_x);
        float py = (float)(DEFAULT_Y + state->offset_y);
        /* Move border panel (outer) */
        if (g_border_panel) {
            MonoClass *cls = g_border_panel->vtable ? g_border_panel->vtable->klass : NULL;
            if (cls) {
                set_float_prop(cls, g_border_panel, "X", px);
                set_float_prop(cls, g_border_panel, "Y", py);
            }
        }
        /* Move grid panel (inset by border width) */
        if (g_grid_panel) {
            MonoClass *cls = g_grid_panel->vtable ? g_grid_panel->vtable->klass : NULL;
            if (cls) {
                set_float_prop(cls, g_grid_panel, "X", px + BORDER_W);
                set_float_prop(cls, g_grid_panel, "Y", py + BORDER_W);
            }
        }
    }

    /* Update title (UTF-16 → UTF-8 conversion) */
    if (g_title_label &&
        memcmp(state->title, g_cached_state.title,
               TG_IPC_TITLE_MAX * sizeof(uint16_t)) != 0) {
        char tbuf[200];
        int tp = 0;
        for (int i = 0; i < TG_IPC_TITLE_MAX && state->title[i] && tp < 190; i++) {
            uint16_t ch = state->title[i];
            if (ch >= 32 && ch < 128) {
                tbuf[tp++] = (char)ch;
            } else if (ch >= 0x80 && ch <= 0x7FF) {
                tbuf[tp++] = (char)(0xC0 | (ch >> 6));
                tbuf[tp++] = (char)(0x80 | (ch & 0x3F));
            } else if (ch >= 0x800) {
                tbuf[tp++] = (char)(0xE0 | (ch >> 12));
                tbuf[tp++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                tbuf[tp++] = (char)(0x80 | (ch & 0x3F));
            }
        }
        tbuf[tp] = '\0';
        set_text_prop(g_title_label, tbuf);
    }

    /* Self-calibrate avg char width from measure label (reads PREVIOUS cycle's layout).
     * FitWidthToText layout is deferred, so we read first, then set new text. */
    if (g_measure_label && g_measure_len > 0) {
        float mw = get_widget_width(g_measure_label);
        if (mw > 0.0f)
            g_avg_char_w = mw / (float)g_measure_len;
    }

    /* Update text display */
    if (g_text_label &&
        (state->output_length != g_cached_state.output_length ||
         state->text_cursor != g_cached_state.text_cursor ||
         state->selected_all != g_cached_state.selected_all ||
         state->sel_start != g_cached_state.sel_start ||
         state->sel_end != g_cached_state.sel_end ||
         memcmp(state->output, g_cached_state.output,
                state->output_length * sizeof(uint16_t)) != 0)) {

        uint32_t tlen = state->output_length;
        if (tlen > 200) tlen = 200;

        /* Determine selection range */
        uint32_t ss = state->sel_start;
        uint32_t se = state->sel_end;
        bool has_sel = (ss != se) || state->selected_all;
        if (state->selected_all) { ss = 0; se = tlen; }
        if (ss > se) { uint32_t t = ss; ss = se; se = t; }

        /* Build display: UTF-8 text with cursor indicator */
        char buf[700];
        int pos = 0;
        for (uint32_t i = 0; i < tlen && pos < 680; i++) {
            if (i == state->text_cursor && !has_sel)
                buf[pos++] = '|';
            uint16_t ch = state->output[i];
            if (ch >= 32 && ch < 128) {
                buf[pos++] = (char)ch;
            } else if (ch >= 0x80 && ch <= 0x7FF) {
                buf[pos++] = (char)(0xC0 | (ch >> 6));
                buf[pos++] = (char)(0x80 | (ch & 0x3F));
            } else if (ch >= 0x800) {
                buf[pos++] = (char)(0xE0 | (ch >> 12));
                buf[pos++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (ch & 0x3F));
            } else {
                buf[pos++] = '?';
            }
        }
        if (state->text_cursor >= tlen && !has_sel)
            buf[pos++] = '|';
        buf[pos] = '\0';
        set_text_prop(g_text_label, buf);

        /* Build pure text (no cursor) for measure label — next cycle reads width */
        if (g_measure_label && tlen > 0) {
            char mbuf[700];
            int mp = 0;
            for (uint32_t i = 0; i < tlen && mp < 680; i++) {
                uint16_t ch = state->output[i];
                if (ch >= 32 && ch < 128) {
                    mbuf[mp++] = (char)ch;
                } else if (ch >= 0x80 && ch <= 0x7FF) {
                    mbuf[mp++] = (char)(0xC0 | (ch >> 6));
                    mbuf[mp++] = (char)(0x80 | (ch & 0x3F));
                } else if (ch >= 0x800) {
                    mbuf[mp++] = (char)(0xE0 | (ch >> 12));
                    mbuf[mp++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    mbuf[mp++] = (char)(0x80 | (ch & 0x3F));
                } else {
                    mbuf[mp++] = '?';
                }
            }
            mbuf[mp] = '\0';
            set_text_prop(g_measure_label, mbuf);
            g_measure_len = tlen;
        }

        /* Position highlight using calibrated avg char width */
        if (g_text_highlight) {
            if (has_sel && se > ss) {
                float text_x = PAD_OUTER + TEXT_BORDER_W + 6.0f;
                float text_y = PAD_OUTER + TITLE_BAR_H + TITLE_GAP
                             + TEXT_BORDER_W + 2.0f;
                float hx = text_x + (float)ss * g_avg_char_w;
                float hw = (float)(se - ss) * g_avg_char_w;
                set_widget_pos(g_text_highlight,
                               hx, text_y, hw, TEXT_BAR_H - 4.0f);
                set_widget_visible(g_text_highlight, true);
            } else {
                set_widget_visible(g_text_highlight, false);
            }
        }
    }

    /* Update L2 button highlight when shift state changes */
    if (state->shift_active != g_cached_state.shift_active) {
        if (g_l2_panel) {
            if (state->shift_active) {
                set_panel_bg(g_l2_panel,
                             COL_DONE_R, COL_DONE_G, COL_DONE_B, COL_DONE_A);
            } else {
                set_panel_bg(g_l2_panel,
                             COL_L2_R, COL_L2_G, COL_L2_B, COL_L2_A);
            }
        }
    }

    /* Update L3 button highlight when accent mode changes */
    if (state->accent_mode != g_cached_state.accent_mode) {
        if (g_l3_panel) {
            if (state->accent_mode) {
                set_panel_bg(g_l3_panel,
                             COL_DONE_R, COL_DONE_G, COL_DONE_B, COL_DONE_A);
            } else {
                set_panel_bg(g_l3_panel,
                             COL_L2_R, COL_L2_G, COL_L2_B, COL_L2_A);
            }
        }
    }

    /* Update cell button labels if page changed, cell content changed, accent or shift toggled */
    if (state->current_page != g_cached_state.current_page ||
        state->accent_mode != g_cached_state.accent_mode ||
        state->shift_active != g_cached_state.shift_active ||
        memcmp(state->cells, g_cached_state.cells, sizeof(state->cells)) != 0) {
        for (int cell = 0; cell < 9; cell++) {
            for (int btn = 0; btn < 4; btn++) {
                if (g_cell_btn_labels[cell][btn]) {
                    char buf[8];
                    format_btn_label(state->cells[cell][btn], buf, sizeof(buf),
                                     state->accent_mode != 0);
                    set_text_prop(g_cell_btn_labels[cell][btn], buf);
                }
            }
        }
    }

    /* Update cell highlight — selected cell gets cyan bg, others dark gray */
    if (state->selected_cell != g_cached_state.selected_cell) {
        /* De-highlight old cell → dark gray */
        int old_cell = g_cached_state.selected_cell;
        if (old_cell >= 0 && old_cell < 9 && g_cell_panels[old_cell]) {
            set_panel_bg(g_cell_panels[old_cell],
                         COL_CELL_R, COL_CELL_G, COL_CELL_B, COL_CELL_A);
        }
        /* Highlight new cell → cyan/teal (PS4 selection color) */
        int new_cell = state->selected_cell;
        if (new_cell >= 0 && new_cell < 9 && g_cell_panels[new_cell]) {
            set_panel_bg(g_cell_panels[new_cell],
                         COL_CELL_SEL_R, COL_CELL_SEL_G,
                         COL_CELL_SEL_B, COL_CELL_SEL_A);
        }
    }

    /* Update status bar — page name */
    if (state->current_page != g_cached_state.current_page ||
        memcmp(state->page_name, g_cached_state.page_name,
               TG_IPC_PAGE_NAME_MAX) != 0) {
        if (g_status_label) {
            char buf[16];
            snprintf(buf, sizeof(buf), "[%s]", state->page_name);
            set_text_prop(g_status_label, buf);
        }
    }

    g_cached_state = *state;
}

/* ─── Poll thread: reads IPC + updates widgets at ~30Hz ──────── */

/* Stale detection: if game exits with IME open, sequence stops updating.
 * After 2s of no sequence change while ime_active=1, force hide grid. */
#define IPC_STALE_TIMEOUT_US  2000000

static void *poll_thread(void *arg) {
    (void)arg;

    LOG("Poll thread started");

    MonoThread *mt = mono_thread_attach(g_domain);
    if (!mt) {
        LOGE("Poll thread: failed to attach to Mono");
        return NULL;
    }

    uint32_t ipc_retry_count = 0;
    uint32_t poll_count = 0;
    uint32_t read_ok = 0;
    uint32_t read_fail = 0;
    uint32_t last_seq = 0;
    uint64_t last_seq_change_us = 0;

    while (g_running) {
        /* Try to open IPC if not yet mapped */
        if (!g_ipc_map) {
            if (ipc_reader_open()) {
                LOG("IPC reader connected");
                ipc_retry_count = 0;
                last_seq = 0;
                last_seq_change_us = 0;
            } else {
                ipc_retry_count++;
                /* Log only occasionally */
                if (ipc_retry_count == 1 || (ipc_retry_count % 100) == 0) {
                    LOG("IPC waiting... (%u)", ipc_retry_count);
                }
                sceKernelUsleep(100000); /* 100ms between retries */
                continue;
            }
        }

        /* Read IPC state */
        ThumbGridSharedState snap;
        if (thumbgrid_ipc_read(g_ipc_map, &snap)) {
            read_ok++;

            /* Track sequence changes for stale detection */
            uint64_t now_us = sceKernelGetProcessTime();
            if (snap.sequence != last_seq) {
                last_seq = snap.sequence;
                last_seq_change_us = now_us;
            }

            /* Detect stale: game exited with IME open */
            if (snap.ime_active && last_seq_change_us > 0 &&
                now_us - last_seq_change_us > IPC_STALE_TIMEOUT_US) {
                LOG("Stale IPC detected (seq=%u unchanged for >2s), forcing hide",
                    snap.sequence);
                snap.ime_active = 0;
                /* Reset IPC file so next game starts clean */
                ThumbGridSharedState *m = (ThumbGridSharedState *)g_ipc_map;
                m->ime_active = 0;
                m->sequence = 0;
                last_seq = 0;
                last_seq_change_us = 0;
            }

            update_widgets(&snap);
        } else {
            read_fail++;
        }

        poll_count++;
        /* Diagnostic log every ~5s (150 iterations at 30Hz) */
        if ((poll_count % 150) == 0) {
            LOG("Poll: %u ok=%u fail=%u seq=%u active=%u",
                poll_count, read_ok, read_fail,
                g_ipc_map->sequence, g_ipc_map->ime_active);
        }

        sceKernelUsleep(33000); /* ~30Hz */
    }

    ipc_reader_close();
    LOG("Poll thread exiting");
    return NULL;
}

/* ─── Main initialization (runs on worker thread) ────────────── */

static int shell_overlay_init(void) {
    /* S2: Mono attach */
    g_domain = mono_get_root_domain();
    if (!g_domain) { LOG("S2: FAIL no domain"); return -1; }

    MonoThread *mt = mono_thread_attach(g_domain);
    if (!mt) { LOG("S2: FAIL thread attach"); return -2; }
    LOG("S2: Mono attached domain=%p", (void*)g_domain);

    /* S3: Find PUI assembly */
    MonoAssembly *pui_asm = try_open_assembly(g_domain, PUI_ASM_NAMES, "PUI");
    if (!pui_asm) pui_asm = try_open_assembly_sandbox(g_domain, "PUI");
    if (!pui_asm) { LOG("S3: FAIL no PUI asm"); return -3; }

    g_pui_image = mono_assembly_get_image(pui_asm);
    if (!g_pui_image) { LOG("S3: FAIL no PUI image"); return -4; }

    MonoAssembly *app_asm = try_open_assembly(g_domain, APP_ASM_NAMES, "app");
    g_app_image = app_asm ? mono_assembly_get_image(app_asm) : NULL;
    LOG("S3: PUI=%p app=%p", (void*)g_pui_image, (void*)g_app_image);

    /* S4: Find classes and determine namespace */
    g_cls_label = find_class_multi_ns(
        g_pui_image, NS_CANDIDATES, "Label", &g_pui_ns);
    if (!g_cls_label) { LOG("S4: FAIL no Label class"); return -5; }

    g_cls_widget = mono_class_from_name(g_pui_image, g_pui_ns, "Widget");
    g_cls_panel  = mono_class_from_name(g_pui_image, g_pui_ns, "Panel");
    LOG("S4: ns=%s W=%p L=%p P=%p", g_pui_ns,
        (void*)g_cls_widget, (void*)g_cls_label, (void*)g_cls_panel);

    /* S5: Patch main thread check */
    bool patched = patch_main_thread_check(g_pui_image, g_pui_ns);
    if (!patched && g_app_image) {
        for (int i = 0; NS_CANDIDATES[i] && !patched; i++)
            patched = patch_main_thread_check(g_app_image, NS_CANDIDATES[i]);
    }
    LOG("S5: thread check patch=%s", patched ? "YES" : "NO");

    /* Phase 1: Property Discovery */
    discover_properties();

    /* S6: Find Game scene */
    MonoObject *scene = find_game_scene();
    if (!scene) {
        LOG("S6: no scene found");
        return -6;
    }

    /* Get RootWidget from scene */
    MonoClass *scene_cls = scene->vtable ? scene->vtable->klass : NULL;
    if (!scene_cls) { LOG("S7: no scene class"); return -7; }

    LOG("S7: scene=%s.%s", scene_cls->name_space, scene_cls->name);

    MonoProperty *root_prop = mono_class_get_property_from_name(
        scene_cls, "RootWidget");
    if (!root_prop) { LOG("S7: no RootWidget prop"); return -8; }

    MonoMethod *get_root = mono_property_get_get_method(root_prop);
    if (!get_root) { LOG("S7: no RootWidget getter"); return -9; }

    MonoObject *root = mono_runtime_invoke(get_root, scene, NULL, NULL);
    if (!root) { LOG("S7: RootWidget is NULL"); return -10; }

    MonoClass *root_cls = root->vtable->klass;
    LOG("S7: root=%s.%s (%d methods)",
        root_cls->name_space, root_cls->name, count_methods(root_cls));

    /* Dump available methods for debugging */
    dump_methods_log(root_cls, "S7 root methods");
    if (root_cls->parent) {
        dump_methods_log(root_cls->parent, "S7 parent methods");
    }

    /* S7: Build full widget tree */
    if (!build_widget_tree(root)) {
        LOGE("Widget tree construction failed");
        return -11;
    }

    g_initialized = true;

    /* Start polling thread for IPC */
    g_running = true;
    OrbisPthread pt;
    int rc = scePthreadCreate(&pt, NULL, poll_thread, NULL, "sovl_poll");
    if (rc != 0) {
        LOGE("Failed to create poll thread: 0x%08X", rc);
        g_running = false;
    } else {
        LOG("Poll thread spawned");
    }

    return 0;
}

/* ─── Worker thread entry ─────────────────────────────────────── */

static void *init_thread(void *arg) {
    (void)arg;
    sceKernelUsleep(1000000);  /* 1s settle delay */
    int ret = shell_overlay_init();
    LOG("Init done rc=%d", ret);
    return NULL;
}

/* ─── PRX entry / exit ────────────────────────────────────────── */

int module_start(size_t argc, const void *args) {
    (void)argc;
    (void)args;

    sovl_log_open();
    LOG("PRX loaded into SceShellUI (fd=%d)", g_log_fd);

    OrbisPthread thread;
    int ret = scePthreadCreate(&thread, NULL, init_thread, NULL, "sovl_init");
    if (ret != 0) {
        LOGE("Failed to create init thread: 0x%08X", ret);
        sceKernelUsleep(500000);
        shell_overlay_init();
    } else {
        LOG("Init thread spawned");
    }

    return 0;
}

int module_stop(size_t argc, const void *args) {
    (void)argc;
    (void)args;

    LOG("=== Shell Overlay PRX unloading ===");

    /* Stop poll thread */
    g_running = false;
    sceKernelUsleep(100000); /* give poll thread time to exit */

    /* Hide widgets before releasing (prevents "after-image") */
    if (g_set_visible && g_domain) {
        MonoThread *mt = mono_thread_attach(g_domain);
        if (mt) {
            if (g_grid_panel)   set_widget_visible(g_grid_panel, false);
            if (g_border_panel) set_widget_visible(g_border_panel, false);
        }
    }

    /* Release GC handles */
    for (int i = 0; i < g_gc_count; i++) {
        if (g_gc_handles[i]) {
            mono_gchandle_free(g_gc_handles[i]);
        }
    }
    g_gc_count = 0;

    /* Close IPC */
    ipc_reader_close();

    g_initialized = false;
    LOG("Cleanup complete");
    if (g_log_fd >= 0) { sceKernelClose(g_log_fd); g_log_fd = -1; }
    return 0;
}
