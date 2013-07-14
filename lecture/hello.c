#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

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



