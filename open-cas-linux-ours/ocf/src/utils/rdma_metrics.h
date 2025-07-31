#ifndef __RDMA_METRICS_H__
#define __RDMA_METRICS_H__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

struct rdma_metrics {
    uint64_t latency;
    uint64_t throughput;
};

int rdma_metrics_init(void);
void rdma_metrics_cleanup(void);
int rdma_metrics_get(struct rdma_metrics *metrics);

#endif /* __RDMA_METRICS_H__ */ 