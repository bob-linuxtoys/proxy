/*
 * proxy.c:  A bidirectional pipe device
 *
 *	This device is meant as a simple proxy to connect two user-space
 * programs through a device, allowing each of the user space programs
 * to select() on the device.  The first program to open the device gets
 * immediately blocked on either reads or writes until the other side is
 * opened.  The idea of "two sides" is enforced by limiting the number
 * of opens on the device to two.
 *	This device is different from named pipes and pseudo terminals in
 * that it is bidirectional and it doesn't block writes when the buffer
 * is full, it blocks when the buffer is full _OR_ if other end is closed.
 *
 *
 * Copyright (C) 2010-2015, Bob Smith
 * This software is released under your choice of either
 * the GPLv2 or the 3-clause BSD license.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/uaccess.h>


/* Limits and other defines */
/* The # proxy devices.  Max minor # is one less than this */
#define NUM_PX_DEVS (255)
#define DEVNAME "proxy"
#define DEBUGLEVEL (2)


/* Data structure definitions */
/* This structure describes the buffer and queues in one direction */
struct cirbuf {
	char *buf;	/* points to sf circular buffer */
	int widx;	/* where to write next sf character */
	int ridx;	/* where to read next sf character */
	int cidx;	/* file closed at this index.  ==-1 while open */
	wait_queue_head_t que;	   /* sf readers wait on this queue */
};

/* This data structure describes one proxy device.  There
 * is one of these for each instance (minor #) of proxy.
 * Since data flow is completely symmetric, we differentiate
 * the two endpoints as East (e) and West (w), with the
 * two corresponding directions ew and we.
 */
struct px {
	int minor;		/* minor number of this proxy instance */
	struct cirbuf ewbuf;
	struct cirbuf webuf;
	struct semaphore sem;	/* lock to protect nopen */
	int nopen;		/* number of opens on this device */
	struct file *east;	/* used to tell which cirbuf to use */
	struct file *west;	/* used to tell which cirbuf to use */
	int eastaccmode;	/* Access mode (O_RDONLY, O_WRONLY) */
	int westaccmode;	/* needed even after one side closes */
};


/* Function prototypes.  */
int proxy_init_module(void);
void proxy_exit_module(void);
static int proxy_open(struct inode *, struct file *);
static int proxy_release(struct inode *, struct file *);
static ssize_t proxy_read(struct file *, char *, size_t, loff_t *);
static ssize_t proxy_write(struct file *, const char *, size_t, loff_t *);
static unsigned int proxy_poll(struct file *, poll_table *);


/* Global variables */
static int buffersize = 0x1000;		/* circular buffer is 0x1000 4K */
static unsigned char numberofdevs = NUM_PX_DEVS;
static int px_major = 0;		/* major device number */
/* Debuglvl controls whether a printk is executed
 * 0 = no printk at all
 * 1 = printk on error only
 * 2 = printk on errors and on init/remove
 * 3 = debug prink to trace calls into proxy
 * 4 = debug trace inside of proxy calls 
 */
static unsigned char debuglevel = DEBUGLEVEL;	/* printk verbosity */

struct cdev px_cdev;		/* a char device global */
dev_t px_devicenumber;		/* first device number */

module_param(buffersize, int, S_IRUSR);
module_param(debuglevel, byte, S_IRUSR);
module_param(numberofdevs, byte, S_IRUSR);

static struct px *px_devices;	/* point to devices (minors) */

/* map the callbacks into this driver */
static struct file_operations proxy_fops = {
	.owner = THIS_MODULE,
	.open = proxy_open,
	.read = proxy_read,
	.write = proxy_write,
	.poll = proxy_poll,
	.release = proxy_release
};


/* Module description and macros */

MODULE_DESCRIPTION
	("A device to transparently connect two user-space program through a device");
MODULE_AUTHOR("Bob Smith");
MODULE_LICENSE("GPL");
MODULE_PARM_DESC(buffersize, "Size of each buffer. default=4096 (4K) ");
MODULE_PARM_DESC(debuglevel, "Debug level. Higher=verbose. default=2");
MODULE_PARM_DESC(numberofdevs,
			"Create this many minor devices. default=16");



int proxy_init_module(void)
{
	int i, err;
	px_devices = kmalloc(numberofdevs * sizeof(struct px), GFP_KERNEL);
	if (px_devices == NULL) {
		/* no memory available */
		if (debuglevel >= 1)
			printk(KERN_ALERT "%s: init fails: no memory.\n",
					DEVNAME);
		return 0;
	}
	memset(px_devices, 0, numberofdevs * sizeof(struct px));

	/* init devices in this block */
	for (i = 0; i < numberofdevs; i++) {   /* for every minor device */
		px_devices[i].minor = i;	  /* set minor number */
		px_devices[i].ewbuf.buf = (char *) 0;
		px_devices[i].webuf.buf = (char *) 0;
		px_devices[i].ewbuf.widx = 0;
		px_devices[i].webuf.widx = 0;
		px_devices[i].ewbuf.ridx = 0;
		px_devices[i].webuf.ridx = 0;
		px_devices[i].ewbuf.cidx = -1;
		px_devices[i].webuf.cidx = -1;
		px_devices[i].east = (struct file *) 0;  /* !=0 if open */
		px_devices[i].west = (struct file *) 0;
		init_waitqueue_head(&px_devices[i].ewbuf.que);
		init_waitqueue_head(&px_devices[i].webuf.que);
		px_devices[i].nopen = 0;
#ifdef init_MUTEX
		init_MUTEX(&px_devices[i].sem);
#else
		sema_init(&px_devices[i].sem,1);
#endif
	}

	/* alloc number of char devs in kernel */
	err = alloc_chrdev_region(&px_devicenumber, 0, numberofdevs, DEVNAME);
	if (err < 0) {
		if (debuglevel >= 1)
			printk(KERN_ALERT "%s: init fails. err=%d.\n",
					DEVNAME, err);
		return err;
	}
	px_major = MAJOR(px_devicenumber);	/* save assign major */
	cdev_init(&px_cdev, &proxy_fops);	/* init dev structures */
	kobject_set_name(&(px_cdev.kobj), "proxy%d", px_devicenumber);

	err = cdev_add(&px_cdev, px_devicenumber, numberofdevs);
	if (err < 0) {
		if (debuglevel >= 1)
			printk(KERN_ALERT "%s: init fails. err=%d.\n",
					DEVNAME, err);
		return err;
	}

	if (debuglevel >= 2)
		printk(KERN_INFO 
			"%s: Installed %d minor devices on major number %d.\n",
		   		DEVNAME, numberofdevs, px_major);
	return 0;	/* success */
}


void proxy_exit_module(void)
{
	int i;
	if (!px_devices)
		return;

	for (i = 0; i < numberofdevs; i++) {
		if (px_devices[i].ewbuf.buf) {
			kfree(px_devices[i].ewbuf.buf);
		}
		if (px_devices[i].webuf.buf) {
			kfree(px_devices[i].webuf.buf);
		}
	}

	cdev_del(&px_cdev);		/* delete major device */
	kfree(px_devices);		/* free */
	px_devices = NULL;		/* reset pointer */
	unregister_chrdev_region(px_devicenumber, numberofdevs);

	if (debuglevel >= 2)
		printk(KERN_INFO "%s: Uninstalled.\n", DEVNAME);
}


static int proxy_open(struct inode *inode, struct file *filp)
{
	int mnr = iminor(inode);
	struct px *dev = &px_devices[mnr];

	if (debuglevel >= 3) {
		printk(KERN_DEBUG "%s open. Minor#=%d.\n", DEVNAME, mnr);
	}

	if (down_interruptible(&dev->sem))	/* prevent races on open */
		return -ERESTARTSYS;

	if (dev->nopen >= 2) {			/* Only two opens please! */
		up(&dev->sem);
		return -EBUSY;
	}
	dev->nopen = dev->nopen + 1;

	if (!dev->ewbuf.buf) {			/* get east-to-west buffer */
		dev->ewbuf.buf = kmalloc(buffersize, GFP_KERNEL);
		if (!dev->ewbuf.buf) {
			if (debuglevel >= 1)
				printk(KERN_ALERT "%s: No memory dev=%d.\n",
						DEVNAME, mnr);
			up(&dev->sem);
			return -ENOMEM;
		}
	}
	if (!dev->webuf.buf) {			/* get west-to-east buffer */
		dev->webuf.buf = kmalloc(buffersize, GFP_KERNEL);
		if (!dev->webuf.buf) {
			if (debuglevel >= 1)
				printk(KERN_ALERT "%s: No memory dev=%d.\n",
						DEVNAME, mnr);
			up(&dev->sem);
			return -ENOMEM;
		}
	}

	/* store the proxy device in the file's private data */
	filp->private_data = (void *) dev;
	if (dev->east == (struct file *) 0) {
		dev->east = filp;		/* tells west from east */
		dev->webuf.ridx = dev->webuf.widx; /* reader starts caught up */
		dev->ewbuf.cidx = -1;		/* xmit is open */
		dev->webuf.cidx = -1;		/* xmit is open */
		if (dev->nopen == 2) {		/* wake up other end */
			wake_up_interruptible(&dev->webuf.que);
		}
		dev->eastaccmode = filp->f_flags;
	}
	else if (dev->west == (struct file *) 0) {
		dev->west = filp;		/* tells east from west */
		dev->ewbuf.ridx = dev->ewbuf.widx; /* reader starts caught up */
		dev->webuf.cidx = -1;
		dev->ewbuf.cidx = -1;
		if (dev->nopen == 2) {		/* wake up other end */
			wake_up_interruptible(&dev->ewbuf.que);
		}
		dev->westaccmode = filp->f_flags;
	}
	else if (debuglevel >= 1)
		printk(KERN_ALERT "%s: inconsistent open count.\n", DEVNAME);

	up (&dev->sem);			/* unlock sema we are done */

	return nonseekable_open(inode, filp);	/* success */
}


static int proxy_release(struct inode *inode, struct file *filp)
{
	struct px *dev = (struct px *) filp->private_data;

	if (debuglevel >= 3)
		printk(KERN_DEBUG "%s release. Minor#=%d.\n", DEVNAME,
			((struct px *) filp->private_data)->minor);

	if (down_interruptible(&dev->sem))	/* prevent races on close */
		return -ERESTARTSYS;

	dev->nopen = dev->nopen - 1;

	if (dev->east == filp) {
		dev->east = (struct file *) 0;	/* mark as not in use */
		dev->ewbuf.cidx = dev->ewbuf.widx; /* set close index */
	}
	else if (dev->west == filp) {
		dev->west = (struct file *) 0;	/* mark as not in use */
		dev->webuf.cidx = dev->webuf.widx; /* set close index */
	}
	else if (debuglevel >= 1)
		printk(KERN_ALERT "%s: inconsistent open count.\n", DEVNAME);

	up(&dev->sem);			/* unlock sema we are done */

	return 0;			/* success */
}


/* Utility to look for a full circular buffer */
int is_full(struct cirbuf *pcbuffer)
{
	if ((pcbuffer->ridx - pcbuffer->widx == 1) ||
	    ((pcbuffer->ridx == 0) && (pcbuffer->widx == buffersize -1)))
		return 1;
	else
		return 0;
}


static ssize_t proxy_read(
	struct file *filp, char __user * buff,
	size_t count,
	loff_t * offset)
{
	int ret;
	int xfer;			/* num bytes read from proxy buf */
	int cpcnt;			/* cp count and start location */
	struct cirbuf *pcbuffer;

	struct px *dev = (struct px *) filp->private_data;

	if (debuglevel >= 3)
		printk(KERN_DEBUG "%s: read %zu char from dev%d, off=%lld.\n",
			   DEVNAME, count, dev->minor, *offset);

	if (filp == dev->east)
		pcbuffer = &dev->webuf;
	else if (filp == dev->west)
		pcbuffer = &dev->ewbuf;
	else
		return 0;	 /* should not get here */

	/* cidx is set if writer is trying to close the file */
	if (pcbuffer->ridx == pcbuffer->cidx) {
		return 0;
	}

	/* Wait here until new data is available */
	while (pcbuffer->ridx == pcbuffer->widx) {
		if (filp->f_flags & O_NONBLOCK)
			return -EWOULDBLOCK;
		/* wait on event queue, predicate is .. */
		if (wait_event_interruptible(pcbuffer->que,
			(pcbuffer->ridx != pcbuffer->widx))) {
			if (debuglevel >= 1)
				printk(KERN_INFO
			"%s: read failed in wait_event_interruptible\n",
				DEVNAME);
			return -ERESTARTSYS;
		}
	}

	/* Copy the new data out to the user */
	xfer = pcbuffer->widx - pcbuffer->ridx;
	xfer = (xfer < 0) ? (xfer + buffersize) : xfer;
	xfer = min((int) count, xfer);
	ret = xfer;		/* we will handle these bytes */

	cpcnt = buffersize - pcbuffer->ridx;
	cpcnt = (cpcnt < xfer) ? cpcnt : xfer; 
	if (cpcnt) {
		if (copy_to_user(buff, pcbuffer->buf + pcbuffer->ridx, cpcnt)) {
			if (debuglevel >= 1)
				printk(KERN_INFO
					"%s: read failed in copy_to_user.\n",
					DEVNAME);
			return -EFAULT;
		}
	}

	if (xfer - cpcnt > 0) {
		if (copy_to_user(buff + cpcnt, pcbuffer->buf, xfer - cpcnt)) {
			if (debuglevel >= 1)
				printk(KERN_INFO
					"%s: read failed in copy_to_user.\n",
					DEVNAME);
			return -EFAULT;
		}
	}
	pcbuffer->ridx += xfer;
	pcbuffer->ridx -= (pcbuffer->ridx > buffersize -1) ? buffersize : 0 ;

	/* This is what the writers have been waiting for */
	wake_up_interruptible(&pcbuffer->que);

	if (debuglevel >= 3)
		printk(KERN_DEBUG "%s: read %d bytes.\n", DEVNAME, xfer);
	return ret;
}


static ssize_t proxy_write(
	struct file *filp,
	const char __user * buff,
	size_t count, loff_t * off)
{
	int ret;
	int xfer;			/* num bytes to read from user */
	int cpcnt;			/* num bytes in a single copy */
	struct cirbuf *pcbuffer;

	struct px *dev = (struct px *) filp->private_data;

	if (debuglevel >= 3)
		printk(KERN_DEBUG "%s: write %zu char from dev%d\n",
				DEVNAME, count, dev->minor);

	if (filp == dev->east)
		pcbuffer = &dev->ewbuf;
	else if (filp == dev->west)
		pcbuffer = &dev->webuf;
	else {
		if (debuglevel >= 3)
			printk(KERN_DEBUG "%s: can't tell east from west.\n",
					DEVNAME);
		return 0;	 /* should not get here */
	}

	/* Wait here until new data is available to write */
	while ((dev->nopen != 2) || is_full(pcbuffer)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EWOULDBLOCK;
		/* wait on event queue, predicate is .. */
		if (wait_event_interruptible(pcbuffer->que,
			((dev->nopen == 2) && (! is_full(pcbuffer))))) {
			if (debuglevel >= 1)
				printk(KERN_INFO
			 "%s: write failed in wait_event_interruptible.\n",
                                DEVNAME);
			return -ERESTARTSYS;
		}
	}

	xfer = pcbuffer->ridx - 1 - pcbuffer->widx;
	xfer = (xfer < 0) ? xfer + buffersize : xfer;
	xfer = min((int) count, xfer);
	ret = xfer;

	cpcnt = min(xfer, buffersize - pcbuffer->widx);
	if (cpcnt) {
		if (copy_from_user(pcbuffer->buf + pcbuffer->widx,
			buff, cpcnt)) {
			if (debuglevel >= 1)
				printk(KERN_INFO
				"%s: read failed in copy_from_user.\n",
					DEVNAME);
			return -EFAULT;
		}
	}

	if (xfer - cpcnt > 0) {
		if (copy_from_user(pcbuffer->buf,buff + cpcnt, xfer - cpcnt)) {
			if (debuglevel >= 1)
				printk(KERN_INFO
				"%s: read failed in copy_from_user.\n",
					DEVNAME);
			return -EFAULT;
		}
	}
	pcbuffer->widx += xfer;
	pcbuffer->widx -= (pcbuffer->widx > buffersize -1) ? buffersize : 0 ;

	/* Count=0 if writer is trying to close the file */
	if (count == 0) {
		pcbuffer->cidx = pcbuffer->widx;
	}

	/* This is what the readers have been waiting for */
	wake_up_interruptible(&pcbuffer->que);

	if (debuglevel >= 3)
		printk(KERN_DEBUG "%s: wrote %d bytes.\n", DEVNAME, ret);
	return ret;
}


static unsigned int proxy_poll(struct file *filp, poll_table * ppt)
{
	int ready_mask = 0;
	struct px *dev = filp->private_data;

	poll_wait(filp, &dev->ewbuf.que, ppt);
	poll_wait(filp, &dev->webuf.que, ppt);


	if (filp == dev->west) {
		/* Writable if there's space, the other end is connected,
		 * we haven't already written an end-of-file marker,
		 * the other side is not WRONLY, and our side is not O_RDONLY
		 */
		if (!is_full(&dev->webuf) && (dev->nopen == 2)
		   && (dev->webuf.cidx != dev->webuf.widx)
		   && ((dev->eastaccmode & O_ACCMODE) != O_WRONLY)
		   && ((filp->f_flags & O_ACCMODE) != O_RDONLY)) {
			ready_mask = POLLOUT | POLLWRNORM;
		}
		/* Readable if the buffer has data or we're at end of file,
		 * and the other sice is not RDONLY,
		 * and our side is not O_WRONLY
		 */
		if (((dev->ewbuf.widx != dev->ewbuf.ridx)
		    || (dev->ewbuf.ridx == dev->ewbuf.cidx))
		   && ((dev->eastaccmode & O_ACCMODE) != O_RDONLY)
		   && ((filp->f_flags & O_ACCMODE) != O_WRONLY)) {
			ready_mask |= (POLLIN | POLLRDNORM);
		}
	}
	else if (filp == dev->east) {
		if (!is_full(&dev->ewbuf) && (dev->nopen == 2)
		   && (dev->ewbuf.cidx != dev->ewbuf.widx)
		   && ((dev->westaccmode & O_ACCMODE) != O_WRONLY)
		   && ((filp->f_flags & O_ACCMODE) != O_RDONLY)) {
			ready_mask = POLLOUT | POLLWRNORM;
		}
		if (((dev->webuf.widx != dev->webuf.ridx)
		    || (dev->webuf.ridx == dev->webuf.cidx))
		   && ((dev->westaccmode  & O_ACCMODE) != O_RDONLY)
		   && ((filp->f_flags & O_ACCMODE) != O_WRONLY)) {
			ready_mask |= (POLLIN | POLLRDNORM);
		}
	}

	if (debuglevel >= 3)
		printk(KERN_DEBUG "%s: poll returns 0x%x.\n",
				DEVNAME, ready_mask);
	return ready_mask;
}

module_init(proxy_init_module);
module_exit(proxy_exit_module);
