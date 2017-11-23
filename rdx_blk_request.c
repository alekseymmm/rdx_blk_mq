#include <linux/module.h>

#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <trace/events/block.h>
#include <linux/preempt.h>

#include "rdx_blk.h"
#include "rdx_blk_request.h"


// request covers bio to only one range
blk_qc_t rdx_blk_make_request(struct request_queue *q, struct bio *bio){
	struct rdx_blk *mydev = q->queuedata;
	struct bio *split;

	if (bio_sectors(bio) == 0) {
		bio->bi_error = 0;
		bio_endio(bio);
		return BLK_QC_T_NONE;
	}

	//Exmaple of  bio_redirect

	pr_debug("Before redirecting bio=%p : dir=%s, dev=%s, first_sect=%lu, sectors=%d\n",
				bio, bio_data_dir(bio) == WRITE ? "W" : "R", bio->bi_bdev->bd_disk->disk_name, bio_first_sector(bio), bio_sectors(bio));
	if(bio_first_sector(bio) % 16 == 0){
		bio->bi_bdev = mydev->bdev1;
	} else {
		bio->bi_bdev = mydev->bdev2;
	}
	pr_debug("After redirecting bio=%p : dir=%s, dev=%s, first_sect=%lu, sectors=%d\n",
				bio, bio_data_dir(bio) == WRITE ? "W" : "R", bio->bi_bdev->bd_disk->disk_name, bio_first_sector(bio), bio_sectors(bio));
	submit_bio(bio);

	//Example of bio_split
/*
	pr_debug("Before splitting bio=%p : dir=%s, dev=%s, first_sect=%lu, sectors=%d\n",
				bio, bio_data_dir(bio) == WRITE ? "W" : "R", bio->bi_bdev->bd_disk->disk_name, bio_first_sector(bio), bio_sectors(bio));
	if(bio_sectors(bio) > 4){
		split = bio_split(bio, bio_sectors(bio) / 2, GFP_NOIO, fs_bio_set);
		split->bi_bdev = mydev->bdev1;
		pr_debug("After splitting split=%p : dir=%s, dev=%s, first_sect=%lu, sectors=%d\n",
				split, bio_data_dir(split) == WRITE ? "W" : "R", split->bi_bdev->bd_disk->disk_name, bio_first_sector(split), bio_sectors(split));
		submit_bio(split);
	}

	bio->bi_bdev = mydev->bdev2;
	pr_debug("After splitting bio=%p : dir=%s, dev=%s, first_sect=%lu, sectors=%d\n",
				bio, bio_data_dir(bio) == WRITE ? "W" : "R", bio->bi_bdev->bd_disk->disk_name, bio_first_sector(bio), bio_sectors(bio));
	submit_bio(bio);
*/


	return BLK_QC_T_NONE;
}
