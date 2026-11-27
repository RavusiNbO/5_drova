#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x6152e9dd, "__pci_register_driver" },
	{ 0xfcdaedac, "seq_printf" },
	{ 0x2d9e7732, "pci_unregister_driver" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0x296695f, "refcount_warn_saturate" },
	{ 0x37a0cba, "kfree" },
	{ 0x2355eeed, "misc_deregister" },
	{ 0x84cf81e7, "proc_remove" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0xc1514a3b, "free_irq" },
	{ 0x9eb609b3, "dma_free_attrs" },
	{ 0x8585aa4a, "pci_iounmap" },
	{ 0xe25a5073, "pci_clear_master" },
	{ 0x34e89459, "pci_release_regions" },
	{ 0xd66ea019, "pci_disable_device" },
	{ 0x90e419e9, "_dev_info" },
	{ 0xa6257a2f, "complete" },
	{ 0x726bc3c7, "wait_for_completion_killable_timeout" },
	{ 0x15ba50a6, "jiffies" },
	{ 0xf9a482f9, "msleep" },
	{ 0xb6104fed, "_dev_err" },
	{ 0xb43f9365, "ktime_get" },
	{ 0xe979607b, "kmalloc_caches" },
	{ 0x4ba488d1, "__kmalloc_cache_noprof" },
	{ 0x608741b5, "__init_swait_queue_head" },
	{ 0xcefb0c9f, "__mutex_init" },
	{ 0xd1031cb4, "pci_enable_device" },
	{ 0x7f37c573, "pci_request_regions" },
	{ 0x682ccc39, "dma_set_mask" },
	{ 0x6a222a79, "dma_set_coherent_mask" },
	{ 0x4a46d0e6, "pci_set_master" },
	{ 0xaf960d6e, "pci_iomap" },
	{ 0x5acb39b2, "dma_alloc_attrs" },
	{ 0x92d5838e, "request_threaded_irq" },
	{ 0xdcb764ad, "memset" },
	{ 0x449ad0a7, "memcmp" },
	{ 0x55faeaa5, "proc_create_single_data" },
	{ 0xd546a9a3, "_dev_warn" },
	{ 0xdcb4184, "misc_register" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0x89940875, "mutex_lock_interruptible" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0x52c5c991, "__kmalloc_noprof" },
	{ 0x4829a47e, "memcpy" },
	{ 0x2cf0c910, "sg_init_table" },
	{ 0xd03cd873, "alloc_pages_noprof" },
	{ 0xe663541b, "dma_map_sg_attrs" },
	{ 0xfbe215e4, "sg_next" },
	{ 0xe0f0ae0, "dma_unmap_sg_attrs" },
	{ 0xea5e9dc5, "__free_pages" },
	{ 0xc31db0ce, "is_vmalloc_addr" },
	{ 0x3b362729, "dma_map_page_attrs" },
	{ 0x2f31bb17, "dma_unmap_page_attrs" },
	{ 0xc4804d28, "__dma_sync_single_for_cpu" },
	{ 0xc1cb797, "__dma_sync_single_for_device" },
	{ 0x16197235, "dev_driver_string" },
	{ 0x56470118, "__warn_printk" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x75ca79b5, "__fortify_panic" },
	{ 0x9850a0ff, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("pci:v00001234d000011E8sv*sd*bc*sc*i*");
