#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/major.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>


static LIST_HEAD(misc_list);
static DEFINE_MUTEX(misc_mtx);
#define DYNAMIC_MINORS 64 
static unsigned char misc_minors[DYNAMIC_MINORS / 8];
extern int pmu_device_init(void);
#ifdef CONFIG_PROC_FS
static void *misc_seq_start(struct seq_file *seq, loff_t *pos)
{
         mutex_lock(&misc_mtx);
         return seq_list_start(&misc_list, *pos);
 }
 
static void *misc_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
         return seq_list_next(v, &misc_list, pos);
}
 
static void misc_seq_stop(struct seq_file *seq, void *v)
{
         mutex_unlock(&misc_mtx);
}
 
static int misc_seq_show(struct seq_file *seq, void *v)
{
         const struct miscdevice *p = list_entry(v, struct miscdevice, list);
 
         seq_printf(seq, "%3i %s\n", p->minor, p->name ? p->name : "");
         return 0;
}
 
 
static struct seq_operations misc_seq_ops = 
{
         .start = misc_seq_start,
         .next  = misc_seq_next,
         .stop  = misc_seq_stop,
         .show  = misc_seq_show,
};
 
static int misc_seq_open(struct inode *inode, struct file *file)
{
        return seq_open(file, &misc_seq_ops);
}
 
static const struct file_operations misc_proc_fops = 
{
         .owner   = THIS_MODULE,
         .open    = misc_seq_open,
         .read    = seq_read,
         .llseek  = seq_lseek,
         .release = seq_release,
};
#endif

static int misc_open(struct inode * inode, struct file * file)
{
         int minor = iminor(inode);
         struct miscdevice *c;
         int err = -ENODEV;
         const struct file_operations *old_fops, *new_fops = NULL;
         
         lock_kernel();
         mutex_lock(&misc_mtx);
         
         list_for_each_entry(c, &misc_list, list) {
                 if (c->minor == minor) {
                         new_fops = fops_get(c->fops);           
                         break;
                 }
         }
                 
         if (!new_fops) {
                 mutex_unlock(&misc_mtx);
                 request_module("char-major-%d-%d", MISC_MAJOR, minor);
                 mutex_lock(&misc_mtx);
 
                 list_for_each_entry(c, &misc_list, list) {
                         if (c->minor == minor) {
                                 new_fops = fops_get(c->fops);
                                 break;
                         }
                 }
                 if (!new_fops)
                         goto fail;
         }
 
         err = 0;
         old_fops = file->f_op;
         file->f_op = new_fops;
         if (file->f_op->open) {
                 err=file->f_op->open(inode,file);
                 if (err) {
                         fops_put(file->f_op);
                         file->f_op = fops_get(old_fops);
                 }
         }
         fops_put(old_fops);
fail:
         mutex_unlock(&misc_mtx);
         unlock_kernel();
         return err;
}
 
static struct class *misc_class;
static const struct file_operations misc_fops = 
{
         .owner          = THIS_MODULE,
         .open           = misc_open,
 };
 
int misc_register(struct miscdevice * misc)
{
         struct miscdevice *c;
         dev_t dev;
         int err = 0;
 
         INIT_LIST_HEAD(&misc->list);
 
         mutex_lock(&misc_mtx);
         list_for_each_entry(c, &misc_list, list) {
                 if (c->minor == misc->minor) {
                         mutex_unlock(&misc_mtx);
                         return -EBUSY;
                 }
         }
 
         if (misc->minor == MISC_DYNAMIC_MINOR) {
                 int i = DYNAMIC_MINORS;
                 while (--i >= 0)
                         if ( (misc_minors[i>>3] & (1 << (i&7))) == 0)
                                 break;
                 if (i<0) {
                         mutex_unlock(&misc_mtx);
                         return -EBUSY;
                 }
                 misc->minor = i;
         }
 
         if (misc->minor < DYNAMIC_MINORS)
                 misc_minors[misc->minor >> 3] |= 1 << (misc->minor & 7);
         dev = MKDEV(MISC_MAJOR, misc->minor);
 
         misc->this_device = device_create(misc_class, misc->parent, dev,
                                           misc, "%s", misc->name);
         if (IS_ERR(misc->this_device)) {
                 err = PTR_ERR(misc->this_device);
                 goto out;
         }
 
     list_add(&misc->list, &misc_list);
     out:
         mutex_unlock(&misc_mtx);
         return err;
}

int misc_deregister(struct miscdevice *misc)
{
         int i = misc->minor;
 
         if (list_empty(&misc->list))
                 return -EINVAL;
 
         mutex_lock(&misc_mtx);
         list_del(&misc->list);
         device_destroy(misc_class, MKDEV(MISC_MAJOR, misc->minor));
         if (i < DYNAMIC_MINORS && i>0) {
                 misc_minors[i>>3] &= ~(1 << (misc->minor & 7));
         }
         mutex_unlock(&misc_mtx);
         return 0;
}
 
EXPORT_SYMBOL(misc_register);
EXPORT_SYMBOL(misc_deregister);
 
static char *misc_nodename(struct device *dev)
{
         struct miscdevice *c = dev_get_drvdata(dev);
 
         if (c->devnode)
                 return kstrdup(c->devnode, GFP_KERNEL);
         return NULL;
 }
 
static int __init misc_init(void)
{
         int err;
 
#ifdef CONFIG_PROC_FS
         proc_create("misc", 0, NULL, &misc_proc_fops);
#endif
         misc_class = class_create(THIS_MODULE, "misc");
         err = PTR_ERR(misc_class);
         if (IS_ERR(misc_class))
                 goto fail_remove;
 
         err = -EIO;
         if (register_chrdev(MISC_MAJOR,"misc",&misc_fops))
                 goto fail_printk;
         misc_class->nodename = misc_nodename;
         return 0;
 
fail_printk:
         printk("unable to get major %d for misc devices\n", MISC_MAJOR);
         class_destroy(misc_class);
fail_remove:
         remove_proc_entry("misc", NULL);
         return err;
}
subsys_initcall(misc_init);


int __init init_hello()
{
	printk(KERN_ALERT "[module message] hello module.\n");
	return 0;
}

void __exit exit_hello()
{
	printk(KERN_ALERT "[module message] do you exit?.\n");
}

module_init(init_hello);
module_exit(exit_hello);
MODULE_LICENSE("GPL");



