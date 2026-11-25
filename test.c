#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

#include "edu_dma.h"

int main(int argc, char **argv)
{
    struct edu_dma_request req = {0};
    char *end;
    unsigned long size, segments;
    int fd, ret;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <size_bytes> <segments>\n", argv[0]);
        return 1;
    }

    errno = 0;
    size = strtoul(argv[1], &end, 10);
    if (errno || *end || size == 0 || size > 16UL * 1024 * 1024) {
        fprintf(stderr, "Invalid size\n");
        return 1;
    }

    errno = 0;
    segments = strtoul(argv[2], &end, 10);
    if (errno || *end || segments == 0 || segments > 4096) {
        fprintf(stderr, "Invalid segment count\n");
        return 1;
    }

    req.size = (uint32_t)size;
    req.segments = (uint32_t)segments;

    fd = open("/dev/dmadriver", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dmadriver");
        return 1;
    }

    ret = ioctl(fd, EDU_DMA_TRANSFER, &req);
    if (ret < 0) {
        perror("EDU_DMA_TRANSFER");
        close(fd);
        return 1;
    }

    printf("Requested size:   %u bytes\n", req.size);
    printf("Original entries: %u\n", req.segments);
    printf("Mapped src:       %u\n", req.mapped_src);
    printf("Mapped dst:       %u\n", req.mapped_dst);
    printf("Elapsed:          %llu ns\n",
           (unsigned long long)req.elapsed_ns);
    printf("Verification:     %s\n",
           req.verified ? "SUCCESS" : "FAILED");

    if (req.elapsed_ns) {
        double seconds = (double)req.elapsed_ns / 1e9;
        double mib = (double)req.size / (1024.0 * 1024.0);
        printf("Throughput:       %.2f MiB/s\n", mib / seconds);
    }

    close(fd);
    return req.verified ? 0 : 2;
}
