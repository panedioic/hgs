#pragma once

#include <stdint.h>

#define HGS_XIM_TEXT_LIMIT 4096

enum hgs_xim_notice_type { HGS_XIM_READY = 1, HGS_XIM_FOCUS = 2 };

// SOCK_SEQPACKET preserves a complete notice or UTF-8 commit per record.
struct hgs_xim_notice { uint32_t type, xid; };
struct hgs_xim_commit { uint32_t xid; char text[HGS_XIM_TEXT_LIMIT]; };

#ifdef __cplusplus
extern "C" {
#endif
int hgs_spawn_xim(const char *display, const char *authority, const char *server_name);
#ifdef __cplusplus
}
#endif
