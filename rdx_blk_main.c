#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>

#include "rdx_blk.h"
#include "rdx_blk_request.h"

char RDX_BLKDEV_NAME[32] = "rdx_blk";

static struct mutex lock;
static int rdx_major;
static int rdx_minor = 1;
static int blocksize = 4096;

bool read_caching_enabled = false;

struct rdx_blk *rdx_blk = NULL;

struct kmem_cache *rdx_request_cachep = NULL;
struct kmem_cache *range_cachep = NULL;
struct workqueue_struct *rdx_blk_wq = NULL;

static char *bdev1_path = "/dev/md/storage_14";
module_param(bdev1_path, charp, 0000);
MODULE_PARM_DESC(bdev1_path, "Path to main storage block device");

static char *bdev2_path = "/dev/md/wal_14";
module_param(bdev2_path, charp, 0000);
MODULE_PARM_DESC(bdev2_path, "Path to buffer storage block device");

static int home_node = NUMA_NO_NODE;
module_param(home_node, int, S_IRUGO);
MODULE_PARM_DESC(home_node, "Home node for the device");

static int submit_queues = 1;
module_param(submit_queues, int, S_IRUGO);
MODULE_PARM_DESC(home_node, "Number of submit queues");

static bool use_per_node_hctx = false;
module_param(use_per_node_hctx, bool, S_IRUGO);
MODULE_PARM_DESC(use_per_node_hctx, "Use per-node allocation for hardware context queues. Default: false");

static int hw_queue_depth = 64;
module_param(hw_queue_depth, int, S_IRUGO);
MODULE_PARM_DESC(hw_queue_depth, "Queue depth for each hardware queue. Default: 64");

static int rdx_blk_open(struct block_device *bdev, fmode_t mode){
	return 0;
}

static void  rdx_blk_release(struct gendisk *disk, fmode_t mode){
}

static const struct block_device_operations rdx_blk_fops ={
		.owner = THIS_MODULE,
		.open = rdx_blk_open,
		.release = rdx_blk_release,
};


static void end_cmd(struct rdx_blk_cmd *cmd){
	struct request_queue *q = NULL;

	if(cmd->rq){
		q = cmd->rq->q;
	}

	blk_mq_end_request(cmd->rq, 0);
	return;
}
static inline void rdx_blk_handle_cmd(struct rdx_blk_cmd *cmd){
	//blk_mq_end_request(cmd->rq, 0);
	blk_mq_complete_request(cmd->rq, cmd->rq->errors);

	return;
}

static int rdx_blk_queue_rq(struct blk_mq_hw_ctx *hctx,
							const struct blk_mq_queue_data *bd){
	struct rdx_blk_cmd *cmd = blk_mq_rq_to_pdu(bd->rq);

	cmd->rq = bd->rq;
	cmd->rdx_blk_q = hctx->driver_data;
	blk_mq_start_request(bd->rq);

	rdx_blk_handle_cmd(cmd);
	return BLK_QC_T_NONE;
}

static void rdx_blk_init_queue(struct rdx_blk *dev, struct rdx_blk_queue *rdx_blk_q)
{
	BUG_ON(!dev);
	BUG_ON(!rdx_blk_q);

	init_waitqueue_head(&rdx_blk_q->wait);
	rdx_blk_q->queue_depth = dev->queue_depth;
}

static int rdx_blk_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
		  unsigned int index){
	struct rdx_blk *dev = data;
	struct rdx_blk_queue *rdx_blk_q = &rdx_blk->queues[index];

	hctx->driver_data = rdx_blk_q;
	rdx_blk_init_queue(dev, rdx_blk_q);
	dev->nr_queues++;

	return 0;
}

static void rdx_blk_done_fn(struct request *rq){
	end_cmd(blk_mq_rq_to_pdu(rq));
}

static struct blk_mq_ops rdx_blk_mq_ops = {
		.queue_rq 	= rdx_blk_queue_rq,
		.init_hctx	= rdx_blk_init_hctx,
		.complete	= rdx_blk_done_fn,
};

static void cleanup_queue(struct rdx_blk_queue *q)
{
	kfree(q->tag_map);
	kfree(q->cmds);
}

static void cleanup_queues(struct rdx_blk *dev)
{
	int i;

	for (i = 0; i < dev->nr_queues; i++)
		cleanup_queue(&dev->queues[i]);

	kfree(dev->queues);
}

int setup_queues(struct rdx_blk *dev){
	dev->queues = kzalloc(submit_queues * sizeof(struct rdx_blk_queue), GFP_KERNEL);

	if(!dev->queues){
		return -ENOMEM;
	}

	dev->nr_queues = 0;
	dev->queue_depth = hw_queue_depth;

	return 0;
}
static void rdx_destroy_dev(void)
{
	pr_debug("Destroying device %s\n", RDX_BLKDEV_NAME);

	if(rdx_blk == NULL){
		pr_debug("rdx_blk s NULL. Destroyed\n");
		return;
	}

	if(rdx_blk->name){
		kfree(rdx_blk->name);
	}

	if(rdx_blk->queues){
		cleanup_queues(rdx_blk);
		pr_debug("For dev %s queues freed\n", RDX_BLKDEV_NAME);
	}

	if(!IS_ERR(rdx_blk->q) && rdx_blk->q != NULL){
		blk_mq_free_tag_set(&rdx_blk->tag_set);
	}

	if(rdx_blk->split_bioset){
		bioset_free(rdx_blk->split_bioset);
		pr_debug("For dev %s bioset freed\n", RDX_BLKDEV_NAME);
	}

	if(rdx_blk->gd){
		del_gendisk(rdx_blk->gd);
		put_disk(rdx_blk->gd);
		pr_debug("For dev %s gendisk deleted\n", RDX_BLKDEV_NAME);
	}


	if(!IS_ERR(rdx_blk->bdev1) && rdx_blk->bdev1 != NULL){
		blkdev_put(rdx_blk->bdev1, FMODE_READ | FMODE_WRITE);
		pr_debug("For dev %s put bdev1\n", RDX_BLKDEV_NAME);
	}

	if(!IS_ERR(rdx_blk->bdev2) && rdx_blk->bdev2 != NULL){
		blkdev_put(rdx_blk->bdev2, FMODE_READ | FMODE_WRITE);
		pr_debug("For dev %s put bdev2\n", RDX_BLKDEV_NAME);
	}

	kfree(rdx_blk);
	rdx_blk = NULL;
	pr_debug("Device %s destroyed \n", RDX_BLKDEV_NAME);
}

static int rdx_blk_create_dev(void)
{
	struct gendisk *gd;
	int ret = 0;

	rdx_blk = kzalloc_node(sizeof(struct rdx_blk), GFP_KERNEL, home_node);
	if(!rdx_blk){
		ret = -ENOMEM;
		pr_debug("Not enough memory for allocating rdx_blk\n");
		goto out;
	}

	rdx_blk->name = kstrdup(RDX_BLKDEV_NAME, GFP_KERNEL);
	if(!rdx_blk->name){
		ret = -ENOMEM;
		pr_debug("Cannot allocate name %s \n", RDX_BLKDEV_NAME);
		goto out;
	}
	pr_debug("Device %s allocated\n", rdx_blk->name);

	if(use_per_node_hctx){
		submit_queues = nr_online_nodes;
	}
	pr_warn("rdx_blk: submit_queues is set to  %u\n", submit_queues);

	ret = setup_queues(rdx_blk);
	if(ret){
		pr_err("Cannot allocate queues\n");
		goto out;
	}

	rdx_blk->tag_set.ops = &rdx_blk_mq_ops; //assign something
	rdx_blk->tag_set.nr_hw_queues = submit_queues;
	rdx_blk->tag_set.queue_depth = hw_queue_depth;
	rdx_blk->tag_set.numa_node = home_node;
	rdx_blk->tag_set.cmd_size = sizeof(struct rdx_blk_cmd);
	rdx_blk->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	rdx_blk->tag_set.driver_data = rdx_blk;

	ret = blk_mq_alloc_tag_set(&rdx_blk->tag_set);
	if(ret){
		pr_err("blk_mq tag allocation failed\n");
		goto out;
	}

	rdx_blk->q = blk_mq_init_queue(&rdx_blk->tag_set);
	if(IS_ERR(rdx_blk->q)){
		pr_err("Cannot init queues\n");
		ret = -ENOMEM;
	}


	rdx_blk->q->queuedata = rdx_blk;
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, rdx_blk->q);
	queue_flag_set_unlocked(QUEUE_FLAG_ADD_RANDOM, rdx_blk->q);
	blk_queue_logical_block_size(rdx_blk->q, blocksize);
	blk_queue_physical_block_size(rdx_blk->q, blocksize);

	rdx_blk->split_bioset = bioset_create_nobvec(RDX_BLK_MIN_POOL_PAGES, 0);
	if(!rdx_blk->split_bioset){
		ret = -ENOMEM;
		pr_debug("Cannot allocate bioset of size %d\n", RDX_BLK_MIN_POOL_PAGES);
		goto out;
	}

	rdx_blk->bdev1 = blkdev_get_by_path(bdev1_path, FMODE_READ | FMODE_WRITE, rdx_blk);
	if(IS_ERR(rdx_blk->bdev1)){
		pr_debug("Cannot find bdev: %s \n", bdev1_path);
		ret = -EINVAL;
		goto out;
	}

	pr_debug("Set main bdev to %s\n", bdev1_path);

	rdx_blk->bdev2 = blkdev_get_by_path(bdev2_path, FMODE_READ | FMODE_WRITE, rdx_blk);
	if(IS_ERR(rdx_blk->bdev2)){
		pr_debug("Cannot find bdev: %s \n", bdev2_path);
		ret = -EINVAL;
		goto out;
	}

	pr_debug("Set bdev2 to %s\n", bdev2_path);
	rdx_blk->sectors = get_capacity(rdx_blk->bdev1->bd_disk);

	gd = alloc_disk_node(rdx_minor, home_node);
	if(!gd){
		pr_debug("Cannot allocate gendisk for %s\b", RDX_BLKDEV_NAME);
		ret = -ENOMEM;
		goto out;
	}

	rdx_blk->gd = gd;
	gd->private_data = rdx_blk;
	gd->queue = rdx_blk->q;
	gd->major = rdx_major;
	gd->first_minor = rdx_minor;
	rdx_minor++;
	gd->flags |= GENHD_FL_EXT_DEVT | GENHD_FL_SUPPRESS_PARTITION_INFO;
	gd->fops = &rdx_blk_fops;
	snprintf(gd->disk_name, DISK_NAME_LEN, "%s", rdx_blk->name);
	set_capacity(gd, rdx_blk->sectors);

	add_disk(gd);
	pr_debug("Disk %s added on node %d, rdx_blk=%p\n", gd->disk_name, home_node, rdx_blk);


	return 0;

out:
	rdx_destroy_dev();
	return ret;
}

static int __init rdx_blk_init(void)
{
	int ret = 0;

	printk("Main storage path: %s, buffer path: %s\n", bdev1_path, bdev2_path);
	mutex_init(&lock);

	if(submit_queues < nr_online_nodes){
		submit_queues = nr_online_nodes;
	} else if(submit_queues > nr_cpu_ids){
		submit_queues = nr_cpu_ids;
	}
	pr_warn("rdx_blk: submit_queues is set to  %u\n", submit_queues);

//    rdx_request_cachep = kmem_cache_create("rdx_request_cachep", sizeof(struct rdx_request),
//    		0, 0,  NULL);
//
//    if (!rdx_request_cachep) {
//        pr_debug( "Could not allocate rdx_request_cachep!\n" ) ;
//        ret = -ENOMEM;
//        goto out_evict_wq;
//    }

	rdx_major = register_blkdev(0, RDX_BLKDEV_NAME);
	if(rdx_major < 0){
		pr_debug("Cannot register blkdev\n");
		ret = -EINVAL;
		goto out;
	}

	pr_info("rdx_blk module loaded\n");
out:
	return ret;
}


static void __exit rdx_blk_exit(void)
{
	if (rdx_blk != NULL){
		rdx_destroy_dev();
	}

//    if (rdx_request_cachep){
//        kmem_cache_destroy(rdx_request_cachep);
//    }


	unregister_blkdev(rdx_major, RDX_BLKDEV_NAME);
	pr_debug("%s unregistered, exit.\n", RDX_BLKDEV_NAME);
}

int __set_cur_cmd(const char *str, struct kernel_param *kp){
	pr_debug("Got command \"%s\" in rp_msb control\n", str);
	if(!strcmp(str, "create\n")){
		rdx_blk_create_dev();
		return 0;
	}
	if(!strcmp(str, "destroy\n")){
		rdx_destroy_dev();
		return 0;
	}

	return 0;
}

MODULE_PARM_DESC(control, "cmd to execute");
module_param_call(control, __set_cur_cmd, NULL, NULL, S_IRUGO | S_IWUSR);

module_init(rdx_blk_init);
module_exit(rdx_blk_exit);

MODULE_AUTHOR("AM");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
