/***************************************************************************//**
*  \file       block_device_driver.c
*
*  \details    Block Device driver to demonstrate Read sector from the virtual memory
*
*  \author     Anurag Singh
*
*******************************************************************************/

#include <linux/version.h> 	/* LINUX_VERSION_CODE  */
#include <linux/blk-mq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>
#include<linux/moduleparam.h>
#include<linux/stat.h>

#define DRIVER_AUTHOR "Anurag Singh <anuragsingh@inevitableinfotech.com>"
#define DRIVER_DESC "This is a simple block device driver Demonstrates the command line argument"

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
//A version number to inform users
MODULE_VERSION("2.0"); 

static int major_numer = 0;
module_param(major_numer, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int, 0);
static int nsectors = 1024;	
/* How many sectors are in drive */
module_param(nsectors, int, 0);
static int ndevices = 4;
module_param(ndevices, int, 0);

/*
 * The different "request modes" we can use.
 */
enum {
	RM_SIMPLE  = 0,	/* The extra-simple request function */
	RM_FULL    = 1,	/* The full-blown version */
	RM_NOQUEUE = 2,	/* Use make_request */
};
static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);

/*
 * Minor number and partition management.
 */
#define BLOCK_MINORS	16
#define MINOR_SHIFT	4
#define DEVNUM(kdevnum)	(MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERN_SECTOR_SIZE 512
/*
 * After this much idle time, the driver will simulate a media change.
 */
#define INVALIDATE_DELAY 30*HZ

/*
 * The internal representation of our device.
 */
struct block_dev {
        int size;                       /* Device size in sectors */
        u8 *data;                       /* The data array */
        short users;                    /* How many users */
        short media_change;             /* Flag a media change? */
        spinlock_t lock;                /* For mutual exclusion */
	struct blk_mq_tag_set tag_set;	/* tag_set added */
        struct request_queue *queue;    /* The device request queue */
        struct gendisk *gd;             /* The gendisk structure */
        struct timer_list timer;        /* For simulated media changes */
};

static struct block_dev *Devices = NULL;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
static inline struct request_queue *
blk_generic_alloc_queue(make_request_fn make_request, int node_id)
#else
static inline struct request_queue *
blk_generic_alloc_queue(int node_id)
#endif
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))
	struct request_queue *q = blk_alloc_queue(GFP_KERNEL);
	if (q != NULL)
		blk_queue_make_request(q, make_request);

	return (q);
#elif (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
	return (blk_alloc_queue(make_request, node_id));
#else
	return (blk_alloc_queue(node_id));
#endif
}

/*
 * Handle an I/O request.
 */
static void transfer_data(struct block_dev *dev, unsigned long sector,
		unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector * KERN_SECTOR_SIZE;
	unsigned long nbytes = nsect * KERN_SECTOR_SIZE;

	if ((offset + nbytes) > dev->size) {
		printk (KERN_NOTICE "my_block_device: Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}
	if (write)
		memcpy(dev->data + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->data + offset, nbytes);
}

/*
 * The simple form of the request function.
 */
static blk_status_t simple_request(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data* bd)   /* For blk-mq */
{
	struct request *req = bd->rq;
	struct block_dev *dev = req->rq_disk->private_data;
        struct bio_vec bvec;
        struct req_iterator iter;
        sector_t pos_sector = blk_rq_pos(req);
	void	*buffer;
	blk_status_t  ret;

	blk_mq_start_request (req);

	if (blk_rq_is_passthrough(req)) {
		printk (KERN_NOTICE "my_block_device: Skip non-fs request\n");
                ret = BLK_STS_IOERR;  //-EIO
			goto done;
	}
	rq_for_each_segment(bvec, req, iter)
	{
		size_t num_sector = blk_rq_cur_sectors(req);
		printk (KERN_NOTICE "my_block_device: Req dev %u dir %d sec %lld, nr %ld\n",
                        (unsigned)(dev - Devices), rq_data_dir(req),
                        pos_sector, num_sector);
		buffer = page_address(bvec.bv_page) + bvec.bv_offset;
		transfer_data(dev, pos_sector, num_sector,
				buffer, rq_data_dir(req) == WRITE);
		pos_sector += num_sector;
	}
	ret = BLK_STS_OK;
done:
	blk_mq_end_request (req, ret);
	return ret;
}


/*
 * Transfer a single BIO.
 */
static int single_transfer_bio(struct block_dev *dev, struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;

	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, iter) {
		char *buffer = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
		transfer_data(dev, sector, (bio_cur_bytes(bio) / KERN_SECTOR_SIZE),
				buffer, bio_data_dir(bio) == WRITE);
		sector += (bio_cur_bytes(bio) / KERN_SECTOR_SIZE);
		kunmap_atomic(buffer);
	}
	return 0; /* Always "succeed" */
}

/*
 * Transfer a full request.
 */
static int full_transfer_request(struct block_dev *dev, struct request *req)
{
	struct bio *bio;
	int nsect = 0;
    
	__rq_for_each_bio(bio, req) {
		single_transfer_bio(dev, bio);
		nsect += bio->bi_iter.bi_size/KERN_SECTOR_SIZE;
	}
	return nsect;
}



/*
 * Smarter request function that "handles clustering".
 */

static blk_status_t full_request(struct blk_mq_hw_ctx * hctx, const struct blk_mq_queue_data * bd)
{
	struct request *req = bd->rq;
	int sectors_xferred;
	struct block_dev *dev = req->q->queuedata;
	blk_status_t  ret;

	blk_mq_start_request (req);
		if (blk_rq_is_passthrough(req)) {
			printk (KERN_NOTICE "my_block_device: Skip non-fs request\n");
			ret = BLK_STS_IOERR;
			goto done;
		}
		sectors_xferred = full_transfer_request(dev, req);
		ret = BLK_STS_OK; 
	done:
		blk_mq_end_request (req, ret);
	return ret;
}



/*
 * The direct make request version.
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
static blk_qc_t make_request(struct request_queue *q, struct bio *bio)
#else
static blk_qc_t make_request(struct bio *bio)
#endif
{
	struct block_dev *dev = bio->bi_disk->private_data;
	int status;

	status = single_transfer_bio(dev, bio);
	bio->bi_status = status;
	bio_endio(bio);
	return BLK_QC_T_NONE;
}


/*
 * Open and close.
 */

static int my_open(struct block_device *bdev, fmode_t mode)
{
	struct block_dev *dev = bdev->bd_disk->private_data;

	del_timer_sync(&dev->timer);
	spin_lock(&dev->lock);
	printk(KERN_INFO"my_block_device: Device File open sucessfully\n");
	if (! dev->users) 
	{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0))
		check_disk_change(bdev);
#else
                if(bdev_check_media_change(bdev))
                {
                        struct gendisk *gd = bdev->bd_disk;
                        const struct block_device_operations *bdo = gd->fops;
                        if (bdo && bdo->revalidate_disk)
                                bdo->revalidate_disk(gd);
                }
#endif
	}
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static void my_release(struct gendisk *disk, fmode_t mode)
{
	struct block_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;	
	printk(KERN_INFO "my_block_device: Device File release sucessfully\n");
	if (!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);
}

/*
 * Look for a (simulated) media change.
 */
int my_media_changed(struct gendisk *gd)
{

	struct block_dev *dev = gd->private_data;
	printk(KERN_INFO "my_block_device: media changed sucessfully\n");
	return dev->media_change;
}

/*
 * Revalidate.  WE DO NOT TAKE THE LOCK HERE, for fear of deadlocking
 * with open.  That needs to be reevaluated.
 */
int my_revalidate(struct gendisk *gd)
{
	
	struct block_dev *dev = gd->private_data;
	printk(KERN_INFO "my_block_device: revalidate sucessfully\n");
	
	if (dev->media_change) {
		dev->media_change = 0;
		memset (dev->data, 0, dev->size);
	}
	return 0;
}

/*
 * The "invalidate" function runs out of the device timer; it sets
 * a flag to simulate the removal of the media.
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)) && !defined(timer_setup)
void my_invalidate(unsigned long ldev)
{
        struct block_dev *dev = (struct block_dev *) ldev;
#else
void my_invalidate(struct timer_list * ldev)
{
        
	struct block_dev *dev = from_timer(dev, ldev, timer);
	printk(KERN_INFO "my_block_device: validate sucessfully2\n");	
#endif

	spin_lock(&dev->lock);
	if (dev->users || !dev->data) 
		printk (KERN_WARNING "my_block_device: timer sanity check failed\n");
	else
		dev->media_change = 1;
	spin_unlock(&dev->lock);
}

/*
 * The ioctl() implementation
 */

int my_ioctl (struct block_device *bdev, fmode_t mode,
                 unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct block_dev *dev = bdev->bd_disk->private_data;
	printk(KERN_INFO "my_block_device:IOCTL function called sucessfully\n");
	switch(cmd) {
	    case HDIO_GETGEO:
        	/*
		 * Get geometry: since we are a virtual device, we have to make
		 * up something plausible.  So we claim 16 sectors, four heads,
		 * and calculate the corresponding number of cylinders.  We set the
		 * start of data at sector four.
		 */
		size = dev->size*(hardsect_size/KERN_SECTOR_SIZE);
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	
	}
	return -ENOTTY; /* unknown command */
}

/*
 * The device operations structure.
 */
static struct block_device_operations block_ops = {
	.owner           = THIS_MODULE,
	.open 	         = my_open,
	.release 	 = my_release,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
	.media_changed   = my_media_changed,
#else
	.submit_bio      = make_request,
#endif
	.revalidate_disk = my_revalidate,
	.ioctl	         = my_ioctl
};

static struct blk_mq_ops mq_ops_simple = {
    .queue_rq = simple_request,
};

static struct blk_mq_ops mq_ops_full = {
    .queue_rq = full_request,
};


/*
 * Set up our internal device.
 */
static void setup_device(struct block_dev *dev, int which)
{
	/*
	 * Get some memory.
	 */
	memset (dev, 0, sizeof (struct block_dev));
	dev->size = nsectors*hardsect_size;
	dev->data = vmalloc(dev->size);
	if (dev->data == NULL) {
		printk (KERN_NOTICE "my_block_device: vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);
	
	/*
	 * The timer which "invalidates" the device.
	 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)) && !defined(timer_setup)
	init_timer(&dev->timer);
	dev->timer.data = (unsigned long) dev;
	dev->timer.function = my_invalidate;
#else
        timer_setup(&dev->timer, my_invalidate, 0);
#endif


	
	/*
	 * The I/O queue, depending on whether we are using our own
	 * make_request function or not.
	 */
	switch (request_mode) {
	    case RM_NOQUEUE:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
		dev->queue =  blk_generic_alloc_queue(make_request, NUMA_NO_NODE);
#else
		dev->queue =  blk_generic_alloc_queue(NUMA_NO_NODE);
#endif
		if (dev->queue == NULL)
			goto out_vfree;
		break;

	    case RM_FULL:
		//dev->queue = blk_init_queue(full_request, &dev->lock);
		dev->queue = blk_mq_init_sq_queue(&dev->tag_set, &mq_ops_full, 128, BLK_MQ_F_SHOULD_MERGE);
		if (dev->queue == NULL)
			goto out_vfree;
		break;
		/* fall into.. */
            case RM_SIMPLE:
                //dev->queue = blk_init_queue(simple_request, &dev->lock);
                dev->queue = blk_mq_init_sq_queue(&dev->tag_set, &mq_ops_simple, 128, BLK_MQ_F_SHOULD_MERGE);
                if (dev->queue == NULL)
                        goto out_vfree;
                break;

	    default:
		printk(KERN_NOTICE "my_block_device: Bad request mode %d, using simple\n", request_mode);
        	/* fall into.. */	
	}
	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;
	/*
	 * And the gendisk structure.
	 */
	dev->gd = alloc_disk(BLOCK_MINORS);
	if (! dev->gd) {
		printk (KERN_NOTICE "my_block_device: alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = major_numer;
	dev->gd->first_minor = which*BLOCK_MINORS;
	dev->gd->fops = &block_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf (dev->gd->disk_name, 32, "myBlock%c", which + 'a');
	set_capacity(dev->gd, nsectors*(hardsect_size/KERN_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

  out_vfree:
	if (dev->data)
		vfree(dev->data);
}



static int __init my_driver_init(void)
{
	int i;
	/*
	 * Get registered.
	 */
	major_numer = register_blkdev(major_numer, "myBlock");
	if (major_numer <= 0) {
		printk(KERN_WARNING "my_block_device: unable to get major number\n");
		return -EBUSY;
	}
	/*
	 * Allocate the device array, and initialize each one.
	 */
	Devices = kmalloc(ndevices*sizeof (struct block_dev), GFP_KERNEL);
	if (Devices == NULL)
		goto out_unregister;
	for (i = 0; i < ndevices; i++) 
		setup_device(Devices + i, i);
    
	return 0;

  out_unregister:
	unregister_blkdev(major_numer, "sbd");
	return -ENOMEM;
}

static void my_driver_exit(void)
{
	int i;

	for (i = 0; i < ndevices; i++) {
		struct block_dev *dev = Devices + i;

		del_timer_sync(&dev->timer);
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue) {
			if (request_mode == RM_NOQUEUE)
				blk_put_queue(dev->queue);
			else
				blk_cleanup_queue(dev->queue);
		}
		if (dev->data)
			vfree(dev->data);
	}
	unregister_blkdev(major_numer, "myBlock");
	kfree(Devices);
}
	
module_init(my_driver_init);
module_exit(my_driver_exit);
