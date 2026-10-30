/*
 * hbx_svc.h - host services mailbox (sw2-sdk extension to ABI 3).
 *
 * Native code cannot reach the network or the PC's app library directly; the
 * host workstation can.  An app that declares `"services": [...]` in its
 * manifest receives a 64 KiB service block at ctx->args[5].  To call the host:
 *
 *   1. fill the request with hbx_svc_begin()/hbx_svc_put*()
 *   2. return HBX_SVC_CALL from hbx_app_main (do not tear anything down that
 *      you cannot rebuild: the image is unmapped)
 *   3. the host performs the operation, writes the reply, and re-enters the
 *      app with HBX_FLAG_RESUME set and the same block at args[5]
 *
 * `scratch` survives every round trip, so keep UI state (selection, scroll,
 * page) there.  The data region and heap are fresh on every entry.
 *
 * The header is additive: ctx layout and the ABI version are unchanged, and
 * apps that do not declare services never see a block.
 */
#ifndef HBX_SVC_H
#define HBX_SVC_H

#include "hbx_sdk.h"

#define HBX_SVC_MAGIC        0x43565348u  /* "HSVC" */
#define HBX_SVC_VERSION      1u
#define HBX_SVC_BLOCK_BYTES  65536u
#define HBX_SVC_DATA_BYTES   49152u
#define HBX_SVC_SCRATCH_BYTES 16320u

/* Return this from hbx_app_main to hand the request to the host. */
#define HBX_SVC_CALL         0x5356u      /* "SV" */

/* Operations.  Service names in the manifest gate them: "store" allows
 * CATALOG/INSTALL/REMOVE/REFRESH, "info" allows INFO only. */
enum hbx_svc_op {
    HBX_SVC_NONE     = 0,
    HBX_SVC_INFO     = 1,  /* reply: text "key=value\n" lines            */
    HBX_SVC_CATALOG  = 2,  /* a0 = first index; reply: hbx_svc_app[]     */
    HBX_SVC_INSTALL  = 3,  /* text: app id; installs or updates          */
    HBX_SVC_REMOVE   = 4,  /* text: app id                               */
    HBX_SVC_REFRESH  = 5   /* re-download the catalog index              */
};

/* Results (in `result`).  Negative = failure, message in `data` as text. */
#define HBX_SVC_OK            0
#define HBX_SVC_E_DENIED     -1   /* service not declared in the manifest */
#define HBX_SVC_E_UNKNOWN    -2   /* unknown op or app id                 */
#define HBX_SVC_E_NETWORK    -3
#define HBX_SVC_E_INTEGRITY  -4   /* sha256 or package validation failed  */
#define HBX_SVC_E_IO         -5

struct hbx_svc {
    uint32_t magic;          /* HBX_SVC_MAGIC (host)                       */
    uint32_t version;        /* HBX_SVC_VERSION (host)                     */
    uint32_t op;             /* in:  enum hbx_svc_op; host clears to NONE  */
    uint32_t sequence;       /* in:  bumped by hbx_svc_begin               */
    int32_t  result;         /* out: HBX_SVC_OK or HBX_SVC_E_*             */
    uint32_t bytes;          /* in: request bytes; out: reply bytes        */
    uint32_t a0, a1;         /* in: small arguments                        */
    uint32_t r0, r1;         /* out: small results (e.g. total, count)     */
    uint32_t completed;      /* out: sequence of the last completed call   */
    uint32_t reserved[5];
    uint8_t  scratch[HBX_SVC_SCRATCH_BYTES]; /* app-owned, persistent      */
    uint8_t  data[HBX_SVC_DATA_BYTES];       /* request in, reply out      */
};
_Static_assert(sizeof(struct hbx_svc) == HBX_SVC_BLOCK_BYTES, "hbx_svc");

/* One catalog row (HBX_SVC_CATALOG reply), fixed size for easy paging. */
#define HBX_SVC_APP_INSTALLED 0x1u
#define HBX_SVC_APP_UPDATE    0x2u
#define HBX_SVC_APP_GPUBREW   0x4u

struct hbx_svc_app {
    char     id[64];
    char     name[48];
    char     version[16];
    char     installed_version[16];
    char     author[32];
    char     category[16];
    char     description[160];
    uint32_t size_bytes;
    uint32_t flags;
};
_Static_assert(sizeof(struct hbx_svc_app) == 360, "hbx_svc_app");
#define HBX_SVC_APPS_PER_PAGE (HBX_SVC_DATA_BYTES / sizeof(struct hbx_svc_app))

static inline struct hbx_svc *hbx_svc_block(const struct hbx_ctx *ctx) {
    struct hbx_svc *svc = (struct hbx_svc *)(uintptr_t)ctx->args[5];
    return svc && svc->magic == HBX_SVC_MAGIC && svc->version >= HBX_SVC_VERSION
        ? svc : (struct hbx_svc *)0;
}

/* True on the entry that follows a completed call. */
static inline int hbx_svc_replied(const struct hbx_svc *svc) {
    return svc && svc->sequence && svc->completed == svc->sequence;
}

static inline void hbx_svc_begin(struct hbx_svc *svc, uint32_t op,
                                 uint32_t a0, uint32_t a1) {
    svc->op = op;
    svc->a0 = a0;
    svc->a1 = a1;
    svc->bytes = 0;
    svc->sequence++;
    if (!svc->sequence) svc->sequence = 1;
}

/* Append a NUL-terminated string to the request payload. */
static inline void hbx_svc_put_text(struct hbx_svc *svc, const char *text) {
    while (*text && svc->bytes + 1u < HBX_SVC_DATA_BYTES)
        svc->data[svc->bytes++] = (uint8_t)*text++;
    svc->data[svc->bytes] = 0;
}

static inline const struct hbx_svc_app *hbx_svc_apps(const struct hbx_svc *svc,
                                                     uint32_t *count) {
    if (count) *count = svc->result == HBX_SVC_OK ? svc->r1 : 0u;
    return (const struct hbx_svc_app *)(const void *)svc->data;
}

#endif /* HBX_SVC_H */
