#ifndef EDU_DMA_H
#define EDU_DMA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EDU_IOC_MAGIC 'E'

enum edu_mode {
    EDU_MODE_MEMCPY    = 0,
    EDU_MODE_COHERENT  = 1,
    EDU_MODE_STREAMING = 2,
    EDU_MODE_SG        = 3,
    EDU_MODE_MAX
};

enum edu_direction {
    EDU_DIR_TO_DEVICE      = 0,
    EDU_DIR_FROM_DEVICE    = 1,
    EDU_DIR_BIDIRECTIONAL  = 2
};

#define EDU_F_NO_VERIFY    0x1

#define EDU_VERIFY_FAILED  0
#define EDU_VERIFY_OK      1
#define EDU_VERIFY_SKIPPED 2

struct edu_dma_request {
    __u32 mode;
    __u32 direction;
    __u32 size;
    __u32 segments;
    __u32 flags;
    __u32 mapped_src;
    __u32 mapped_dst;
    __u32 verified;
    __u64 elapsed_ns;
    __u64 dma_ops;
};

#define EDU_DMA_TRANSFER _IOWR(EDU_IOC_MAGIC, 1, struct edu_dma_request)

#endif
