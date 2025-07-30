#include "rdma_metrics.h"
#include <linux/version.h>

static struct proc_dir_entry *rdma_metrics_dir;
static struct rdma_metrics current_metrics = {0};

static int rdma_metrics_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "latency: %llu\nthroughput: %llu\n",
               current_metrics.latency, current_metrics.throughput);
    return 0;
}

static int rdma_metrics_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, rdma_metrics_proc_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops rdma_metrics_proc_ops = {
    .proc_open = rdma_metrics_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations rdma_metrics_proc_ops = {
    .owner = THIS_MODULE,
    .open = rdma_metrics_proc_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};
#endif

int rdma_metrics_init(void)
{
    rdma_metrics_dir = proc_mkdir("rdma_metrics", NULL);
    if (!rdma_metrics_dir)
        return -ENOMEM;

    if (!proc_create("metrics", 0444, rdma_metrics_dir, &rdma_metrics_proc_ops)) {
        remove_proc_entry("rdma_metrics", NULL);
        return -ENOMEM;
    }

    return 0;
}

void rdma_metrics_cleanup(void)
{
    remove_proc_entry("metrics", rdma_metrics_dir);
    remove_proc_entry("rdma_metrics", NULL);
}

int rdma_metrics_get(struct rdma_metrics *metrics)
{
    if (!metrics)
        return -EINVAL;

    // 실제로는 sysfs에서 값을 읽어와야 하지만,
    // 현재는 간단히 하드코딩된 값을 반환
    metrics->latency = current_metrics.latency;
    metrics->throughput = current_metrics.throughput;

    return 0;
} 