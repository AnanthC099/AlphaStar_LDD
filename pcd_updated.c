#include<linux/module.h>
#include<linux/fs.h>
#include<linux/cdev.h>
#include<linux/device.h>
#include<linux/uaccess.h>

#define DEV_MEM_SIZE 512


/*pseudo device's memory*/
char device_buffer[DEV_MEM_SIZE];

/*This holds the device number*/
dev_t device_number;

/*cdev variable*/
struct cdev pcd_cdev;

loff_t pcd_lseek(struct file *filp, loff_t offset, int whence)
{

	loff_t temp;

	printk(KERN_INFO "lseek requested \n");	
	printk(KERN_INFO "current file position = %lld\n", filp->f_pos);

	switch(whence)
	{
		case SEEK_SET:
			if( (offset > DEV_MEM_SIZE) || (offset < 0))
				return -EINVAL;
			filp->f_pos = offset;
			break;

		case SEEK_CUR:
			temp = filp->f_pos + offset;
			if( (temp > DEV_MEM_SIZE) || (temp < 0) )
				return -EINVAL;
			filp->f_pos = temp;
			break;

		case SEEK_END:
			temp = DEV_MEM_SIZE + offset;
			if( (temp > DEV_MEM_SIZE) || (temp < 0) )
				return -EINVAL;
			filp->f_pos = temp;
			break;

		default:
			return -EINVAL;
			break;
	}
	
	printk(KERN_INFO "New Value of the file position = %lld\n", filp->f_pos);

	return(0);
}

ssize_t pcd_read(struct file *filp, char __user *buff, size_t count,loff_t *f_pos)
{	
	printk(KERN_INFO "read requested for %zu bytes \n",count);
	printk(KERN_INFO "current file position = %lld\n",*f_pos);

	/* Adjust the count */
	if((*f_pos + count) > DEV_MEM_SIZE)
		count = DEV_MEM_SIZE - *f_pos;

	/*Copy to user */
	if(copy_to_user(buff,&device_buffer[*f_pos],count))
	{
		return -EFAULT;
	}

	/*Update the current file position*/
	*f_pos += count;


	printk(KERN_INFO "Number of bytes successfully read  = %zu\n",count);
	printk(KERN_INFO "Updated file position = %lld\n",*f_pos);

	return(count);
}

ssize_t pcd_write(struct file *filp,const char __user *buff,size_t count, loff_t *f_pos)
{
	static int cnt = 0;

	printk(KERN_INFO "write requested for %zu bytes \n",count);
	printk(KERN_INFO "current file position = %lld\n",*f_pos);

	/* Adjust the count */
	if((*f_pos + count) > DEV_MEM_SIZE)
		count = DEV_MEM_SIZE - *f_pos;

	if(!count)
		return -ENOMEM;

	printk(KERN_INFO " static int value = %d",++cnt);

	/*Copy from user*/
	if(copy_from_user(&device_buffer[*f_pos],buff,count))
	{
		return -EFAULT;
	}

	/*Update the current file position*/
	*f_pos += count;


	printk(KERN_INFO "Number of bytes successfully written  = %zu\n",count);
	printk(KERN_INFO "Updated file position = %lld\n",*f_pos);
	return(count);
}

int pcd_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "%s open was successful\n",__func__);
	return(0);
}

int pcd_release(struct inode *inode,struct file *filp)
{
	printk(KERN_INFO "%s close was successful\n",__func__);
	return(0);
}

/* file operations of the drivers*/
struct file_operations pcd_fops =
{
	.open = pcd_open,
	.write = pcd_write,
	.read = pcd_read,
	.llseek = pcd_lseek,
	.release = pcd_release,
	.owner = THIS_MODULE
};

struct class *class_pcd;

struct device *device_pcd;

static char *pcd_devnode(struct device *dev, umode_t *mode)
{
	if(mode)
		*mode = 0666;		//rw for everyone
					
	return NULL;
}

static int __init rs_init(void)
{
	/*Dynamically allocate a device number*/
	alloc_chrdev_region(&device_number,0,1,"pcd");
	
	printk(KERN_INFO "%s : Device number <major>:<minor> = %d:%d\n",__func__,MAJOR(device_number),MINOR(device_number));

	/*Initialize the cdev structure with fops*/
	cdev_init(&pcd_cdev,&pcd_fops);

	/*Register a device (cdev structure) with VFS*/
	pcd_cdev.owner = THIS_MODULE;
	cdev_add(&pcd_cdev,device_number,1);

	/*create device class under /sys/class/ */
	class_pcd = class_create(THIS_MODULE,"pcd_class");

	class_pcd->devnode = pcd_devnode;

	/* populate the sysfs with device info */
	device_pcd = device_create(class_pcd,NULL,device_number,NULL,"pcd");	//this name will appear in /dev
		
	printk(KERN_INFO "Module init was successful\n");	
								
	printk(KERN_INFO "init pseudo char driver\n");
	return(0);
}

static void __exit rs_driver_cleanup(void)
{
	device_destroy(class_pcd, device_number);
	class_destroy(class_pcd);
	cdev_del(&pcd_cdev);
	unregister_chrdev_region(device_number,1);

	printk(KERN_INFO "module unloaded\n");

	printk(KERN_INFO "Exitting pseudo char driver");
}

module_init(rs_init);
module_exit(rs_driver_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alphastar");
MODULE_DESCRIPTION("PCD Kernel Module");



