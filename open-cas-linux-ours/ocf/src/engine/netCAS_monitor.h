/*
 * netCAS monitor module header
 *
 * Provides performance monitoring functionality for netCAS
 */

#ifndef NETCAS_MONITOR_H_
#define NETCAS_MONITOR_H_

#include "ocf/ocf.h"
#include "netcas_common.h"

/* Function declarations */

/**
 * @brief Measure IOPS using OpenCAS statistics
 * @param req The OCF request
 * @param elapsed_time Time elapsed in milliseconds
 * @return IOPS value
 */
uint64_t measure_iops_using_opencas_stats(struct ocf_request *req, uint64_t elapsed_time);

/**
 * @brief Measure IOPS using disk statistics
 * @param elapsed_time Time elapsed in milliseconds
 * @return IOPS value
 */
uint64_t measure_iops_using_disk_stats(uint64_t elapsed_time);

/**
 * @brief Read RDMA metrics from sysfs
 * @return RDMA metrics structure containing latency and throughput
 */
struct rdma_metrics read_rdma_metrics(void);

/**
 * @brief Measure overall performance metrics
 * @param elapsed_time Time elapsed in milliseconds
 * @return Performance metrics structure
 */
struct performance_metrics measure_performance(uint64_t elapsed_time);

#endif /* NETCAS_MONITOR_H_ */ 