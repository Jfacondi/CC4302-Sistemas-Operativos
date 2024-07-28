#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>

#include "kmutex.h"

MODULE_LICENSE("Dual BSD/GPL");

int disco_open(struct inode *inode, struct file *filp);
int disco_release(struct inode *inode, struct file *filp);
ssize_t disco_read(struct file *filp, char *buf, size_t count, loff_t *f_pos);
ssize_t disco_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos);
void disco_exit(void);
int disco_init(void);

struct file_operations disco_fops = {
  .read = disco_read,
  .write = disco_write,
  .open = disco_open,
  .release = disco_release
};

typedef struct {
    char *buff;
    int in;
    int out;
    int size;
    int lectorf;
    int lector;
    int escritor;
} data_pipe;

module_init(disco_init);
module_exit(disco_exit);

#define TRUE 1
#define FALSE 0

int disco_major = 61;
#define MAX_SIZE 8192

static int wait;
static KMutex mutex;
static KCondition cond;
data_pipe *pipe;

int disco_init(void) {
    int rc = register_chrdev(disco_major, "syncread", &disco_fops);
    if (rc < 0) {
        printk("<1>syncread: cannot obtain major number %d\n", disco_major);
        return rc;
    }
    pipe = NULL;
    wait = FALSE;
    m_init(&mutex);
    c_init(&cond);
    printk("<1>Inserting syncread module\n");
    return 0;
}

void disco_exit(void) {
    unregister_chrdev(disco_major, "syncread");
    printk("<1>Removing syncread module\n");
}

int disco_open(struct inode *inode, struct file *filp) {
    int rc = 0;
    m_lock(&mutex);
    data_pipe *pipeact;

    if (filp->f_mode & FMODE_WRITE) {
        if (wait) {
            pipeact = pipe;
            filp->private_data = (void *)pipeact;
            wait = FALSE;
        } else {
            pipeact = kmalloc(sizeof(data_pipe), GFP_KERNEL);
            if (!pipeact) {
                rc = -ENOMEM;
                goto epilog;
            }
            pipeact->buff = kmalloc(MAX_SIZE, GFP_KERNEL);
            if (!pipeact->buff) {
                kfree(pipeact);
                rc = -ENOMEM;
                goto epilog;
            }
            memset(pipeact->buff, 0, MAX_SIZE);
            pipeact->in = 0;
            pipeact->out = 0;
            pipeact->size = 0;
            pipeact->lectorf = FALSE;
            pipe = pipeact;
            wait = TRUE;
        }
        pipeact->lector = TRUE;
        pipeact->escritor = TRUE;
        pipeact->size = 0;
        filp->private_data = (void *)pipeact;
        c_broadcast(&cond);
    } else if (filp->f_mode & FMODE_READ) {
        if (wait) {
            pipeact = pipe;
            filp->private_data = (void *)pipeact;
            wait = FALSE;
            c_broadcast(&cond);
        } else {
            pipeact = kmalloc(sizeof(data_pipe), GFP_KERNEL);
            if (!pipeact) {
                rc = -ENOMEM;
                goto epilog;
            }
            pipeact->buff = kmalloc(MAX_SIZE, GFP_KERNEL);
            if (!pipeact->buff) {
                kfree(pipeact);
                rc = -ENOMEM;
                goto epilog;
            }
            memset(pipeact->buff, 0, MAX_SIZE);
            pipeact->in = 0;
            pipeact->out = 0;
            pipeact->size = 0;
            pipeact->lectorf = FALSE;
            pipe = pipeact;
            wait = TRUE;
            filp->private_data = (void *)pipeact;
        }
    }

epilog:
    m_unlock(&mutex);
    return rc;
}

int disco_release(struct inode *inode, struct file *filp) {
    m_lock(&mutex);
    data_pipe *pipeact = (data_pipe *)filp->private_data;

    if (filp->f_mode & FMODE_WRITE) {
        pipeact->lectorf = TRUE;
        c_broadcast(&cond);
    }

    m_unlock(&mutex);
    return 0;
}

ssize_t disco_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
    ssize_t rc = 0;
    m_lock(&mutex);
    data_pipe *pipeact = (data_pipe *)filp->private_data;

    while (!pipeact->escritor || (pipeact->size <= *f_pos)) {
        if (c_wait(&cond, &mutex)) {
            rc = -EINTR;
            goto epilog;
        }
    }
    if (pipeact->lectorf) {
        count = 0;
        goto epilog;
    }
    if (count > pipeact->size) {
        count = pipeact->size;
    }
    for (int k = 0; k < count; k++) {
        if (copy_to_user(buf + k, pipeact->buff + pipeact->out, 1) != 0) {
            rc = -EFAULT;
            goto epilog;
        }
        pipeact->out = (pipeact->out + 1) % MAX_SIZE;
    }
    pipeact->size -= count;
    rc = count;
epilog:
    m_unlock(&mutex);
    return rc;
}

ssize_t disco_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos) {
    ssize_t rc = 0;
    m_lock(&mutex);
    data_pipe *pipeact = (data_pipe *)filp->private_data;
    if (pipeact->size == MAX_SIZE) {
        rc = -ENOSPC;
        goto epilog;
    }
    if (count > (MAX_SIZE - pipeact->size)) {
        count = MAX_SIZE - pipeact->size;
    }
    for (int k = 0; k < count; k++) {
        if (copy_from_user(pipeact->buff + pipeact->in, buf + k, 1) != 0) {
            rc = -EFAULT;
            goto epilog;
        }
        pipeact->in = (pipeact->in + 1) % MAX_SIZE;
    }
    pipeact->size += count;
    rc = count;
    c_broadcast(&cond);
epilog:
    m_unlock(&mutex);
    return rc;
}

