#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include "edu_dma.h"

#define SEG_MAX (128UL * 1024)

struct cpu_stat {
    unsigned long long busy, total;
};

static const char *mode_names[] = { "memcpy", "coherent", "streaming", "sg" };
static const char *dir_names[] = { "to-device", "from-device", "bidirectional" };

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [-m mode] [-d dir] [-n segments] [-x] <size> [segments]\n"
        "  %s -b [-d dir] [-F] [-x]\n\n"
        "  -m  memcpy | coherent | streaming | sg   (default: sg)\n"
        "  -d  to | from | bidir                     (default: bidir)\n"
        "  -n  number of scatter-gather segments (sg mode)\n"
        "  -x  skip verification (only the requested direction is transferred)\n"
        "  -b  benchmark: memcpy vs DMA at 1K, 64K, 1M, 16M\n"
        "  -F  with -b: also run DMA on 16M (takes very long: 100 ms per 4 KiB)\n"
        "  size accepts K/M suffixes, e.g. 64K, 1M\n",
        prog, prog);
}

static int parse_size(const char *s, unsigned long *out)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || end == s)
        return -1;
    if (*end == 'k' || *end == 'K') {
        v <<= 10;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        v <<= 20;
        end++;
    }
    if (*end)
        return -1;
    *out = v;
    return 0;
}

static int read_cpu(struct cpu_stat *st)
{
    unsigned long long v[8] = {0};
    FILE *f = fopen("/proc/stat", "r");
    int i;

    if (!f)
        return -1;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) {
        fclose(f);
        return -1;
    }
    fclose(f);

    st->total = 0;
    for (i = 0; i < 8; i++)
        st->total += v[i];
    st->busy = st->total - v[3] - v[4];
    return 0;
}

static double cpu_load(const struct cpu_stat *a, const struct cpu_stat *b)
{
    unsigned long long total = b->total - a->total;

    if (!total)
        return 0.0;
    return 100.0 * (double)(b->busy - a->busy) / (double)total;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned default_segments(unsigned long size)
{
    unsigned long n = size < 4 ? size : 4;

    if ((size + n - 1) / n > SEG_MAX)
        n = (size + SEG_MAX - 1) / SEG_MAX;
    return (unsigned)n;
}

static void print_proc(void)
{
    char line[128];
    FILE *f = fopen("/proc/dmadriver", "r");

    if (!f) {
        perror("open /proc/dmadriver");
        return;
    }
    printf("/proc/dmadriver:\n");
    while (fgets(line, sizeof(line), f))
        printf("  %s", line);
    fclose(f);
}

static const char *verdict(unsigned v)
{
    if (v == EDU_VERIFY_OK)
        return "SUCCESS";
    if (v == EDU_VERIFY_SKIPPED)
        return "SKIPPED";
    return "FAILED";
}

static int run_once(int fd, struct edu_dma_request *req)
{
    int ret = ioctl(fd, EDU_DMA_TRANSFER, req);

    if (ret < 0) {
        fprintf(stderr, "EDU_DMA_TRANSFER (%s, %u bytes): %s\n",
                mode_names[req->mode], req->size, strerror(errno));
        return -1;
    }
    return 0;
}

static int cmd_run(int fd, unsigned mode, unsigned dir, unsigned long size,
                   unsigned long segs, unsigned flags)
{
    struct edu_dma_request req = {0};
    struct cpu_stat c0, c1;
    double t0, wall;

    req.mode = mode;
    req.direction = dir;
    req.size = (uint32_t)size;
    req.segments = (uint32_t)(segs ? segs : default_segments(size));
    req.flags = flags;

    read_cpu(&c0);
    t0 = now_sec();
    if (run_once(fd, &req))
        return 1;
    wall = now_sec() - t0;
    read_cpu(&c1);

    printf("Mode:              %s\n", mode_names[mode]);
    printf("Direction:         %s\n", dir_names[dir]);
    if (mode == EDU_MODE_SG) {
        printf("Segments:          %u\n", req.segments);
        printf("Mapped segments:   src %u, dst %u (after dma_map_sg)\n",
               req.mapped_src, req.mapped_dst);
    }
    printf("Total size:        %u bytes\n", req.size);
    printf("DMA operations:    %llu\n", (unsigned long long)req.dma_ops);
    printf("Transfer time:     %.3f ms\n", req.elapsed_ns / 1e6);
    if (req.elapsed_ns)
        printf("Throughput:        %.3f MiB/s\n",
               (req.size / 1048576.0) / (req.elapsed_ns / 1e9));
    printf("Wall time:         %.3f s\n", wall);
    printf("CPU load:          %.1f%% (all CPUs, /proc/stat before/after)\n",
           cpu_load(&c0, &c1));
    printf("Verification:      %s\n", verdict(req.verified));
    print_proc();

    return req.verified == EDU_VERIFY_FAILED ? 2 : 0;
}

static int bench_one(int fd, unsigned mode, unsigned dir, unsigned long size,
                     unsigned flags)
{
    struct edu_dma_request req = {0};
    struct cpu_stat c0, c1;
    unsigned long long ns = 0;
    unsigned long reps = 0;
    unsigned verified = EDU_VERIFY_OK;
    double t0, wall, avg_ns;

    req.mode = mode;
    req.direction = dir;
    req.size = (uint32_t)size;
    req.segments = (mode == EDU_MODE_SG) ? (uint32_t)((size + 4095) / 4096)
                                         : 1;
    req.flags = flags;
    if (mode == EDU_MODE_SG && (size + req.segments - 1) / req.segments > SEG_MAX)
        req.segments = default_segments(size);

    read_cpu(&c0);
    t0 = now_sec();
    do {
        if (run_once(fd, &req))
            return -1;
        ns += req.elapsed_ns;
        if (req.verified == EDU_VERIFY_FAILED)
            verified = EDU_VERIFY_FAILED;
        else if (req.verified == EDU_VERIFY_SKIPPED &&
                 verified != EDU_VERIFY_FAILED)
            verified = EDU_VERIFY_SKIPPED;
        reps++;
        wall = now_sec() - t0;
    } while (wall < 0.5 && reps < 100000);
    read_cpu(&c1);

    avg_ns = (double)ns / reps;
    printf("%-10s %9lu %6lu %14.3f %14.3f %8.1f%% %-8s\n",
           mode_names[mode], size, reps, avg_ns / 1e3,
           avg_ns ? (size / 1048576.0) / (avg_ns / 1e9) : 0.0,
           cpu_load(&c0, &c1), verdict(verified));
    fflush(stdout);

    return verified == EDU_VERIFY_FAILED ? 2 : 0;
}

static int cmd_bench(int fd, unsigned dir, int full, unsigned flags)
{
    static const unsigned long sizes[] = {
        1UL << 10, 64UL << 10, 1UL << 20, 16UL << 20
    };
    static const unsigned modes[] = {
        EDU_MODE_MEMCPY, EDU_MODE_COHERENT, EDU_MODE_STREAMING, EDU_MODE_SG
    };
    int rc = 0;
    unsigned i, j;

    printf("Direction: %s, verification %s\n", dir_names[dir],
           (flags & EDU_F_NO_VERIFY) ? "off" : "on");
    printf("%-10s %9s %6s %14s %14s %9s %-8s\n", "method", "size", "reps",
           "time/op,us", "MiB/s", "CPU", "verify");

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (modes[j] != EDU_MODE_MEMCPY && sizes[i] > (1UL << 20) &&
                !full) {
                printf("%-10s %9lu   skipped (use -F; ~%lu s per op)\n",
                       mode_names[modes[j]], sizes[i],
                       sizes[i] / 4096 / 10 *
                       ((flags & EDU_F_NO_VERIFY) ? 1 : 2));
                continue;
            }
            if (bench_one(fd, modes[j], dir, sizes[i], flags) < 0)
                rc = 1;
        }
    }
    print_proc();
    return rc;
}

int main(int argc, char **argv)
{
    unsigned mode = EDU_MODE_SG, dir = EDU_DIR_BIDIRECTIONAL, flags = 0;
    unsigned long size = 0, segs = 0;
    int bench = 0, full = 0, opt, fd, ret;

    while ((opt = getopt(argc, argv, "m:d:n:xbFh")) != -1) {
        switch (opt) {
        case 'm':
            for (mode = 0; mode < EDU_MODE_MAX; mode++)
                if (!strcmp(optarg, mode_names[mode]))
                    break;
            if (mode == EDU_MODE_MAX) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'd':
            if (!strcmp(optarg, "to"))
                dir = EDU_DIR_TO_DEVICE;
            else if (!strcmp(optarg, "from"))
                dir = EDU_DIR_FROM_DEVICE;
            else if (!strcmp(optarg, "bidir"))
                dir = EDU_DIR_BIDIRECTIONAL;
            else {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'n':
            if (parse_size(optarg, &segs) || !segs || segs > 4096) {
                fprintf(stderr, "Invalid segment count\n");
                return 1;
            }
            break;
        case 'x':
            flags |= EDU_F_NO_VERIFY;
            break;
        case 'b':
            bench = 1;
            break;
        case 'F':
            full = 1;
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (!bench) {
        if (optind >= argc || argc - optind > 2 ||
            parse_size(argv[optind], &size) ||
            !size || size > 16UL * 1024 * 1024) {
            usage(argv[0]);
            return 1;
        }
        if (argc - optind == 2 &&
            (parse_size(argv[optind + 1], &segs) || !segs || segs > 4096)) {
            fprintf(stderr, "Invalid segment count\n");
            return 1;
        }
    }

    fd = open("/dev/dmadriver", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dmadriver");
        return 1;
    }

    if (bench)
        ret = cmd_bench(fd, dir, full, flags);
    else
        ret = cmd_run(fd, mode, dir, size, segs, flags);

    close(fd);
    return ret;
}
