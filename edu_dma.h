#ifndef EDU_DMA_H
#define EDU_DMA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EDU_IOC_MAGIC 'E'

struct edu_dma_request {
    __u32 size;
    __u32 segments;
    __u32 mapped_src;
    __u32 mapped_dst;
    __u64 elapsed_ns;
    __u32 verified;
};

#define EDU_DMA_TRANSFER _IOWR(EDU_IOC_MAGIC, 1, struct edu_dma_request)

#endif
