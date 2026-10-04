/* The vendor default reserves 4 MiB for all MMAP capture buffers.  A single
 * page-aligned 1080p NV12 frame consumes roughly 3 MiB, leaving the legacy
 * queue unable to grant the two buffers required to overlap capture and
 * encoding.  TX_ISP_FRAME_CHANNEL_BUFFER_MAX (8 MiB: two full-resolution
 * frames plus the down-scaled channels) is the recommended isp_mmap_pool_kb
 * value for MMAP users; the pool itself is off by default (USERPTR only),
 * see isp_mmap_pool_kb in source/tx-isp-videobuf.c. */
#include "source/tx-isp-videobuf.h"
#undef TX_ISP_FRAME_CHANNEL_BUFFER_MAX
#define TX_ISP_FRAME_CHANNEL_BUFFER_MAX (8 * 1024 * 1024)
#include "source/tx-isp-videobuf.c"
