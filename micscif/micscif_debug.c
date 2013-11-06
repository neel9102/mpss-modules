/*
 * Copyright 2010-2013 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Disclaimer: The codes contained in these modules may be specific to
 * the Intel Software Development Platform codenamed Knights Ferry,
 * and the Intel product codenamed Knights Corner, and are not backward
 * compatible with other Intel products. Additionally, Intel will NOT
 * support the codes or instruction set in future products.
 *
 * Intel offers no warranty of any kind regarding the code. This code is
 * licensed on an "AS IS" basis and Intel is not obligated to provide
 * any support, assistance, installation, training, or other services
 * of any kind. Intel is also not obligated to provide any updates,
 * enhancements or extensions. Intel specifically disclaims any warranty
 * of merchantability, non-infringement, fitness for any particular
 * purpose, and any other warranty.
 *
 * Further, Intel disclaims all liability of any kind, including but
 * not limited to liability for infringement of any proprietary rights,
 * relating to the use of the code, even if Intel is notified of the
 * possibility of such liability. Except as expressly stated in an Intel
 * license agreement provided with this code and agreed upon with Intel,
 * no license, express or implied, by estoppel or otherwise, to any
 * intellectual property rights is granted herein.
 */

#include "mic/micscif.h"
#ifndef _MIC_SCIF_
#include "mic_common.h"
#endif
#include "scif.h"
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>

static char *window_type[] = {
	"NONE",
	"SELF",
	"PEER"};

static char *scifdev_state[] = {
	"SCIFDEV_NOTPRESENT",
	"SCIFDEV_INIT",
	"SCIFDEV_RUNNING",
	"SCIFDEV_SLEEPING",
	"SCIFDEV_STOPPING",
	"SCIFDEV_STOPPED"};

static struct proc_dir_entry *scif_proc;
static struct dentry *mic_debug = NULL;

#define DEBUG_LEN 10

static int scif_ep_show(struct seq_file *m, void *v)
{
	struct endpt *ep;
	struct list_head *pos;
	unsigned long sflags;

	seq_printf(m, "EP Address         State      Port  Peer     Remote Ep Address\n");
	seq_printf(m, "=================================================================\n");
	spin_lock_irqsave(&ms_info.mi_eplock, sflags);
	list_for_each(pos, &ms_info.mi_listen) {
		ep = list_entry(pos, struct endpt, list);
		seq_printf(m, "%p %s %6d\n",
			      ep, scif_ep_states[ep->state], ep->port.port);
	}
	spin_unlock_irqrestore(&ms_info.mi_eplock, sflags);

	spin_lock_irqsave(&ms_info.mi_connlock, sflags);
	list_for_each(pos, &ms_info.mi_connected) {
		ep = list_entry(pos, struct endpt, list);
		seq_printf(m, "%p %s %6d %2d:%-6d %p\n",
			      ep, scif_ep_states[ep->state], ep->port.port, ep->peer.node,
			      ep->peer.port, (void *)ep->remote_ep);
	}
	list_for_each(pos, &ms_info.mi_disconnected) {
		ep = list_entry(pos, struct endpt, list);
		seq_printf(m, "%p %s %6d %2d:%-6d %p\n",
			      ep, scif_ep_states[ep->state], ep->port.port, ep->peer.node,
			      ep->peer.port, (void *)ep->remote_ep);
	}
	spin_unlock_irqrestore(&ms_info.mi_connlock, sflags);

	seq_printf(m,
		"EP Address         State      Port  Peer     Remote Ep Address reg_list "
		"remote_reg_list mmn_list tw_refcount tcw_refcount mi_rma mi_rma_tc "
		"task_list mic_mmu_notif_cleanup\n");
	seq_printf(m, "=================================================================\n");
	spin_lock_irqsave(&ms_info.mi_eplock, sflags);
	list_for_each(pos, &ms_info.mi_zombie) {
		ep = list_entry(pos, struct endpt, list);
		seq_printf(m, "%p %s %6d %2d:%-6d %p %d %d %d %d %d %d %d %d %d\n",
				ep, scif_ep_states[ep->state], ep->port.port, ep->peer.node,
				ep->peer.port, (void *)ep->remote_ep,
				list_empty(&ep->rma_info.reg_list),
				list_empty(&ep->rma_info.remote_reg_list),
				list_empty(&ep->rma_info.mmn_list),
				atomic_read(&ep->rma_info.tw_refcount),
				atomic_read(&ep->rma_info.tcw_refcount),
				list_empty(&ms_info.mi_rma),
				list_empty(&ms_info.mi_rma_tc),
				list_empty(&ep->rma_info.task_list),
#ifdef CONFIG_MMU_NOTIFIER
				list_empty(&ms_info.mi_mmu_notif_cleanup)
#else
				-1
#endif
			    );
	}
	spin_unlock_irqrestore(&ms_info.mi_eplock, sflags);

	return 0;
}

static int scif_ep_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_ep_show, PDE_DATA(inode));
}

static const struct file_operations scif_ep_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_ep_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_rma_window_show(struct seq_file *m, void *v)
{
	struct endpt *ep;
	struct list_head *pos, *item, *tmp;
	unsigned long sflags;
	struct reg_range_t *window;

	seq_printf(m, "SCIF Connected EP RMA Window Info\n");
	seq_printf(m, "=================================================================\n");
	seq_printf(m, "%-16s\t%-16s %-16s %-16s %-8s %-8s %-8s\n",
		      "Endpoint", "Type", "Offset", "NumPages", "Prot", "Ref_Count", "Unreg State");
	spin_lock_irqsave(&ms_info.mi_connlock, sflags);
	list_for_each(pos, &ms_info.mi_connected) {
		ep = list_entry(pos, struct endpt, list);
		if (mutex_trylock(&ep->rma_info.rma_lock)) {
			list_for_each_safe(item, tmp, &ep->rma_info.reg_list) {
				window = list_entry(item, struct reg_range_t, list_member);
				seq_printf(m,
					"%-16p\t%-16s 0x%-16llx %-16lld %-8d %-8d %-8d\n",
					ep, window_type[window->type], window->offset,
					window->nr_pages, window->prot, window->ref_count,
					window->unreg_state);
			}
			list_for_each_safe(item, tmp, &ep->rma_info.remote_reg_list) {
				window = list_entry(item, struct reg_range_t, list_member);
				seq_printf(m,
					"%-16p\t%-16s 0x%-16llx %-16lld %-8d %-8d %-8d\n",
					ep, window_type[window->type], window->offset,
					window->nr_pages, window->prot, window->ref_count,
					window->unreg_state);
			}
			mutex_unlock(&ep->rma_info.rma_lock);
		} else
			seq_printf(m, "Try Again, some other thread has the RMA lock for ep %p\n", ep);
	}
	spin_unlock_irqrestore(&ms_info.mi_connlock, sflags);

	seq_printf(m, "=================================================================\n");
	seq_printf(m, "SCIF Zombie EP RMA Window Info\n");
	spin_lock_irqsave(&ms_info.mi_eplock, sflags);
	list_for_each(pos, &ms_info.mi_zombie) {
		ep = list_entry(pos, struct endpt, list);
		if (mutex_trylock(&ep->rma_info.rma_lock)) {
			list_for_each_safe(item, tmp, &ep->rma_info.reg_list) {
				window = list_entry(item, struct reg_range_t, list_member);
				seq_printf(m,
					"%-16p\t%-16s 0x%-16llx %-16lld %-8d %-8d %-8d\n",
					ep, window_type[window->type], window->offset,
					window->nr_pages, window->prot, window->ref_count,
					window->unreg_state);
			}
			list_for_each_safe(item, tmp, &ep->rma_info.remote_reg_list) {
				window = list_entry(item, struct reg_range_t, list_member);
				seq_printf(m,
					"%-16p\t%-16s 0x%-16llx %-16lld %-8d %-8d %-8d\n",
					ep, window_type[window->type], window->offset,
					window->nr_pages, window->prot, window->ref_count,
					window->unreg_state);
			}
			mutex_unlock(&ep->rma_info.rma_lock);
		} else
			seq_printf(m, "Try Again, some other thread has the RMA lock for ep %p\n", ep);
	}
	spin_unlock_irqrestore(&ms_info.mi_eplock, sflags);
	seq_printf(m, "=================================================================\n");
	seq_printf(m, "%-16s\t%-16s %-16s %-16s %-8s %-8s %-8s\n",
			"Endpoint", "Type", "Offset", "NumPages", "Prot", "Ref_Count", "Unreg State");
	spin_lock(&ms_info.mi_rmalock);
	list_for_each_safe(item, tmp, &ms_info.mi_rma) {
		window = list_entry(item, 
				struct reg_range_t, list_member);
		ep = (struct endpt *)window->ep;
		seq_printf(m,
			"%-16p\t%-16s 0x%-16llx %-16lld %-8d %-8d %-8d\n",
			ep, window_type[window->type], window->offset,
			window->nr_pages, window->prot, window->ref_count,
			window->unreg_state);
	}
	spin_unlock(&ms_info.mi_rmalock);

	return 0;
}

static int scif_rma_window_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_rma_window_show, PDE_DATA(inode));
}

static const struct file_operations scif_rma_window_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_rma_window_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_rma_xfer_show(struct seq_file *m, void *v)
{
	struct endpt *ep;
	struct list_head *pos;
	unsigned long sflags;

	seq_printf(m, "SCIF RMA Debug\n");
	seq_printf(m, "=================================================================\n");
	seq_printf(m, "%-16s\t %-16s %-16s %-16s\n",
		      "Endpoint", "Fence Ref Count", "Temp Window Ref Count", "DMA CHANNEL");
	spin_lock_irqsave(&ms_info.mi_connlock, sflags);
	list_for_each(pos, &ms_info.mi_connected) {
		ep = list_entry(pos, struct endpt, list);
		seq_printf(m, "%-16p\t%-16d %-16d %-16d\n",
			ep, ep->rma_info.fence_refcount,
			atomic_read(&ep->rma_info.tw_refcount),
			ep->rma_info.dma_chan ? get_chan_num(ep->rma_info.dma_chan): -1);
	}
	spin_unlock_irqrestore(&ms_info.mi_connlock, sflags);

	return 0;
}

static int scif_rma_xfer_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_rma_xfer_show, PDE_DATA(inode));
}

static const struct file_operations scif_rma_xfer_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_rma_xfer_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

/* Place Holder for generic SCIF debug information */
static int scif_debug_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Num gtt_entries %d\n", ms_info.nr_gtt_entries);
	/*
	 * Tracking the number of zombies for debug.
	 * Need to make sure they are not being left behind forever.
	 */
	seq_printf(m, "Num Zombie Endpoints %d\n", ms_info.mi_nr_zombies);

	seq_printf(m, "Watchdog timeout %d\n", ms_info.mi_watchdog_to);

	seq_printf(m, "Watchdog enabled %d\n", ms_info.mi_watchdog_enabled);

	seq_printf(m, "Watchdog auto reboot %d\n", ms_info.mi_watchdog_auto_reboot);

	seq_printf(m, "Huge Pages Enabled %d Detected 2mb %lld 4k %lld\n",
		mic_huge_page_enable, ms_info.nr_2mb_pages, ms_info.nr_4k_pages);
#ifdef RMA_DEBUG
	seq_printf(m,
		"rma_alloc_cnt %ld rma_pin_cnt %ld mmu_notif %ld rma_unaligned_cpu_cnt %ld\n",
		atomic_long_read(&ms_info.rma_alloc_cnt),
		atomic_long_read(&ms_info.rma_pin_cnt),
		atomic_long_read(&ms_info.mmu_notif_cnt),
		atomic_long_read(&ms_info.rma_unaligned_cpu_cnt));
#endif
	seq_printf(m,
		"List empty? mi_uaccept %d mi_listen %d mi_zombie %d "
		"mi_connected %d mi_disconnected %d\n",
		list_empty(&ms_info.mi_uaccept),
		list_empty(&ms_info.mi_listen),
		list_empty(&ms_info.mi_zombie),
		list_empty(&ms_info.mi_connected),
		list_empty(&ms_info.mi_disconnected));

	return 0;
}

static int scif_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_debug_show, PDE_DATA(inode));
}

static const struct file_operations scif_debug_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_debug_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_dev_info_show(struct seq_file *m, void *v)
{
	int node;

#ifdef _MIC_SCIF_
	micscif_get_node_info();

	mutex_lock(&ms_info.mi_conflock);
#endif
	seq_printf(m, "Total Nodes %d Self Node Id %d Maxid %d\n",
		ms_info.mi_total, ms_info.mi_nodeid, ms_info.mi_maxid);

	seq_printf(m, "%-16s\t%-16s %-16s\t%-16s\t%-8s\t%-8s\t%-8s\n",
		"node_id", "state", "scif_ref_cnt", "scif_map_ref_cnt",
		"wait_status", "conn count", "numa_node");

	for (node = 0; node <= ms_info.mi_maxid; node++)
		seq_printf(m, "%-16d\t%-16s\t0x%-16lx\t%-16d\t%-16lld\t%-16d\t%-16d\n",
			scif_dev[node].sd_node, scifdev_state[scif_dev[node].sd_state],
			atomic_long_read(&scif_dev[node].scif_ref_cnt),
			scif_dev[node].scif_map_ref_cnt,
			scif_dev[node].sd_wait_status,
			scif_dev[node].num_active_conn,
			scif_dev[node].sd_numa_node);
#ifdef _MIC_SCIF_
	mutex_unlock(&ms_info.mi_conflock);
#endif

	return 0;
}

static int scif_dev_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_dev_info_show, PDE_DATA(inode));
}

static const struct file_operations scif_dev_info_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_dev_info_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

#ifdef _MIC_SCIF_
static int scif_suspend_show(struct seq_file *m, void *v)
{
	micscif_suspend_handler(NULL, 0, NULL);
	return 0;
}

static int scif_suspend_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_suspend_show, PDE_DATA(inode));
}

static const struct file_operations scif_suspend_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_suspend_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_crash_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s %d Crash the Card to test Lost Nodes\n", __func__, __LINE__);
	panic("Test Lost Node! Crash the card intentionally\n");
	return 0;
}

static int scif_crash_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_crash_show, PDE_DATA(inode));
}

static const struct file_operations scif_crash_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_crash_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_bugon_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s %d Bug on the Card to test Lost Nodes\n", __func__, __LINE__);
	BUG_ON(1);
	return 0;
}

static int scif_bugon_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_bugon_show, PDE_DATA(inode));
}

static const struct file_operations scif_bugon_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_bugon_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_fail_suspend_show(struct seq_file *m, void *v)
{
	micscif_fail_suspend_handler(NULL, 0, NULL);
	seq_printf(m, "Failing Suspend\n");
	return 0;
}

static int scif_fail_suspend_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_fail_suspend_show, PDE_DATA(inode));
}

static const struct file_operations scif_fail_suspend_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_fail_suspend_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int scif_resume_show(struct seq_file *m, void *v)
{
	micscif_resume_handler(NULL, 0, NULL);
	seq_printf(m, "Resuming/Waking up node\n");
	return 0;
}

static int scif_resume_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_resume_show, PDE_DATA(inode));
}

static const struct file_operations scif_resume_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_resume_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};
#endif

static int scif_reg_cache_limit_show(struct seq_file *m, void *v)
{
	seq_printf(m, "reg_cache_limit = 0x%lx\n", ms_info.mi_rma_tc_limit);
	return 0;
}

static int scif_reg_cache_limit_open(struct inode *inode, struct file *file)
{
	return single_open(file, scif_reg_cache_limit_show, PDE_DATA(inode));
}

static ssize_t
scif_reg_cache_limit_write(struct file *file, const char __user *buffer,
					size_t len, loff_t *unused)
{
	unsigned long data = 0;
	char *p;
	if (!(p = kzalloc(len, GFP_KERNEL)))
		return -ENOMEM;
	if (copy_from_user(p, buffer, len))
		return -EFAULT;
	data = simple_strtoul(p, NULL, 0);
	ms_info.mi_rma_tc_limit = data;
	return len;
}

static const struct file_operations scif_reg_cache_limit_fops = {
	.owner   = THIS_MODULE,
	.open    = scif_reg_cache_limit_open,
	.write   = scif_reg_cache_limit_write,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

#ifdef _MIC_SCIF_
static int smpt_seq_show(struct seq_file *s, void *pos)
{
	volatile uint8_t *mm_sbox = scif_dev[SCIF_HOST_NODE].mm_sbox;
	uint32_t smpt_reg_offset = SBOX_SMPT00;
	uint32_t smpt_reg_val;
	int i;

	seq_printf(s,
		"=================================================================\n");
	seq_printf(s,"%-11s| %-15s %-14s %-5s \n",
		"SMPT entry", "SMPT reg value", "DMA addr", "SNOOP");
	seq_printf(s,
		"=================================================================\n");

	for (i = 0; i < NUM_SMPT_ENTRIES_IN_USE; i++) {
		smpt_reg_val = readl(mm_sbox + smpt_reg_offset);
		seq_printf(s,"%-11d| %-#15x %-#14llx %-5s \n",
			i, smpt_reg_val, ((uint64_t)smpt_reg_val >> 2ULL) << MIC_SYSTEM_PAGE_SHIFT,
			(smpt_reg_val & 0x1) ? "OFF" : "ON");
		smpt_reg_offset += 4;
	}

	seq_printf(s,
		"=================================================================\n");
	return 0;
}

#else
static int smpt_seq_show(struct seq_file *s, void *pos)
{
	uint64_t bid = (uint64_t)s->private;
	mic_ctx_t *mic_ctx;
	int i;
	unsigned long flags;

	mic_ctx = get_per_dev_ctx(bid);
	seq_printf(s,
		"=================================================================\n");
	seq_printf(s,"Board %-2d |%-10s| %-14s %-10s \n",
		(int)bid + 1, "SMPT entry", "DMA addr", "Reference Count");
	seq_printf(s,
		"=================================================================\n");

	if (mic_ctx && mic_ctx->mic_smpt) {
		spin_lock_irqsave(&mic_ctx->smpt_lock, flags);
		for (i = 0; i < NUM_SMPT_ENTRIES_IN_USE; i++) {
			seq_printf(s,"%9s|%-10d| %-#14llx %-10lld \n",
			" ",  i, mic_ctx->mic_smpt[i].dma_addr, mic_ctx->mic_smpt[i].ref_count);
		}
		spin_unlock_irqrestore(&mic_ctx->smpt_lock, flags);
	}

	seq_printf(s,
		"================================================================X\n");
	return 0;
}
#endif

static int smpt_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, smpt_seq_show, inode->i_private);
}

static int smpt_debug_release(struct inode *inode, struct file *file)
{
	return single_release(inode, file);
}

static struct file_operations smpt_file_ops = {
	.owner   = THIS_MODULE,
	.open    = smpt_debug_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = smpt_debug_release
};

#ifndef _MIC_SCIF_
static int log_buf_seq_show(struct seq_file *s, void *pos)
{
	uint64_t bid = (uint64_t)s->private;
	mic_ctx_t *mic_ctx;
	void *log_buf_len_va, *log_buf_va;
	struct micscif_dev *dev;

	mic_ctx = get_per_dev_ctx(bid);
	if (!mic_ctx || !mic_ctx->log_buf_addr || !mic_ctx->log_buf_len)
		goto done;

	if (mic_ctx->bi_family == FAMILY_ABR) {
		seq_printf(s, "log buffer display not supported for KNF\n");
		goto done;
	}

	dev = &scif_dev[mic_get_scifnode_id(mic_ctx)];
	log_buf_len_va = virt_to_phys(mic_ctx->log_buf_len) + mic_ctx->aper.va;
	log_buf_va = virt_to_phys(mic_ctx->log_buf_addr) + mic_ctx->aper.va;

	mutex_lock(&mic_ctx->state_lock);
	switch (mic_ctx->state) {
	case MIC_BOOT:
	case MIC_BOOTFAIL:
	case MIC_ONLINE:
	case MIC_SHUTDOWN:
	case MIC_LOST:
		micscif_inc_node_refcnt(dev, 1);
		seq_write(s, log_buf_va, *(int*)log_buf_len_va);
		micscif_dec_node_refcnt(dev, 1);
		break;
	case MIC_NORESPONSE:
	case MIC_READY:
	/* Cannot access GDDR while reset is ongoing */
	case MIC_RESET:
	case MIC_RESETFAIL:
	case MIC_INVALID:
	default:
		break;
	}
	mutex_unlock(&mic_ctx->state_lock);
done:
	return 0;
}

static int log_buf_open(struct inode *inode, struct file *file)
{
	return single_open(file, log_buf_seq_show, inode->i_private);
}

static int log_buf_release(struct inode *inode, struct file *file)
{
	return single_release(inode, file);
}

static struct file_operations log_buf_ops = {
	.owner   = THIS_MODULE,
	.open    = log_buf_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = log_buf_release
};
#endif

void
scif_proc_init(void)
{
	if ((scif_proc = proc_mkdir("scif", NULL)) != NULL) {
		proc_create("ep", 0444, scif_proc, &scif_ep_fops);
		proc_create("rma_window", 0444, scif_proc, &scif_rma_window_fops);
		proc_create("rma_xfer", 0444, scif_proc, &scif_rma_xfer_fops);
		proc_create("scif_dev", 0444, scif_proc, &scif_dev_info_fops);
		proc_create("debug", 0444, scif_proc, &scif_debug_fops);
#ifdef _MIC_SCIF_
		proc_create("suspend", 0444, scif_proc, &scif_suspend_fops);
		proc_create("fail_suspend", 0444, scif_proc, &scif_fail_suspend_fops);
		proc_create("resume", 0444, scif_proc, &scif_resume_fops);
		proc_create("crash", 0444, scif_proc, &scif_crash_fops);
		proc_create("bugon", 0444, scif_proc, &scif_bugon_fops);
#endif
		proc_create("reg_cache_limit", S_IFREG | S_IRUGO | S_IWUGO, scif_proc, &scif_reg_cache_limit_fops);
	}
}

#ifdef _MIC_SCIF_
void
mic_debug_init(void)
{
	if ((mic_debug = debugfs_create_dir("mic_debug", NULL))) {
		debugfs_create_file("smpt", 0444, mic_debug, NULL, &smpt_file_ops);
		debugfs_create_u8("enable_msg_logging", 0666, mic_debug, &(ms_info.en_msg_log));
	}
}
#else
void
mic_debug_init(mic_ctx_t *mic_ctx)
{
	char name[DEBUG_LEN];
	uint64_t id = mic_ctx->bi_id;
	struct dentry *child;

	if (!mic_debug)
		mic_debug = debugfs_create_dir("mic_debug", NULL);

	if (mic_debug) {
		snprintf(name, DEBUG_LEN, "mic%d", (int)id);
		if ((child = debugfs_create_dir(name, mic_debug))) {
			debugfs_create_file("smpt", 0444, child, (void*)id, &smpt_file_ops);
			debugfs_create_file("log_buf", 0444, child, (void*)id, &log_buf_ops);
		}
		debugfs_create_u8("enable_msg_logging", 0666, mic_debug, &(ms_info.en_msg_log));
	}
}
#endif

void
mic_debug_uninit(void)
{
	debugfs_remove_recursive(mic_debug);
}

void
scif_proc_cleanup(void)
{
	if (scif_proc) {
		remove_proc_entry("reg_cache_limit", scif_proc);
		remove_proc_entry("ep", scif_proc);
		remove_proc_entry("rma_window", scif_proc);
		remove_proc_entry("rma_xfer", scif_proc);
		remove_proc_entry("scif_dev", scif_proc);
		remove_proc_entry("debug", scif_proc);
#ifdef _MIC_SCIF_
		remove_proc_entry("suspend", scif_proc);
		remove_proc_entry("fail_suspend", scif_proc);
		remove_proc_entry("resume", scif_proc);
		remove_proc_entry("crash", scif_proc);
		remove_proc_entry("bugon", scif_proc);
#endif
		remove_proc_entry("scif", NULL);
		scif_proc = NULL;
	}
}

#ifdef _MIC_SCIF_
extern int micscif_max_msg_id;

/*
 * Test entry point for error injection
 */
int
micscif_error_inject(int scenario)
{
	switch (scenario) {
	case 1:
		micscif_max_msg_id = 0;
		break;
	default:
		pr_debug("Illegal error injection scenario %d\n", scenario);
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(micscif_error_inject);
#endif // _MIC_SCIF_
