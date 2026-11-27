// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU EDU DMA driver (lab 5, variant 5: scatter-gather).
 *
 * Modes (selected from user space via ioctl):
 *   memcpy    - CPU copy, baseline for comparison;
 *   coherent  - buffers from dma_alloc_coherent(), allocated in probe();
 *   streaming - kmalloc buffers, dma_map_single() + dma_sync_single_*();
 *   sg        - pages mapped with dma_map_sg(), transferred as several
 *               sequential EDU operations sharing one completion.
 *
 * Stock QEMU EDU facts this driver is built around:
 *   - DMA registers: SRC 0x80, DST 0x88, COUNT 0x90, CMD 0x98 (64-bit);
 *     0x60 raises and 0x64 acknowledges interrupt bits, 0x24 is IRQ status;
 *   - the device has a 4 KiB internal buffer at device address 0x40000,
 *     so one operation moves at most 4096 bytes and has no SG descriptors;
 *   - every operation completes after a fixed 100 ms QEMU timer;
 *   - host addresses are masked with the device "dma_mask" property
 *     (28 bits by default). aarch64 "virt" RAM starts at 1 GiB, so QEMU
 *     must be started with -device edu,dma_mask=0xffffffffffffffff.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/io-64-nonatomic-lo-hi.h>

#include "edu_dma.h"

#define EDU_VENDOR_ID          0x1234
#define EDU_DEVICE_ID          0x11e8

#define EDU_IRQ_STATUS         0x24
#define EDU_IRQ_ACK            0x64
#define EDU_DMA_SRC            0x80
#define EDU_DMA_DST            0x88
#define EDU_DMA_COUNT          0x90
#define EDU_DMA_CMD            0x98

#define EDU_DMA_BUFFER         0x40000
#define EDU_DMA_BUFFER_SIZE    4096

#define EDU_IRQ_DMA            0x100
#define EDU_DMA_START          0x01
#define EDU_DMA_TO_RAM         0x02
#define EDU_DMA_IRQ            0x04

#define EDU_DMA_TIMEOUT_MS     5000
#define EDU_MAX_SIZE           (16U * 1024U * 1024U)
#define EDU_MAX_SEGMENTS       4096U
#define EDU_SEG_MAX            (128U * 1024U)
#define EDU_WINDOW             (128U * 1024U)

struct edu_dma_device {
    struct pci_dev *pdev;
    void __iomem *regs;
    struct completion dma_done;
    struct mutex lock;
    struct miscdevice miscdev;
    struct proc_dir_entry *proc;
    struct kref ref;
    bool removing;
    bool dma_ok;

    void *coh_src;
    void *coh_dst;
    dma_addr_t coh_src_dma;
    dma_addr_t coh_dst_dma;

    atomic64_t transfers;
    atomic64_t total_bytes;
    atomic64_t dma_ops;
    atomic64_t errors;
    atomic64_t timeouts;
    atomic64_t mode_transfers[EDU_MODE_MAX];
};

struct edu_xfer {
    struct edu_dma_device *edu;
    u32 dir;
    bool verify;
    u64 ns;
    u64 ops;
};

struct dma_piece {
    dma_addr_t addr;
    u32 len;
};

static const char * const edu_mode_names[EDU_MODE_MAX] = {
    "memcpy", "coherent", "streaming", "sg",
};

static irqreturn_t edu_irq_handler(int irq, void *data)
{
    struct edu_dma_device *edu = data;
    u32 status;

    status = ioread32(edu->regs + EDU_IRQ_STATUS);
    if (!(status & EDU_IRQ_DMA))
        return IRQ_NONE;

    iowrite32(EDU_IRQ_DMA, edu->regs + EDU_IRQ_ACK);
    complete(&edu->dma_done);

    return IRQ_HANDLED;
}

static void edu_quiesce(struct edu_dma_device *edu)
{
    unsigned long end = jiffies + msecs_to_jiffies(1000);

    while ((ioread32(edu->regs + EDU_DMA_CMD) & EDU_DMA_START) &&
           time_before(jiffies, end))
        msleep(10);

    if (ioread32(edu->regs + EDU_IRQ_STATUS) & EDU_IRQ_DMA)
        iowrite32(EDU_IRQ_DMA, edu->regs + EDU_IRQ_ACK);
}

/*
 * One EDU DMA operation of at most 4096 bytes:
 *   from_edu == false: host RAM -> EDU internal buffer
 *   from_edu == true:  EDU internal buffer -> host RAM
 */
static int edu_dma_one(struct edu_dma_device *edu, dma_addr_t ram_dma,
                       u32 len, bool from_edu)
{
    long left;

    if (!len || len > EDU_DMA_BUFFER_SIZE)
        return -EINVAL;

    reinit_completion(&edu->dma_done);

    if (!from_edu) {
        iowrite64(ram_dma, edu->regs + EDU_DMA_SRC);
        iowrite64(EDU_DMA_BUFFER, edu->regs + EDU_DMA_DST);
    } else {
        iowrite64(EDU_DMA_BUFFER, edu->regs + EDU_DMA_SRC);
        iowrite64(ram_dma, edu->regs + EDU_DMA_DST);
    }
    iowrite64(len, edu->regs + EDU_DMA_COUNT);
    iowrite64(EDU_DMA_START | EDU_DMA_IRQ |
              (from_edu ? EDU_DMA_TO_RAM : 0),
              edu->regs + EDU_DMA_CMD);

    left = wait_for_completion_killable_timeout(
        &edu->dma_done, msecs_to_jiffies(EDU_DMA_TIMEOUT_MS));
    if (left <= 0) {
        edu_quiesce(edu);
        if (left == 0) {
            atomic64_inc(&edu->timeouts);
            dev_err(&edu->pdev->dev, "DMA timeout\n");
            return -ETIMEDOUT;
        }
        return -EINTR;
    }

    if (ioread32(edu->regs + EDU_DMA_CMD) & EDU_DMA_START) {
        dev_err(&edu->pdev->dev, "DMA still running after IRQ\n");
        return -EIO;
    }

    if (ioread32(edu->regs + EDU_IRQ_STATUS) & EDU_IRQ_DMA) {
        dev_err(&edu->pdev->dev, "DMA IRQ status was not acknowledged\n");
        iowrite32(EDU_IRQ_DMA, edu->regs + EDU_IRQ_ACK);
        return -EIO;
    }

    atomic64_inc(&edu->dma_ops);
    return 0;
}

/*
 * One leg of a copy. Only legs belonging to the requested direction are
 * timed; the other leg exists to load or read back data for verification
 * and is skipped altogether when verification is disabled.
 */
static int edu_leg(struct edu_xfer *x, dma_addr_t addr, u32 len, bool from_edu)
{
    bool wanted = from_edu ? x->dir != EDU_DIR_TO_DEVICE
                           : x->dir != EDU_DIR_FROM_DEVICE;
    u64 t0;
    int ret;

    if (!wanted && !x->verify)
        return 0;

    t0 = ktime_get_ns();
    ret = edu_dma_one(x->edu, addr, len, from_edu);
    if (wanted)
        x->ns += ktime_get_ns() - t0;
    if (!ret)
        x->ops++;

    return ret;
}

static int edu_copy_window(struct edu_xfer *x, dma_addr_t src,
                           dma_addr_t dst, u32 len)
{
    u32 off;

    for (off = 0; off < len; off += EDU_DMA_BUFFER_SIZE) {
        u32 chunk = min_t(u32, EDU_DMA_BUFFER_SIZE, len - off);
        int ret;

        ret = edu_leg(x, src + off, chunk, false);
        if (ret)
            return ret;

        ret = edu_leg(x, dst + off, chunk, true);
        if (ret)
            return ret;
    }

    return 0;
}

static u8 edu_pattern(u32 offset)
{
    return (u8)(offset * 131U + 17U + (offset >> 8) * 29U);
}

static void edu_fill(u8 *p, u32 len, u32 base)
{
    u32 i;

    for (i = 0; i < len; i++)
        p[i] = edu_pattern(base + i);
}

static void edu_dir_to_dma(u32 dir, enum dma_data_direction *src_dir,
                           enum dma_data_direction *dst_dir)
{
    if (dir == EDU_DIR_BIDIRECTIONAL) {
        *src_dir = DMA_BIDIRECTIONAL;
        *dst_dir = DMA_BIDIRECTIONAL;
    } else {
        *src_dir = DMA_TO_DEVICE;
        *dst_dir = DMA_FROM_DEVICE;
    }
}

static int edu_run_coherent(struct edu_xfer *x, struct edu_dma_request *req)
{
    struct edu_dma_device *edu = x->edu;
    u32 pos;
    int ret;

    for (pos = 0; pos < req->size; pos += EDU_WINDOW) {
        u32 len = min_t(u32, EDU_WINDOW, req->size - pos);

        edu_fill(edu->coh_src, len, pos);
        memset(edu->coh_dst, 0, len);

        ret = edu_copy_window(x, edu->coh_src_dma, edu->coh_dst_dma, len);
        if (ret)
            return ret;

        if (x->verify && memcmp(edu->coh_src, edu->coh_dst, len)) {
            req->verified = EDU_VERIFY_FAILED;
            return -EIO;
        }
    }

    return 0;
}

static int edu_run_streaming(struct edu_xfer *x, struct edu_dma_request *req)
{
    struct device *dev = &x->edu->pdev->dev;
    enum dma_data_direction sdir, ddir;
    u32 wsize = min_t(u32, EDU_WINDOW, req->size);
    dma_addr_t sdma, ddma;
    void *src, *dst;
    u32 pos;
    int ret;

    edu_dir_to_dma(req->direction, &sdir, &ddir);

    src = kmalloc(wsize, GFP_KERNEL);
    dst = kmalloc(wsize, GFP_KERNEL);
    if (!src || !dst) {
        ret = -ENOMEM;
        goto out_free;
    }

    sdma = dma_map_single(dev, src, wsize, sdir);
    if (dma_mapping_error(dev, sdma)) {
        ret = -EIO;
        goto out_free;
    }

    ddma = dma_map_single(dev, dst, wsize, ddir);
    if (dma_mapping_error(dev, ddma)) {
        ret = -EIO;
        goto out_unmap_src;
    }

    req->mapped_src = 1;
    req->mapped_dst = 1;

    for (pos = 0, ret = 0; pos < req->size && !ret; pos += EDU_WINDOW) {
        u32 len = min_t(u32, EDU_WINDOW, req->size - pos);

        dma_sync_single_for_cpu(dev, sdma, len, sdir);
        dma_sync_single_for_cpu(dev, ddma, len, ddir);
        edu_fill(src, len, pos);
        memset(dst, 0, len);
        dma_sync_single_for_device(dev, sdma, len, sdir);
        dma_sync_single_for_device(dev, ddma, len, ddir);

        ret = edu_copy_window(x, sdma, ddma, len);

        dma_sync_single_for_cpu(dev, sdma, len, sdir);
        dma_sync_single_for_cpu(dev, ddma, len, ddir);
        if (!ret && x->verify && memcmp(src, dst, len)) {
            req->verified = EDU_VERIFY_FAILED;
            ret = -EIO;
        }
        dma_sync_single_for_device(dev, sdma, len, sdir);
        dma_sync_single_for_device(dev, ddma, len, ddir);
    }

    dma_unmap_single(dev, ddma, wsize, ddir);
out_unmap_src:
    dma_unmap_single(dev, sdma, wsize, sdir);
out_free:
    kfree(dst);
    kfree(src);
    return ret;
}

static int edu_run_memcpy(struct edu_xfer *x, struct edu_dma_request *req)
{
    u32 wsize = min_t(u32, EDU_WINDOW, req->size);
    void *src, *dst;
    u32 pos;
    int ret = 0;

    src = kmalloc(wsize, GFP_KERNEL);
    dst = kmalloc(wsize, GFP_KERNEL);
    if (!src || !dst) {
        ret = -ENOMEM;
        goto out;
    }

    for (pos = 0; pos < req->size; pos += EDU_WINDOW) {
        u32 len = min_t(u32, EDU_WINDOW, req->size - pos);
        u64 t0;

        edu_fill(src, len, pos);
        memset(dst, 0, len);

        t0 = ktime_get_ns();
        memcpy(dst, src, len);
        x->ns += ktime_get_ns() - t0;

        if (x->verify && memcmp(src, dst, len)) {
            req->verified = EDU_VERIFY_FAILED;
            ret = -EIO;
            break;
        }
    }

out:
    kfree(dst);
    kfree(src);
    return ret;
}

static void edu_free_segments(struct page **pages, u32 n, unsigned int *orders)
{
    u32 i;

    if (!pages)
        return;

    for (i = 0; i < n; i++) {
        if (pages[i])
            __free_pages(pages[i], orders[i]);
    }
}

static int edu_collect_pieces(struct scatterlist *sgl, int mapped_nents,
                              struct dma_piece *pieces)
{
    struct scatterlist *sg;
    int i;

    for_each_sg(sgl, sg, mapped_nents, i) {
        pieces[i].addr = sg_dma_address(sg);
        pieces[i].len = sg_dma_len(sg);
    }

    return mapped_nents;
}

/*
 * EDU has no SG descriptor engine, so a mapped SG list is walked as a byte
 * stream and sent as several sequential operations that share one
 * completion. dma_map_sg() may merge entries, so source and destination
 * mapped segment boundaries need not match.
 */
static int edu_copy_sg_streams(struct edu_xfer *x,
                               struct dma_piece *src, int src_nents,
                               struct dma_piece *dst, int dst_nents,
                               u32 total_size)
{
    int si = 0, di = 0;
    u32 so = 0, doff = 0;
    u32 done = 0;

    while (done < total_size) {
        u32 len;
        int ret;

        if (si >= src_nents || di >= dst_nents)
            return -EIO;

        len = min(src[si].len - so, dst[di].len - doff);
        len = min_t(u32, len, EDU_DMA_BUFFER_SIZE);
        len = min(len, total_size - done);

        ret = edu_leg(x, src[si].addr + so, len, false);
        if (ret)
            return ret;

        ret = edu_leg(x, dst[di].addr + doff, len, true);
        if (ret)
            return ret;

        done += len;
        so += len;
        doff += len;

        if (so == src[si].len) {
            si++;
            so = 0;
        }
        if (doff == dst[di].len) {
            di++;
            doff = 0;
        }
    }

    return 0;
}

static int edu_run_sg(struct edu_xfer *x, struct edu_dma_request *req)
{
    struct device *dev = &x->edu->pdev->dev;
    struct page **src_pages = NULL, **dst_pages = NULL;
    struct scatterlist *src_sg = NULL, *dst_sg = NULL;
    struct dma_piece *src_pieces = NULL, *dst_pieces = NULL;
    unsigned int *orders = NULL;
    u32 *lens = NULL;
    enum dma_data_direction sdir, ddir;
    bool src_is_mapped = false, dst_is_mapped = false;
    int src_mapped = 0, dst_mapped = 0;
    u32 n = req->segments;
    u32 size = req->size;
    u32 i, pos = 0;
    int ret = 0;

    edu_dir_to_dma(req->direction, &sdir, &ddir);

    src_pages = kcalloc(n, sizeof(*src_pages), GFP_KERNEL);
    dst_pages = kcalloc(n, sizeof(*dst_pages), GFP_KERNEL);
    src_sg = kcalloc(n, sizeof(*src_sg), GFP_KERNEL);
    dst_sg = kcalloc(n, sizeof(*dst_sg), GFP_KERNEL);
    src_pieces = kcalloc(n, sizeof(*src_pieces), GFP_KERNEL);
    dst_pieces = kcalloc(n, sizeof(*dst_pieces), GFP_KERNEL);
    orders = kcalloc(n, sizeof(*orders), GFP_KERNEL);
    lens = kcalloc(n, sizeof(*lens), GFP_KERNEL);
    if (!src_pages || !dst_pages || !src_sg || !dst_sg ||
        !src_pieces || !dst_pieces || !orders || !lens) {
        ret = -ENOMEM;
        goto out;
    }

    sg_init_table(src_sg, n);
    sg_init_table(dst_sg, n);

    /* Separate allocations, so the segments are not contiguous in RAM. */
    for (i = 0; i < n; i++) {
        u32 len = DIV_ROUND_UP(size - pos, n - i);

        orders[i] = get_order(len);
        lens[i] = len;

        src_pages[i] = alloc_pages(GFP_KERNEL, orders[i]);
        dst_pages[i] = alloc_pages(GFP_KERNEL | __GFP_ZERO, orders[i]);
        if (!src_pages[i] || !dst_pages[i]) {
            ret = -ENOMEM;
            goto out;
        }

        edu_fill(page_address(src_pages[i]), len, pos);
        sg_set_page(&src_sg[i], src_pages[i], len, 0);
        sg_set_page(&dst_sg[i], dst_pages[i], len, 0);
        pos += len;
    }

    src_mapped = dma_map_sg(dev, src_sg, n, sdir);
    if (!src_mapped) {
        ret = -EIO;
        goto out;
    }
    src_is_mapped = true;

    dst_mapped = dma_map_sg(dev, dst_sg, n, ddir);
    if (!dst_mapped) {
        ret = -EIO;
        goto out;
    }
    dst_is_mapped = true;

    req->mapped_src = src_mapped;
    req->mapped_dst = dst_mapped;

    edu_collect_pieces(src_sg, src_mapped, src_pieces);
    edu_collect_pieces(dst_sg, dst_mapped, dst_pieces);

    ret = edu_copy_sg_streams(x, src_pieces, src_mapped,
                              dst_pieces, dst_mapped, size);

    /* Unmapping hands the buffers back to the CPU before verification. */
    dma_unmap_sg(dev, dst_sg, n, ddir);
    dst_is_mapped = false;
    dma_unmap_sg(dev, src_sg, n, sdir);
    src_is_mapped = false;

    if (!ret && x->verify) {
        for (i = 0; i < n; i++) {
            if (memcmp(page_address(src_pages[i]),
                       page_address(dst_pages[i]), lens[i])) {
                req->verified = EDU_VERIFY_FAILED;
                ret = -EIO;
                break;
            }
        }
    }

out:
    if (dst_is_mapped)
        dma_unmap_sg(dev, dst_sg, n, ddir);
    if (src_is_mapped)
        dma_unmap_sg(dev, src_sg, n, sdir);

    edu_free_segments(src_pages, n, orders);
    edu_free_segments(dst_pages, n, orders);
    kfree(src_pages);
    kfree(dst_pages);
    kfree(src_sg);
    kfree(dst_sg);
    kfree(src_pieces);
    kfree(dst_pieces);
    kfree(orders);
    kfree(lens);

    return ret;
}

static int edu_validate(struct edu_dma_request *req)
{
    if (req->mode >= EDU_MODE_MAX || req->direction > EDU_DIR_BIDIRECTIONAL)
        return -EINVAL;
    if (!req->size || req->size > EDU_MAX_SIZE)
        return -EINVAL;

    if (req->mode == EDU_MODE_SG) {
        if (!req->segments || req->segments > EDU_MAX_SEGMENTS ||
            req->segments > req->size ||
            DIV_ROUND_UP(req->size, req->segments) > EDU_SEG_MAX)
            return -EINVAL;
    } else {
        req->segments = 1;
    }

    return 0;
}

static int edu_dma_transfer(struct edu_dma_device *edu,
                            struct edu_dma_request *req)
{
    struct edu_xfer x = {
        .edu = edu,
        .dir = req->direction,
        .verify = !(req->flags & EDU_F_NO_VERIFY),
    };
    int ret;

    req->mapped_src = 0;
    req->mapped_dst = 0;
    req->elapsed_ns = 0;
    req->dma_ops = 0;
    req->verified = x.verify ? EDU_VERIFY_OK : EDU_VERIFY_SKIPPED;

    ret = edu_validate(req);
    if (ret)
        return ret;

    switch (req->mode) {
    case EDU_MODE_MEMCPY:
        ret = edu_run_memcpy(&x, req);
        break;
    case EDU_MODE_COHERENT:
        ret = edu_run_coherent(&x, req);
        break;
    case EDU_MODE_STREAMING:
        ret = edu_run_streaming(&x, req);
        break;
    default:
        ret = edu_run_sg(&x, req);
        break;
    }

    req->elapsed_ns = x.ns;
    req->dma_ops = x.ops;

    if (ret) {
        if (ret != -EINTR && req->verified == EDU_VERIFY_OK)
            req->verified = EDU_VERIFY_FAILED;
        atomic64_inc(&edu->errors);
        return ret;
    }

    if (req->mode != EDU_MODE_MEMCPY) {
        atomic64_inc(&edu->transfers);
        atomic64_add(req->size, &edu->total_bytes);
    }
    atomic64_inc(&edu->mode_transfers[req->mode]);

    return 0;
}

static int edu_proc_show(struct seq_file *m, void *v)
{
    struct edu_dma_device *edu = m->private;
    int i;

    seq_printf(m, "transfers: %lld\n", atomic64_read(&edu->transfers));
    seq_printf(m, "total_bytes: %lld\n", atomic64_read(&edu->total_bytes));
    seq_printf(m, "dma_operations: %lld\n", atomic64_read(&edu->dma_ops));
    seq_printf(m, "errors: %lld\n", atomic64_read(&edu->errors));
    seq_printf(m, "timeouts: %lld\n", atomic64_read(&edu->timeouts));
    for (i = 0; i < EDU_MODE_MAX; i++)
        seq_printf(m, "mode_%s: %lld\n", edu_mode_names[i],
                   atomic64_read(&edu->mode_transfers[i]));

    return 0;
}

static void edu_release_dev(struct kref *ref)
{
    kfree(container_of(ref, struct edu_dma_device, ref));
}

static int edu_open(struct inode *inode, struct file *file)
{
    struct edu_dma_device *edu = container_of(file->private_data,
                                              struct edu_dma_device,
                                              miscdev);

    kref_get(&edu->ref);
    file->private_data = edu;
    return 0;
}

static int edu_close(struct inode *inode, struct file *file)
{
    struct edu_dma_device *edu = file->private_data;

    kref_put(&edu->ref, edu_release_dev);
    return 0;
}

static long edu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct edu_dma_device *edu = file->private_data;
    struct edu_dma_request req;
    int ret;

    if (cmd != EDU_DMA_TRANSFER)
        return -ENOTTY;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (mutex_lock_interruptible(&edu->lock))
        return -ERESTARTSYS;

    if (edu->removing)
        ret = -ENODEV;
    else
        ret = edu_dma_transfer(edu, &req);

    mutex_unlock(&edu->lock);

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;

    return ret;
}

static const struct file_operations edu_fops = {
    .owner          = THIS_MODULE,
    .open           = edu_open,
    .release        = edu_close,
    .unlocked_ioctl = edu_ioctl,
};

/* Catches the "QEMU dma_mask is still 28 bits" misconfiguration early. */
static void edu_selftest(struct edu_dma_device *edu)
{
    int ret;

    edu_fill(edu->coh_src, EDU_DMA_BUFFER_SIZE, 0);
    memset(edu->coh_dst, 0, EDU_DMA_BUFFER_SIZE);

    ret = edu_dma_one(edu, edu->coh_src_dma, EDU_DMA_BUFFER_SIZE, false);
    if (!ret)
        ret = edu_dma_one(edu, edu->coh_dst_dma, EDU_DMA_BUFFER_SIZE, true);

    if (ret) {
        dev_warn(&edu->pdev->dev, "DMA self-test failed: %d\n", ret);
        return;
    }

    if (memcmp(edu->coh_src, edu->coh_dst, EDU_DMA_BUFFER_SIZE)) {
        dev_warn(&edu->pdev->dev,
                 "DMA self-test: data mismatch (src 0x%llx, dst 0x%llx). "
                 "Start QEMU with -device edu,dma_mask=0xffffffffffffffff\n",
                 (u64)edu->coh_src_dma, (u64)edu->coh_dst_dma);
        return;
    }

    edu->dma_ok = true;
    dev_info(&edu->pdev->dev, "DMA self-test passed\n");
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct edu_dma_device *edu;
    int ret;

    edu = kzalloc(sizeof(*edu), GFP_KERNEL);
    if (!edu)
        return -ENOMEM;

    edu->pdev = pdev;
    kref_init(&edu->ref);
    init_completion(&edu->dma_done);
    mutex_init(&edu->lock);
    pci_set_drvdata(pdev, edu);

    ret = pci_enable_device(pdev);
    if (ret) {
        dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
        goto err_free;
    }

    ret = pci_request_regions(pdev, "edu_dma");
    if (ret) {
        dev_err(&pdev->dev, "pci_request_regions failed: %d\n", ret);
        goto err_disable;
    }

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret)
        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (ret) {
        dev_err(&pdev->dev, "DMA mask setup failed: %d\n", ret);
        goto err_regions;
    }

    pci_set_master(pdev);

    edu->regs = pci_iomap(pdev, 0, 0);
    if (!edu->regs) {
        ret = -ENOMEM;
        dev_err(&pdev->dev, "pci_iomap failed\n");
        goto err_master;
    }

    edu->coh_src = dma_alloc_coherent(&pdev->dev, EDU_WINDOW,
                                      &edu->coh_src_dma, GFP_KERNEL);
    edu->coh_dst = dma_alloc_coherent(&pdev->dev, EDU_WINDOW,
                                      &edu->coh_dst_dma, GFP_KERNEL);
    if (!edu->coh_src || !edu->coh_dst) {
        ret = -ENOMEM;
        dev_err(&pdev->dev, "dma_alloc_coherent failed\n");
        goto err_coherent;
    }

    if (ioread32(edu->regs + EDU_IRQ_STATUS))
        iowrite32(ioread32(edu->regs + EDU_IRQ_STATUS),
                  edu->regs + EDU_IRQ_ACK);

    ret = request_irq(pdev->irq, edu_irq_handler, IRQF_SHARED,
                      "edu_dma", edu);
    if (ret) {
        dev_err(&pdev->dev, "request_irq failed: %d\n", ret);
        goto err_coherent;
    }

    edu_selftest(edu);

    edu->proc = proc_create_single_data("dmadriver", 0444, NULL,
                                        edu_proc_show, edu);
    if (!edu->proc) {
        ret = -ENOMEM;
        goto err_irq;
    }

    edu->miscdev.minor = MISC_DYNAMIC_MINOR;
    edu->miscdev.name = "dmadriver";
    edu->miscdev.fops = &edu_fops;
    edu->miscdev.parent = &pdev->dev;

    ret = misc_register(&edu->miscdev);
    if (ret)
        goto err_proc;

    dev_info(&pdev->dev, "EDU DMA driver loaded (irq %d)\n", pdev->irq);
    return 0;

err_proc:
    proc_remove(edu->proc);
err_irq:
    free_irq(pdev->irq, edu);
err_coherent:
    if (edu->coh_dst)
        dma_free_coherent(&pdev->dev, EDU_WINDOW,
                          edu->coh_dst, edu->coh_dst_dma);
    if (edu->coh_src)
        dma_free_coherent(&pdev->dev, EDU_WINDOW,
                          edu->coh_src, edu->coh_src_dma);
    pci_iounmap(pdev, edu->regs);
err_master:
    pci_clear_master(pdev);
err_regions:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
err_free:
    pci_set_drvdata(pdev, NULL);
    kfree(edu);
    return ret;
}

static void edu_remove(struct pci_dev *pdev)
{
    struct edu_dma_device *edu = pci_get_drvdata(pdev);

    misc_deregister(&edu->miscdev);
    proc_remove(edu->proc);

    /* Wait for a transfer in flight, then refuse new ones. */
    mutex_lock(&edu->lock);
    edu->removing = true;
    mutex_unlock(&edu->lock);

    free_irq(pdev->irq, edu);
    dma_free_coherent(&pdev->dev, EDU_WINDOW, edu->coh_dst, edu->coh_dst_dma);
    dma_free_coherent(&pdev->dev, EDU_WINDOW, edu->coh_src, edu->coh_src_dma);
    pci_iounmap(pdev, edu->regs);
    pci_clear_master(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pci_set_drvdata(pdev, NULL);

    dev_info(&pdev->dev, "EDU DMA driver unloaded\n");
    kref_put(&edu->ref, edu_release_dev);
}

static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_pci_driver = {
    .name     = "edu_dma",
    .id_table = edu_ids,
    .probe    = edu_probe,
    .remove   = edu_remove,
};

module_pci_driver(edu_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ravil Fakhretdinov");
MODULE_DESCRIPTION("QEMU EDU DMA driver: coherent, streaming and scatter-gather");
