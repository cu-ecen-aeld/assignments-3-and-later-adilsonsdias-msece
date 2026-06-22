/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("AESD Student");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static int aesd_add_packet(struct aesd_dev *dev, const char *data, size_t len)
{
    struct aesd_buffer_entry entry;
    char *storage;

    if (dev->cb.full) {
        kfree(dev->cb.entry[dev->cb.in_offs].buffptr);
        dev->cb.entry[dev->cb.in_offs].buffptr = NULL;
    }

    storage = kmalloc(len, GFP_KERNEL);
    if (!storage) {
        return -ENOMEM;
    }

    memcpy(storage, data, len);
    entry.buffptr = storage;
    entry.size = len;
    aesd_circular_buffer_add_entry(&dev->cb, &entry);

    return 0;
}

static int aesd_append_partial(struct aesd_dev *dev, const char *data, size_t len)
{
    char *new_buf;
    size_t new_size = dev->partial_write_size + len;

    new_buf = krealloc(dev->partial_write_buf, new_size, GFP_KERNEL);
    if (!new_buf) {
        return -ENOMEM;
    }

    memcpy(new_buf + dev->partial_write_size, data, len);
    dev->partial_write_buf = new_buf;
    dev->partial_write_size = new_size;

    return 0;
}

static int aesd_process_partial(struct aesd_dev *dev)
{
    size_t i = 0;
    size_t start = 0;
    int retval = 0;

    while (i < dev->partial_write_size) {
        if (dev->partial_write_buf[i] == '\n') {
            size_t packet_len = i - start + 1;

            retval = aesd_add_packet(dev, dev->partial_write_buf + start, packet_len);
            if (retval) {
                return retval;
            }
            start = i + 1;
        }
        i++;
    }

    if (start > 0) {
        size_t remaining = dev->partial_write_size - start;

        if (remaining == 0) {
            kfree(dev->partial_write_buf);
            dev->partial_write_buf = NULL;
            dev->partial_write_size = 0;
        } else {
            memmove(dev->partial_write_buf, dev->partial_write_buf + start, remaining);
            dev->partial_write_size = remaining;
            dev->partial_write_buf = krealloc(dev->partial_write_buf, remaining, GFP_KERNEL);
            if (!dev->partial_write_buf) {
                dev->partial_write_size = 0;
                return -ENOMEM;
            }
        }
    }

    return 0;
}

static size_t aesd_get_entry_count(struct aesd_circular_buffer *cb)
{
    if (cb->full) {
        return AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    if (cb->in_offs >= cb->out_offs) {
        return cb->in_offs - cb->out_offs;
    }
    return 0;
}

static size_t aesd_get_total_size(struct aesd_circular_buffer *cb)
{
    size_t count = aesd_get_entry_count(cb);
    size_t total = 0;
    uint8_t i;
    uint8_t index;

    for (i = 0; i < count; i++) {
        index = (cb->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        total += cb->entry[index].size;
    }

    return total;
}

static int aesd_seekto_position(struct aesd_dev *dev, uint32_t write_cmd,
                                uint32_t write_cmd_offset, loff_t *pos_rtn)
{
    size_t count;
    size_t accumulated = 0;
    uint8_t i;
    uint8_t index;
    struct aesd_buffer_entry *entry;

    count = aesd_get_entry_count(&dev->cb);
    if (write_cmd >= count) {
        return -EINVAL;
    }

    for (i = 0; i < write_cmd; i++) {
        index = (dev->cb.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        accumulated += dev->cb.entry[index].size;
    }

    index = (dev->cb.out_offs + write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    entry = &dev->cb.entry[index];

    if (write_cmd_offset >= entry->size) {
        return -EINVAL;
    }

    *pos_rtn = (loff_t)(accumulated + write_cmd_offset);
    return 0;
}

static int aesd_validate_fpos(struct aesd_dev *dev, loff_t pos)
{
    if (pos < 0) {
        return -EINVAL;
    }
    if ((size_t)pos > aesd_get_total_size(&dev->cb)) {
        return -EINVAL;
    }
    return 0;
}

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    filp->private_data = &aesd_device;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t entry_offset;
    struct aesd_buffer_entry *entry;
    size_t bytes_to_read;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    while (retval < (ssize_t)count) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->cb, *f_pos, &entry_offset);
        if (!entry) {
            break;
        }

        bytes_to_read = entry->size - entry_offset;
        if (bytes_to_read > count - (size_t)retval) {
            bytes_to_read = count - (size_t)retval;
        }

        if (copy_to_user(buf + retval, entry->buffptr + entry_offset, bytes_to_read)) {
            retval = -EFAULT;
            goto out;
        }

        *f_pos += bytes_to_read;
        retval += (ssize_t)bytes_to_read;
    }

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *kernel_buf;
    int err;

    (void)f_pos;

    if (count == 0) {
        return 0;
    }

    kernel_buf = kmalloc(count, GFP_KERNEL);
    if (!kernel_buf) {
        return -ENOMEM;
    }

    if (copy_from_user(kernel_buf, buf, count)) {
        kfree(kernel_buf);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(kernel_buf);
        return -ERESTARTSYS;
    }

    err = aesd_append_partial(dev, kernel_buf, count);
    if (err == 0) {
        err = aesd_process_partial(dev);
    }

    mutex_unlock(&dev->lock);
    kfree(kernel_buf);

    if (err) {
        return err;
    }

    return (ssize_t)count;
}

static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;
    size_t total_size;
    int retval;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    total_size = aesd_get_total_size(&dev->cb);

    switch (whence) {
    case SEEK_SET:
        newpos = offset;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + offset;
        break;
    case SEEK_END:
        newpos = (loff_t)total_size + offset;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    retval = aesd_validate_fpos(dev, newpos);
    if (retval) {
        mutex_unlock(&dev->lock);
        return retval;
    }

    filp->f_pos = newpos;
    mutex_unlock(&dev->lock);
    return newpos;
}

static long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t newpos;
    long retval = 0;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) {
        return -ENOTTY;
    }
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) {
        return -ENOTTY;
    }

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        if (copy_from_user(&seekto, (void __user *)arg, sizeof(seekto))) {
            return -EFAULT;
        }

        if (mutex_lock_interruptible(&dev->lock)) {
            return -ERESTARTSYS;
        }

        retval = aesd_seekto_position(dev, seekto.write_cmd,
                                      seekto.write_cmd_offset, &newpos);
        if (retval == 0) {
            filp->f_pos = newpos;
        }

        mutex_unlock(&dev->lock);
        break;
    default:
        return -ENOTTY;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =          THIS_MODULE,
    .read =           aesd_read,
    .write =          aesd_write,
    .open =           aesd_open,
    .release =        aesd_release,
    .llseek =         aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.cb);
    mutex_init(&aesd_device.lock);
    aesd_device.partial_write_buf = NULL;
    aesd_device.partial_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.cb, index) {
        kfree(entry->buffptr);
        entry->buffptr = NULL;
        entry->size = 0;
    }

    kfree(aesd_device.partial_write_buf);
    aesd_device.partial_write_buf = NULL;
    aesd_device.partial_write_size = 0;
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
