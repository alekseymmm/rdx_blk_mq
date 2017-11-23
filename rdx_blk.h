/*
 * rdx_blk.h
 *
 *  Created on: 3 окт. 2017 г.
 *      Author: alekseym
 */

#ifndef RDX_BLK_H_
#define RDX_BLK_H_

#include <linux/module.h>

#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/bitmap.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>

#define KERNEL_SECT_SIZE_SHIFT 9
#define KERNEL_SECT_SIZE (1 << KERNEL_SECT_SIZE_SHIFT)
#define RDX_BLK_MIN_POOL_PAGES 128
#define MSB_DEFAULT_RANGE_SIZE_SECTORS (20 * 1024 * 2)
#define MSB_DEFAULT_MAX_NUM_EVICT_CMD (8)
#define MSB_BLOCK_SIZE_SECTORS (8)

extern struct rdx_blk *rdx_blk;

struct rdx_blk_cmd{
	struct list_head list;
	struct llist_node ll_list;
	struct call_single_data csd;
	struct request *rq;
	struct bio *bio;
	unsigned int tag;
	struct rdx_blk_queue *rdx_blk_q;
};

struct rdx_blk_queue{
	unsigned long *tag_map;
	wait_queue_head_t wait;
	unsigned int queue_depth;

	struct rdx_blk_cmd *cmds;
};

struct rdx_blk {
	struct rdx_blk_queue	*queues;
	unsigned int 			nr_queues;
	unsigned int 			queue_depth;
	struct blk_mq_tag_set 	tag_set;
	struct request_queue 	*q;
	struct gendisk 			*gd;
	struct block_device 	*bdev1;
	struct block_device 	*bdev2;
	char 					*name;
	sector_t 				sectors;
	struct bio_set 			*split_bioset;
};


#define bio_first_sector(bio) ((bio_end_sector(bio) - bio_sectors(bio)))

#endif /* RDX_BLK_H_ */
