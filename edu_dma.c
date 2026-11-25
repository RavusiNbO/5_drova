// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU EDU Scatter-Gather DMA example, variant 5.
 *
 * The QEMU EDU device has no SG descriptor engine. We therefore map
 * non-contiguous RAM pages with dma_map_sg(), then transfer the mapped
 * ranges sequentially through EDU's 4 KiB internal DMA buffer.
 *
 * Tested target assumptions:
 *   PCI ID:       1234:11e8
 *   BAR0:         MMIO
 *   IRQ status:   0x24
 *   IRQ ACK:      0x64
 *   DMA SRC:      0x80 (4-byte accesses; 28-bit DMA mask)
 *   DMA DST:      0x88
 *   DMA COUNT:    0x90
 *   DMA CMD:      0x98
 *   EDU DMA RAM:  device-side offset 0x40000, size 4096
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/compat.h>
#include <linux/limits.h>

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

#define EDU_IRQ_DMA             0x100
#define EDU_DMA_START           0x01
#define EDU_DMA_TO_RAM          0x02
#define EDU_DMA_IRQ             0x04

#define EDU_DMA_TIMEOUT_MS      5000
#define EDU_MAX_SIZE            (16U * 1024U * 1024U)
#define EDU_MAX_SEGMENTS        4096U

struct edu_dma_device {
    struct pci_dev *pdev;
    void __iomem *regs;
    struct completion dma_done;
    struct mutex lock;
    struct miscdevice miscdev;
    u64 transfers;
    u64 total_bytes;
};

struct dma_piece {
    dma_addr_t addr;
    u32 len;
};

static struct edu_dma_device *file_to_edu(struct file *file)
{
    return container_of(file->private_data,
                        struct edu_dma_device, miscdev);
}

static irqreturn_t edu_irq_handler(int irq, void *data)
{
    struct edu_dma_device *edu = data;
    u32 status;

    status = ioread32(edu->regs + EDU_IRQ_STATUS);
    if (!(status & EDU_IRQ_DMA))
        return IRQ_NONE;

    /* ACK the bit reported by the device. */
    iowrite32(status & EDU_IRQ_DMA, edu->regs + EDU_IRQ_ACK);
    complete(&edu->dma_done);

    return IRQ_HANDLED;
}

/*
 * Start one EDU DMA operation:
 *   from_edu == false: host RAM -> EDU internal buffer
 *   from_edu == true:  EDU internal buffer -> host RAM
 *
 * EDU's DMA address registers accept 32-bit writes. probe() limits the
 * DMA mask to 28 bits, matching the QEMU EDU device's default capability.
 */
static int edu_dma_one(struct edu_dma_device *edu,
                       dma_addr_t ram_dma, u32 len, bool from_edu)
{
    unsigned long waited;
    u32 irq_status;

    if (!len || len > EDU_DMA_BUFFER_SIZE)
        return -EINVAL;

    reinit_completion(&edu->dma_done);

    /* Remove any stale DMA IRQ before starting the next operation. */
    irq_status = ioread32(edu->regs + EDU_IRQ_STATUS);
    if (irq_status & EDU_IRQ_DMA)
        iowrite32(EDU_IRQ_DMA, edu->regs + EDU_IRQ_ACK);

    if (!from_edu) {
        /* RAM -> EDU */
        iowrite32(lower_32_bits(ram_dma), edu->regs + EDU_DMA_SRC);
        iowrite32(EDU_DMA_BUFFER, edu->regs + EDU_DMA_DST);
    } else {
        /* EDU -> RAM */
        iowrite32(EDU_DMA_BUFFER, edu->regs + EDU_DMA_SRC);
        iowrite32(lower_32_bits(ram_dma), edu->regs + EDU_DMA_DST);
    }

    iowrite32(len, edu->regs + EDU_DMA_COUNT);

    iowrite32(EDU_DMA_START | EDU_DMA_IRQ |
              (from_edu ? EDU_DMA_TO_RAM : 0),
              edu->regs + EDU_DMA_CMD);

    waited = wait_for_completion_timeout(
        &edu->dma_done, msecs_to_jiffies(EDU_DMA_TIMEOUT_MS));

    if (!waited)
        return -ETIMEDOUT;

    /* Completion IRQ should be raised only after EDU clears START. */
    if (ioread32(edu->regs + EDU_DMA_CMD) & EDU_DMA_START)
        return -EIO;

    return 0;
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

static void edu_free_pages(struct page **pages, u32 nents)
{
    u32 i;

    if (!pages)
        return;

    for (i = 0; i < nents; i++) {
        if (pages[i])
            __free_page(pages[i]);
    }
    kfree(pages);
}

static u8 edu_pattern(u32 offset)
{
    return (u8)((offset * 131U + 17U) & 0xff);
}

static void edu_fill_source(struct page **pages, u32 nents, u32 size)
{
    u32 pos = 0;
    u32 i;

    for (i = 0; i < nents && pos < size; i++) {
        u32 len = min_t(u32, PAGE_SIZE, size - pos);
        u8 *p = kmap_local_page(pages[i]);
        u32 j;

        for (j = 0; j < len; j++)
            p[j] = edu_pattern(pos + j);

        kunmap_local(p);
        pos += len;
    }
}

static bool edu_verify_pages(struct page **src_pages,
                             struct page **dst_pages,
                             u32 nents, u32 size)
{
    u32 pos = 0;
    u32 i;

    for (i = 0; i < nents && pos < size; i++) {
        u32 len = min_t(u32, PAGE_SIZE, size - pos);
        u8 *src = kmap_local_page(src_pages[i]);
        u8 *dst = kmap_local_page(dst_pages[i]);
        bool equal = true;
        u32 j;

        for (j = 0; j < len; j++) {
            if (src[j] != dst[j]) {
                equal = false;
                break;
            }
        }

        kunmap_local(dst);
        kunmap_local(src);

        if (!equal)
            return false;

        pos += len;
    }

    return pos == size;
}

/*
 * Walk two independently mapped SG lists as byte streams.
 * This is important: dma_map_sg() may merge the original entries, so
 * source and destination mapped segment boundaries need not match.
 */
static int edu_copy_sg_streams(struct edu_dma_device *edu,
                               struct dma_piece *src, int src_nents,
                               struct dma_piece *dst, int dst_nents,
                               u32 total_size)
{
    int si = 0, di = 0;
    u32 so = 0, doff = 0;
    u32 done = 0;

    while (done < total_size) {
        u32 src_left, dst_left, len;
        dma_addr_t src_addr, dst_addr;
        int ret;

        if (si >= src_nents || di >= dst_nents)
            return -EIO;

        src_left = src[si].len - so;
        dst_left = dst[di].len - doff;

        len = min(src_left, dst_left);
        len = min_t(u32, len, EDU_DMA_BUFFER_SIZE);
        len = min(len, total_size - done);

        src_addr = src[si].addr + so;
        dst_addr = dst[di].addr + doff;

        ret = edu_dma_one(edu, src_addr, len, false);
        if (ret)
            return ret;

        ret = edu_dma_one(edu, dst_addr, len, true);
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

static int edu_dma_transfer(struct edu_dma_device *edu,
                            struct edu_dma_request *req)
{
    struct page **src_pages = NULL, **dst_pages = NULL;
    struct scatterlist *src_sg = NULL, *dst_sg = NULL;
    struct dma_piece *src_pieces = NULL, *dst_pieces = NULL;
    int src_mapped = 0, dst_mapped = 0;
    bool src_is_mapped = false, dst_is_mapped = false;
    u32 n = req->segments;
    u32 size = req->size;
    u32 i, pos = 0;
    u64 start_ns;
    int ret = 0;

    req->mapped_src = 0;
    req->mapped_dst = 0;
    req->elapsed_ns = 0;
    req->verified = 0;

    if (!size || size > EDU_MAX_SIZE ||
        !n || n > EDU_MAX_SEGMENTS ||
        n > size || n < DIV_ROUND_UP(size, PAGE_SIZE))
        return -EINVAL;

    src_pages = kcalloc(n, sizeof(*src_pages), GFP_KERNEL);
    dst_pages = kcalloc(n, sizeof(*dst_pages), GFP_KERNEL);
    src_sg = kcalloc(n, sizeof(*src_sg), GFP_KERNEL);
    dst_sg = kcalloc(n, sizeof(*dst_sg), GFP_KERNEL);
    src_pieces = kcalloc(n, sizeof(*src_pieces), GFP_KERNEL);
    dst_pieces = kcalloc(n, sizeof(*dst_pieces), GFP_KERNEL);

    if (!src_pages || !dst_pages || !src_sg || !dst_sg ||
        !src_pieces || !dst_pieces) {
        ret = -ENOMEM;
        goto out;
    }

    sg_init_table(src_sg, n);
    sg_init_table(dst_sg, n);

    /*
     * Allocate separate pages. Each page is one original SG entry.
     * Distribute the requested byte count over the requested pages.
     */
    for (i = 0; i < n; i++) {
        u32 remaining_entries = n - i;
        u32 remaining_bytes = size - pos;
        u32 len = DIV_ROUND_UP(remaining_bytes, remaining_entries);

        len = min_t(u32, len, PAGE_SIZE);

        src_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
        dst_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!src_pages[i] || !dst_pages[i]) {
            ret = -ENOMEM;
            goto out;
        }

        sg_set_page(&src_sg[i], src_pages[i], len, 0);
        sg_set_page(&dst_sg[i], dst_pages[i], len, 0);
        pos += len;
    }

    if (pos != size) {
        ret = -EINVAL;
        goto out;
    }

    edu_fill_source(src_pages, n, size);

    src_mapped = dma_map_sg(&edu->pdev->dev, src_sg, n, DMA_TO_DEVICE);
    if (!src_mapped) {
        ret = -EIO;
        goto out;
    }
    src_is_mapped = true;

    dst_mapped = dma_map_sg(&edu->pdev->dev, dst_sg, n, DMA_FROM_DEVICE);
    if (!dst_mapped) {
        ret = -EIO;
        goto out;
    }
    dst_is_mapped = true;

    req->mapped_src = src_mapped;
    req->mapped_dst = dst_mapped;

    edu_collect_pieces(src_sg, src_mapped, src_pieces);
    edu_collect_pieces(dst_sg, dst_mapped, dst_pieces);

    start_ns = ktime_get_ns();

    ret = edu_copy_sg_streams(edu, src_pieces, src_mapped,
                              dst_pieces, dst_mapped, size);
    req->elapsed_ns = ktime_get_ns() - start_ns;
    if (ret)
        goto out;

    /*
     * Unmap before CPU reads the DMA_FROM_DEVICE buffer. The DMA API
     * performs the required ownership/cache transition.
     */
    dma_unmap_sg(&edu->pdev->dev, dst_sg, n, DMA_FROM_DEVICE);
    dst_is_mapped = false;

    dma_unmap_sg(&edu->pdev->dev, src_sg, n, DMA_TO_DEVICE);
    src_is_mapped = false;

    req->verified = edu_verify_pages(src_pages, dst_pages, n, size);
    if (!req->verified) {
        ret = -EIO;
        goto out;
    }

    edu->transfers++;
    edu->total_bytes += size;

out:
    if (dst_is_mapped)
        dma_unmap_sg(&edu->pdev->dev, dst_sg, n, DMA_FROM_DEVICE);
    if (src_is_mapped)
        dma_unmap_sg(&edu->pdev->dev, src_sg, n, DMA_TO_DEVICE);

    edu_free_pages(src_pages, n);
    edu_free_pages(dst_pages, n);
    kfree(src_sg);
    kfree(dst_sg);
    kfree(src_pieces);
    kfree(dst_pieces);

    return ret;
}

static long edu_ioctl(struct file *file, unsigned int cmd,
                      unsigned long arg)
{
    struct edu_dma_device *edu = file_to_edu(file);
    struct edu_dma_request req;
    int ret;

    if (cmd != EDU_DMA_TRANSFER)
        return -ENOTTY;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (mutex_lock_interruptible(&edu->lock))
        return -ERESTARTSYS;

    ret = edu_dma_transfer(edu, &req);

    mutex_unlock(&edu->lock);

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;

    return ret;
}

static const struct file_operations edu_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = edu_ioctl
};

static int edu_probe(struct pci_dev *pdev,
                     const struct pci_device_id *id)
{
    struct edu_dma_device *edu;
    int ret;
    u32 status;

    edu = kzalloc(sizeof(*edu), GFP_KERNEL);
    if (!edu)
        return -ENOMEM;

    edu->pdev = pdev;
    init_completion(&edu->dma_done);
    mutex_init(&edu->lock);
    pci_set_drvdata(pdev, edu);

    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free;

    ret = pci_request_regions(pdev, "edu_dma");
    if (ret)
        goto err_disable;

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
    if (ret)
        goto err_regions;

    pci_set_master(pdev);

    edu->regs = pci_iomap(pdev, 0, 0);
    if (!edu->regs) {
        ret = -ENOMEM;
        goto err_master;
    }

    status = ioread32(edu->regs + EDU_IRQ_STATUS);
    if (status)
        iowrite32(status, edu->regs + EDU_IRQ_ACK);

    ret = request_irq(pdev->irq, edu_irq_handler, IRQF_SHARED,
                      "edu_dma", edu);
    if (ret)
        goto err_iounmap;

    edu->miscdev.minor = MISC_DYNAMIC_MINOR;
    edu->miscdev.name = "dmadriver";
    edu->miscdev.fops = &edu_fops;
    edu->miscdev.parent = &pdev->dev;

    ret = misc_register(&edu->miscdev);
    if (ret)
        goto err_irq;

    dev_info(&pdev->dev, "EDU DMA driver loaded\n");
    return 0;

err_irq:
    free_irq(pdev->irq, edu);
err_iounmap:
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
    free_irq(pdev->irq, edu);
    pci_iounmap(pdev, edu->regs);
    pci_clear_master(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pci_set_drvdata(pdev, NULL);
    kfree(edu);

    dev_info(&pdev->dev, "EDU DMA driver unloaded\n");
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
MODULE_DESCRIPTION("QEMU EDU Scatter-Gather DMA driver");
