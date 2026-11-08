// Online, peripheral and platform services of the BDSP runtime: error stubs. As planned in docs/architecture.md
// (runtime section): network/online (nifm, bsd, ssl, friend, ldn, bcat, nim, …) and everything the Mac run does not need reports
// an error, the rest (prepo, ldr:ro) is answered with "success, no data". Own code.
#include "bdsp.h"
#ifdef _WIN32
#endif

void ipc_note(const char *svc, uint32_t cmd, int unhandled);

// Network unavailable: every request to the service fails (all commands, all sub-objects)
static void svc_error(Service *s, IpcReq *req, IpcResp *resp) {
    ipc_note(s->name, req->cmd, 0);
    resp->result = MAKE_RESULT(110, 1);  // like nn::nifm "network unavailable" (placeholder; the SDK callers only check for failure)
}
static void svc_ok(Service *s, IpcReq *req, IpcResp *resp) {
    ipc_note(s->name, req->cmd, 0);
    resp_size(resp, 0x10);
}
// csrng: GetRandomBytes(B buffer)
static void csrng(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) {
        IpcBuf *b = out_buf(req, 0);
        if (b) {
            if (b->size > SIZE_MAX || !plat_memory_prepare_write(G2H8(b->addr), (size_t)b->size) ||
                !plat_random(G2H8(b->addr), (size_t)b->size)) {
                LOG("[csrng] system randomness failed\n");
                resp->result = MAKE_RESULT(1, 1);
            }
        }
    } else resp_size(resp, 0x10);
}

// bsd:u (nn::socket): registration succeeds (otherwise nn::socket::Initialize aborts), every socket call fails with ENETDOWN
static void bsd_service(Service *s, IpcReq *req, IpcResp *resp) {
    ipc_note(s->name, req->cmd, 0);
    switch (req->cmd) {
    case 0: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // RegisterClient
    case 1: break;                                             // StartMonitoring
    default: OUT(int32_t, 0) = -1; OUT(int32_t, 4) = 100; resp_size(resp, 8);  // return -1, errno ENETDOWN
    }
}

// bcat:u: session and sub-objects answer "success without data" (registering with the service must not fail, otherwise nn::bcat::Initialize aborts)
static void bcat_obj(Service *s, IpcReq *req, IpcResp *resp) {
    ipc_note(s->name, req->cmd, 0);
    resp_size(resp, 0x10);
}
static void bcat_creator(Service *s, IpcReq *req, IpcResp *resp) {
    ipc_note(s->name, req->cmd, 0);
    ipc_return_object(resp, service_new("IBcatService", bcat_obj, NULL));
}

// pl:u (nn::pl shared fonts). The game loads a system font by name (Switch_Font_STD, ..._NIN_Extended) when a text needs it and aborts
// on any error (svcBreak with nn::Result 0x26e, seen after about two hours of play). Fonts: the user's firmware
// files under BDSP_FIRMWARE_DATA/fonts when imported, otherwise a TrueType font of the host system for every type. The SDK reads the font
// at shared memory base + GetSharedMemoryAddressOffset with GetSize bytes and checks nothing else; the layout follows the console:
// per font an 8 byte header (magic 0x18029A7F, size XOR 0x49621806 byte-swapped), then the plain TrueType data.
#define PL_SHMEM_SIZE 0x1100000u
#define PL_FONT_TYPES 6
static const char *const pl_font_file[PL_FONT_TYPES][2] = {
    {"nintendo_udsg-r_std_003.bfttf", "0100000000000811"},        // 0 Standard
    {"nintendo_udsg-r_org_zh-cn_003.bfttf", "0100000000000814"},  // 1 Chinese Simplified
    {"nintendo_udsg-r_ext_zh-cn_003.bfttf", "0100000000000814"},  // 2 Chinese Simplified extension
    {"nintendo_udjxh-db_zh-tw_003.bfttf", "0100000000000813"},    // 3 Chinese Traditional
    {"nintendo_udsg-r_ko_003.bfttf", "0100000000000812"},         // 4 Korean
    {"nintendo_ext_003.bfttf", "0100000000000810"},               // 5 Nintendo extension (button glyphs)
};
static const char *const pl_host_fonts[] = {
#if defined(_WIN32)
    "C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\segoeui.ttf",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf", "/System/Library/Fonts/Core/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Verdana.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
};
static pthread_once_t g_pl_once = PTHREAD_ONCE_INIT;
static uint8_t *g_pl_image;                  // host copy of the shared memory contents
static uint32_t g_pl_off[PL_FONT_TYPES], g_pl_size[PL_FONT_TYPES], g_pl_used;
static uint32_t g_pl_handle;

static uint8_t *pl_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 16 && n < (long)PL_SHMEM_SIZE && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n))) {
            if (fread(data, 1, (size_t)n, f) == (size_t)n) *len = (size_t)n;
            else { free(data); data = NULL; }
        }
    }
    fclose(f);
    return data;
}
// .bfttf: 8 byte header, then every 32-bit word XORed with a key (first word ^ 0x18029A7F); turned into plain TrueType in place
static uint8_t *pl_read_bfttf(const char *root, int type, size_t *len) {
    char path[1200];
    uint8_t *raw = NULL;
    snprintf(path, sizeof path, "%s/fonts/%s", root, pl_font_file[type][0]);
    if (!(raw = pl_read_file(path, len))) {
        snprintf(path, sizeof path, "%s/fonts/%s/%s", root, pl_font_file[type][1], pl_font_file[type][0]);
        raw = pl_read_file(path, len);
    }
    if (!raw) return NULL;
    uint32_t key, w;
    memcpy(&key, raw, 4); key ^= 0x18029A7Fu;
    size_t words = *len / 4 - 2;
    for (size_t i = 0; i < words; i++) { memcpy(&w, raw + 8 + 4 * i, 4); w ^= key; memcpy(raw + 4 * i, &w, 4); }
    *len = words * 4;
    return raw;
}
static int pl_place(const uint8_t *data, size_t len, int type) {
    if (g_pl_used + 8 + len > PL_SHMEM_SIZE) return 0;
    uint32_t magic = 0x18029A7Fu, size = __builtin_bswap32((uint32_t)len ^ 0x49621806u);
    memcpy(g_pl_image + g_pl_used, &magic, 4);
    memcpy(g_pl_image + g_pl_used + 4, &size, 4);
    memcpy(g_pl_image + g_pl_used + 8, data, len);
    g_pl_off[type] = g_pl_used + 8; g_pl_size[type] = (uint32_t)len;
    g_pl_used = (g_pl_used + 8 + (uint32_t)len + 0xF) & ~0xFu;
    return 1;
}
static void pl_load_fonts(void) {
    if (!(g_pl_image = calloc(1, PL_SHMEM_SIZE))) return;
    const char *root = getenv("BDSP_FIRMWARE_DATA");
    for (int t = 0; root && *root && t < PL_FONT_TYPES; t++) {
        size_t len = 0;
        uint8_t *font = pl_read_bfttf(root, t, &len);
        if (font && pl_place(font, len, t)) LOG("[pl] font %d from the firmware (%zu bytes)\n", t, len);
        free(font);
    }
    int fallback = g_pl_size[0] ? 0 : -1;   // missing types share one font: the firmware's standard font, otherwise one of the host
    for (size_t i = 0; fallback < 0 && i < sizeof pl_host_fonts / sizeof *pl_host_fonts; i++) {
        size_t len = 0;
        uint8_t *font = pl_read_file(pl_host_fonts[i], &len);
        if (font && pl_place(font, len, 0)) { fallback = 0; LOG("[pl] font 0 from %s (%zu bytes)\n", pl_host_fonts[i], len); }
        free(font);
    }
    if (fallback < 0) { LOG("[pl] no font found, shared fonts unavailable\n"); return; }
    for (int t = 1; t < PL_FONT_TYPES; t++)
        if (!g_pl_size[t]) { g_pl_off[t] = g_pl_off[fallback]; g_pl_size[t] = g_pl_size[fallback]; }
}
static int pl_ready(void) { pthread_once(&g_pl_once, pl_load_fonts); return g_pl_size[0] != 0; }
static void pl_shmem_mapped(KSharedMem *s) {
    if (pl_ready()) memcpy(G2H8(s->guest_addr), g_pl_image, g_pl_used < s->size ? g_pl_used : (size_t)s->size);
}
static void pl_service(Service *s, IpcReq *req, IpcResp *resp) {
    ipc_note(s->name, req->cmd, 0);
    if (!pl_ready()) { resp->result = MAKE_RESULT(110, 1); return; }
    uint32_t type = req->data_size >= 4 ? IN(uint32_t, 0) : 0;
    if (type >= PL_FONT_TYPES) type = 0;
    switch (req->cmd) {
    case 0: break;                                                          // RequestLoad
    case 1: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;                // GetLoadState: loaded
    case 2: OUT(uint32_t, 0) = g_pl_size[type]; resp_size(resp, 4); break;  // GetSize
    case 3: OUT(uint32_t, 0) = g_pl_off[type]; resp_size(resp, 4); break;   // GetSharedMemoryAddressOffset
    case 4:                                                                 // GetSharedMemoryNativeHandle
        if (!g_pl_handle) kernel_create_shared_memory(PL_SHMEM_SIZE, pl_shmem_mapped, &g_pl_handle);
        resp->copy_handles[resp->ncopy++] = g_pl_handle;
        break;
    case 5: {                                                               // GetSharedFontInOrderOfPriority(language) -> loaded, count; types, offsets, sizes
        const uint32_t *lists[3] = {NULL, g_pl_off, g_pl_size};
        uint32_t count = PL_FONT_TYPES;
        for (int i = 0; i < 3; i++) {
            IpcBuf *b = out_buf(req, i);
            if (!b || !b->addr) continue;
            uint32_t n = (uint32_t)(b->size / 4) < count ? (uint32_t)(b->size / 4) : count;
            for (uint32_t t = 0; t < n; t++) {
                uint32_t v = lists[i] ? lists[i][t] : t;
                memcpy(G2H8(b->addr + 4 * t), &v, 4);
            }
        }
        OUT(uint8_t, 0) = 1; OUT(uint32_t, 4) = count; resp_size(resp, 8);
        break;
    }
    default: resp_size(resp, 0x10);
    }
}

#define ERR(n) {n, svc_error}
const ServiceDef g_service_defs_net[] = {
    ERR("nifm:u"), {"bsd:u", bsd_service}, {"bsd:s", bsd_service}, ERR("bsdcfg"), ERR("ssl"), ERR("friend:u"), ERR("ldn:u"), {"bcat:u", bcat_creator}, ERR("nim:eca"), ERR("notif:a"),
    ERR("sfdnsres"), ERR("nsd:u"), ERR("ntc"), ERR("ngct:u"), ERR("htc"), ERR("htc:tenv"), ERR("htcs"), ERR("irs"), ERR("nfc:mf:u"),
    ERR("nfc:user"), ERR("nfp:user"), ERR("mii:u"), ERR("banana"), ERR("pcm"), ERR("mm:u"), ERR("caps:su"), ERR("caps:u"), {"pl:u", pl_service},
    ERR("audin:u"), ERR("audout:u"), ERR("hwopus"),
    {"prepo:u", svc_ok}, {"ldr:ro", svc_ok}, {"csrng", csrng},
    {NULL, NULL},
};
