/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

 #include "ocf/ocf.h"
 #include "../ocf_cache_priv.h"
 #include "engine_fast.h"
 #include "engine_common.h"
 #include "engine_pt.h"
 #include "engine_wb.h"
 #include "../ocf_request.h"
 #include "../utils/utils_user_part.h"
 #include "../utils/utils_io.h"
 #include "../concurrency/ocf_concurrency.h"
 #include "../metadata/metadata.h"
 
 #define OCF_ENGINE_DEBUG 0
 
 #define OCF_ENGINE_DEBUG_IO_NAME "fast"
 #include "engine_debug.h"
 
 /*    _____                _   ______        _     _____      _   _
  *   |  __ \              | | |  ____|      | |   |  __ \    | | | |
  *   | |__) |___  __ _  __| | | |__ __ _ ___| |_  | |__) |_ _| |_| |__
  *   |  _  // _ \/ _` |/ _` | |  __/ _` / __| __| |  ___/ _` | __| '_ \
  *   | | \ \  __/ (_| | (_| | | | | (_| \__ \ |_  | |  | (_| | |_| | | |
  *   |_|  \_\___|\__,_|\__,_| |_|  \__,_|___/\__| |_|   \__,_|\__|_| |_|
  */
 
 static void _ocf_read_fast_complete(struct ocf_request *req, int error)
 {
	 if (error)
		 req->error |= error;
 
	 if (env_atomic_dec_return(&req->req_remaining)) {
		 /* Not all requests finished */
		 return; 
	 }
 
	 OCF_DEBUG_RQ(req, "HIT completion");
 
	 if (req->error) {
		 OCF_DEBUG_RQ(req, "ERROR");
 
		 ocf_core_stats_cache_error_update(req->core, OCF_READ);
		 ocf_engine_push_req_front_pt(req);
	 } else {
		 ocf_req_unlock(ocf_cache_line_concurrency(req->cache), req);
 
		 /* Complete request */
		 req->complete(req, req->error);
 
		 /* Free the request at the last point of the completion path */
		 ocf_req_put(req);
	 }
 }
 
static int _ocf_read_fast_do(struct ocf_request *req)
{
    static uint32_t request_counter = 0;
    static uint32_t cache_quota = 0;
    static uint32_t backend_quota = 0;
    static bool last_request_to_cache = false;
    static uint32_t pattern_position = 0;
    static uint32_t pattern_cache = 0;
    static uint32_t pattern_backend = 0;
    static uint32_t pattern_size = 0;
    static uint32_t total_requests = 0;
    static uint32_t cache_requests = 0;
    static uint32_t backend_requests = 0;

    const uint32_t split_ratio = 0;  // % of requests to go to cache (0-100)
    const uint32_t window_size = 100;
    
    // 패턴 크기 계산 (최대 10개로 제한)
    const uint32_t max_pattern_size = 10;
    
    bool send_to_backend;
    uint32_t expected_cache_ratio;
    uint32_t expected_backend_ratio;
    bool is_miss;
    
    // 초기화 또는 split_ratio가 변경되었을 때 패턴 재계산
    if (request_counter % window_size == 0 || pattern_size == 0) {
        // 최대공약수 계산 (유클리드 알고리즘)
        uint32_t a = split_ratio;
        uint32_t b = window_size - split_ratio;
        uint32_t gcd = 1;
        
        if (a > 0 && b > 0) {
            while (b != 0) {
                uint32_t temp = b;
                b = a % b;
                a = temp;
            }
            gcd = a;
        }
        
        // 패턴 크기 계산 (최대공약수로 나눈 값, 최대 max_pattern_size로 제한)
        pattern_size = (split_ratio + (window_size - split_ratio)) / gcd;
        if (pattern_size > max_pattern_size) {
            // 패턴이 너무 크면 더 작은 패턴으로 나눔
            pattern_size = max_pattern_size;
        }
        
        // 패턴 내 cache와 backend 요청 수 계산
        pattern_cache = (split_ratio * pattern_size) / window_size;
        pattern_backend = pattern_size - pattern_cache;
        
        // 전체 요청 수 초기화
        total_requests = 0;
        cache_requests = 0;
        backend_requests = 0;
        
        // quota 초기화
        cache_quota = split_ratio;
        backend_quota = window_size - split_ratio;
        pattern_position = 0;
    }

    // Increment request counter
    request_counter++;
    total_requests++;

    // Miss check - 한 번만 수행
    is_miss = ocf_engine_is_miss(req);
    if (is_miss) {
        OCF_DEBUG_RQ(req, "Backend (miss)");
        ocf_read_pt_do(req);
        return 0;
    }

    // 정확한 비율 계산
    expected_cache_ratio = (total_requests * split_ratio) / window_size;
    expected_backend_ratio = total_requests - expected_cache_ratio;
    
    // 현재까지의 실제 분배와 예상 분배 비교
    if (cache_requests < expected_cache_ratio) {
        // cache 요청이 부족한 경우
        send_to_backend = false;
    } else if (backend_requests < expected_backend_ratio) {
        // backend 요청이 부족한 경우
        send_to_backend = true;
    } else {
        // 둘 다 충분한 경우, 패턴 기반 분배 사용
        if (pattern_position < pattern_size) {
            // 패턴 기반 분배
            send_to_backend = (pattern_position >= pattern_cache);
            pattern_position = (pattern_position + 1) % pattern_size;
        } else {
            // 패턴이 끝난 경우, quota 기반 분배
            if (cache_quota == 0) {
                send_to_backend = true;
            } else if (backend_quota == 0) {
                send_to_backend = false;
            } else {
                // 둘 다 quota 남았을 때, 교차를 유도
                send_to_backend = last_request_to_cache;
            }
        }
    }

    if (send_to_backend) {
        // backend로 요청을 보낼 때마다 quota 감소
        backend_quota--;
        backend_requests++;
        last_request_to_cache = false;

        OCF_DEBUG_RQ(req, "Backend (hit)");
        OCF_DEBUG_RQ(req, "Handling request in BACKEND / split_ratio: %d", 100-split_ratio);

        ocf_read_pt_do(req);
        return 0;
    }

    // Go to cache
    // cache로 요청을 보낼 때마다 quota 감소
    cache_quota--;
    cache_requests++;
    last_request_to_cache = true;

    // Cache path
    ocf_req_get(req);

    if (ocf_engine_needs_repart(req)) {
        OCF_DEBUG_RQ(req, "Repartitioning...");
        ocf_hb_req_prot_lock_wr(req);
        ocf_user_part_move(req);
        ocf_hb_req_prot_unlock_wr(req);
    }

     OCF_DEBUG_RQ(req, "Cache (hit)");
	OCF_DEBUG_RQ(req, "Handling request in CACHE / split_ratio: %d", split_ratio);

    env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));
    ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, req->byte_length,
                          ocf_engine_io_count(req), _ocf_read_fast_complete);

    ocf_engine_update_request_stats(req);
    ocf_engine_update_block_stats(req);
    ocf_req_put(req);

    return 0;
}
 
 static const struct ocf_io_if _io_if_read_fast_resume = {
	 .read = _ocf_read_fast_do,
	 .write = _ocf_read_fast_do,
 };
 
 int ocf_read_fast(struct ocf_request *req)
 {
	 bool hit;
	 int lock = OCF_LOCK_NOT_ACQUIRED;
	 bool part_has_space;
 
	 /* TEMP DEBUG - TESTING IF LOGS WORK */
	//  printk(KERN_INFO "OPENCAS_DEBUG: Testing debug log in ocf_read_fast\n");
 
	 /* Get OCF request - increase reference counter */
	 ocf_req_get(req);
 
	 /* Set resume io_if */
	 req->io_if = &_io_if_read_fast_resume;
 
	 /*- Metadata RD access -----------------------------------------------*/
 
	 ocf_req_hash(req);
	 ocf_hb_req_prot_lock_rd(req);
 
	 /* Traverse request to cache if there is hit */
	 ocf_engine_traverse(req);
 
	 hit = ocf_engine_is_hit(req);
 
	 part_has_space = ocf_user_part_has_space(req);
 
	 if (hit && part_has_space) {
		 ocf_io_start(&req->ioi.io);
		 lock = ocf_req_async_lock_rd(
				 ocf_cache_line_concurrency(req->cache),
				 req, ocf_engine_on_resume);
	 }
 
	 ocf_hb_req_prot_unlock_rd(req);
 
	 if (hit && part_has_space) {
		 OCF_DEBUG_RQ(req, "Fast path success");
 
		 if (lock >= 0) {
			 if (lock != OCF_LOCK_ACQUIRED) {
				 /* Lock was not acquired, need to wait for resume */
				 OCF_DEBUG_RQ(req, "NO LOCK");
			 } else {
				 /* Lock was acquired can perform IO */
				 _ocf_read_fast_do(req);
			 }
		 } else {
			 OCF_DEBUG_RQ(req, "LOCK ERROR");
			 req->complete(req, lock);
			 ocf_req_put(req);
		 }
	 } else {
		 OCF_DEBUG_RQ(req, "Fast path failure");
	 }
 
	 /* Put OCF request - decrease reference counter */
	 ocf_req_put(req);
 
	 return (hit && part_has_space) ? OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
 }
 
 /*  __          __   _ _         ______        _     _____      _   _
  *  \ \        / /  (_) |       |  ____|      | |   |  __ \    | | | |
  *   \ \  /\  / / __ _| |_ ___  | |__ __ _ ___| |_  | |__) |_ _| |_| |__
  *    \ \/  \/ / '__| | __/ _ \ |  __/ _` / __| __| |  ___/ _` | __| '_ \
  *     \  /\  /| |  | | ||  __/ | | | (_| \__ \ |_  | |  | (_| | |_| | | |
  *      \/  \/ |_|  |_|\__\___| |_|  \__,_|___/\__| |_|   \__,_|\__|_| |_|
  */
 
 static const struct ocf_io_if _io_if_write_fast_resume = {
	 .read = ocf_write_wb_do,
	 .write = ocf_write_wb_do,
 };
 
 int ocf_write_fast(struct ocf_request *req)
 {
	 bool mapped;
	 int lock = OCF_LOCK_NOT_ACQUIRED;
	 int part_has_space = false;
 
	 /* Get OCF request - increase reference counter */
	 ocf_req_get(req);
 
	 /* Set resume io_if */
	 req->io_if = &_io_if_write_fast_resume;
 
	 /*- Metadata RD access -----------------------------------------------*/
 
	 ocf_req_hash(req);
	 ocf_hb_req_prot_lock_rd(req);
 
	 /* Traverse request to cache if there is hit */
	 ocf_engine_traverse(req);
 
	 mapped = ocf_engine_is_mapped(req);
 
	 part_has_space = ocf_user_part_has_space(req);
 
	 if (mapped && part_has_space) {
		 ocf_io_start(&req->ioi.io);
		 lock = ocf_req_async_lock_wr(
				 ocf_cache_line_concurrency(req->cache),
				 req, ocf_engine_on_resume);
	 }
 
	 ocf_hb_req_prot_unlock_rd(req);
 
	 if (mapped && part_has_space) {
		 if (lock >= 0) {
			 OCF_DEBUG_RQ(req, "Fast path success");
 
			 if (lock != OCF_LOCK_ACQUIRED) {
				 /* Lock was not acquired, need to wait for resume */
				 OCF_DEBUG_RQ(req, "NO LOCK");
			 } else {
				 /* Lock was acquired can perform IO */
				 ocf_write_wb_do(req);
			 }
		 } else {
			 OCF_DEBUG_RQ(req, "Fast path lock failure");
			 req->complete(req, lock);
			 ocf_req_put(req);
		 }
	 } else {
		 OCF_DEBUG_RQ(req, "Fast path failure");
	 }
 
	 /* Put OCF request - decrease reference counter */
	 ocf_req_put(req);
 
	 return (mapped && part_has_space) ?  OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
 }




// /*
//  * Copyright(c) 2012-2021 Intel Corporation
//  * SPDX-License-Identifier: BSD-3-Clause
//  */

// #include "ocf/ocf.h"
// #include "../ocf_cache_priv.h"
// #include "engine_fast.h"
// #include "engine_common.h"
// #include "engine_pt.h"
// #include "engine_wb.h"
// #include "../ocf_request.h"
// #include "../utils/utils_user_part.h"
// #include "../utils/utils_io.h"
// #include "../utils/pmem_nvme/pmem_nvme_table.h"
// #include "../concurrency/ocf_concurrency.h"
// #include "../metadata/metadata.h"
// #include <linux/fs.h>
// #include <linux/kernel.h>
// #include <linux/path.h>
// #include <linux/atomic.h>

// #define OCF_ENGINE_DEBUG 1

// #define OCF_ENGINE_DEBUG_IO_NAME "fast"
// #include "engine_debug.h"

// // Test app parameters
// const uint64_t IO_DEPTH = 16;
// const uint64_t NUM_JOBS = 1;

// static uint64_t optimal_split_ratio = 49;         /* Global storage for the calculated optimal split ratio */
// static uint64_t last_nonzero_transition_time = 0; // Time when RDMA throughput changed from 0 to non-zero
// static bool in_warmup = false;
// static bool just_initialized = false;

// // RDMA metrics
// static uint64_t curr_rdma_latency;
// static uint64_t curr_rdma_throughput;

// // Modes
// typedef enum
// {
//     NETCAS_MODE_IDLE = 0,
//     NETCAS_MODE_WARMUP = 1,
//     NETCAS_MODE_STABLE = 2,
//     NETCAS_MODE_CONGESTION = 3
// } netCAS_mode_t;

// static netCAS_mode_t netCAS_mode = NETCAS_MODE_IDLE;

// // Moving average window for RDMA throughput
// #define RDMA_WINDOW_SIZE 20
// static uint64_t rdma_throughput_window[RDMA_WINDOW_SIZE] = {0};
// static int rdma_window_index = 0;
// static uint64_t rdma_window_sum = 0;
// static int rdma_window_count = 0;
// static uint64_t rdma_window_average = 0;
// static uint64_t max_average_rdma_throughput = 0;

// /* External function declaration */
// extern int lookup_bandwidth(int io_depth, int num_job, int split_ratio);

/*    _____                _   ______        _     _____      _   _
 *   |  __ \              | | |  ____|      | |   |  __ \    | | | |
 *   | |__) |___  __ _  __| | | |__ __ _ ___| |_  | |__) |_ _| |_| |__
 *   |  _  // _ \/ _` |/ _` | |  __/ _` / __| __| |  ___/ _` | __| '_ \
 *   | | \ \  __/ (_| | (_| | | | | (_| \__ \ |_  | |  | (_| | |_| | | |
 *   |_|  \_\___|\__,_|\__,_| |_|  \__,_|___/\__| |_|   \__,_|\__|_| |_|
 */

// // Global inflight counter for OpenCAS requests
// // static atomic_t ocf_global_inflight = ATOMIC_INIT(0);

// static void _ocf_read_fast_complete(struct ocf_request *req, int error)
// {
//     if (error)
//         req->error |= error;

//     if (env_atomic_dec_return(&req->req_remaining))
//     {
//         /* Not all requests finished */
//         return;
//     }

//     if (req->error)
//     {
//         // OCF_DEBUG_RQ(req, "ERROR");

//         ocf_core_stats_cache_error_update(req->core, OCF_READ);
//         ocf_engine_push_req_front_pt(req);
//     }
//     else
//     {
//         ocf_req_unlock(ocf_cache_line_concurrency(req->cache), req);

//         /* Complete request */
//         req->complete(req, req->error);

//         /* Free the request at the last point of the completion path */
//         ocf_req_put(req);
//     }
// }

// static uint64_t prev_reads_from_core = 0;
// static uint64_t prev_reads_from_cache = 0;
// static bool opencas_stats_initialized = false;
// const uint64_t REQUEST_BLOCK_SIZE = 64;

// static uint64_t measure_iops_using_opencas_stats(struct ocf_request *req, uint64_t elapsed_time /* ms */)
// {
//     uint64_t reads_from_core = 0;
//     uint64_t reads_from_cache = 0;
//     uint64_t curr_IO = 0;
//     uint64_t curr_IOPS = 0;

//     struct ocf_stats_core stats;
//     if (ocf_core_get_stats(req->core, &stats) != 0)
//     {
//         // Failed to get stats, return 0 or error code
//         return 0;
//     }

//     reads_from_cache = stats.cache_volume.read;
//     reads_from_core = stats.core_volume.read;

//     if (!opencas_stats_initialized)
//     {
//         prev_reads_from_core = reads_from_core;
//         prev_reads_from_cache = reads_from_cache;
//         opencas_stats_initialized = true;
//         return 0; // Not enough data to calculate IOPS yet
//     }

//     curr_IO = (reads_from_core - prev_reads_from_core) + (reads_from_cache - prev_reads_from_cache);

//     prev_reads_from_core = reads_from_core;
//     prev_reads_from_cache = reads_from_cache;

//     // Convert to IOPS per second (assuming elapsed_time is in ms)
//     curr_IOPS = (elapsed_time > 0) ? (curr_IO / REQUEST_BLOCK_SIZE) / elapsed_time : 0;

//     return curr_IOPS;
// }

// static uint64_t prev_reads = 0, prev_writes = 0;
// static bool disk_stats_initialized = false;

// /* Cache and core device stat files - use CAS device instead of individual devices */
// static const char *CAS_STAT_FILE = "/sys/block/cas1-1/stat";

// static uint64_t measure_iops_using_disk_stats(struct ocf_request *req, uint64_t elapsed_time /* ms */)
// {
//     struct file *cas_file;
//     uint64_t reads = 0, writes = 0;
//     uint64_t delta_reads, delta_writes, iops = 0;
//     static char cas_buf[1024];
//     char *buf_ptr, *token;
//     int count, read_bytes;
//     mm_segment_t old_fs;

//     /* Prepare for kernel file operations */
//     old_fs = get_fs();
//     set_fs(KERNEL_DS);

//     /* Open CAS stat file */
//     cas_file = filp_open(CAS_STAT_FILE, O_RDONLY, 0);
//     if (IS_ERR(cas_file))
//     {
//         OCF_DEBUG_RQ(req, "disk_stats - Failed to open CAS file: %ld", PTR_ERR(cas_file));
//         set_fs(old_fs);
//         return 0;
//     }

//     /* Read CAS stats */
//     memset(cas_buf, 0, sizeof(cas_buf));
//     read_bytes = kernel_read(cas_file, cas_buf, sizeof(cas_buf) - 1, &cas_file->f_pos);
//     if (read_bytes <= 0)
//     {
//         OCF_DEBUG_RQ(req, "disk_stats - Failed to read CAS file: %d", read_bytes);
//         filp_close(cas_file, NULL);
//         set_fs(old_fs);
//         return 0;
//     }
//     cas_buf[read_bytes] = '\0';

//     /* Close file and restore fs */
//     filp_close(cas_file, NULL);
//     set_fs(old_fs);

//     /* Parse CAS stats */
//     buf_ptr = cas_buf;
//     while (*buf_ptr == ' ')
//         buf_ptr++;

//     for (count = 0; count < 5; ++count)
//     {
//         token = buf_ptr;
//         while (*buf_ptr != ' ' && *buf_ptr != '\0')
//             buf_ptr++;
//         if (*buf_ptr == '\0')
//             break;
//         *buf_ptr++ = '\0';
//         while (*buf_ptr == ' ')
//             buf_ptr++;

//         if (count == 0)
//         {
//             if (kstrtoull(token, 10, &reads))
//                 return 0;
//         }
//         else if (count == 4)
//         {
//             if (kstrtoull(token, 10, &writes))
//                 return 0;
//         }
//     }

//     /* Initialize stats on first run */
//     if (!disk_stats_initialized)
//     {
//         prev_reads = reads;
//         prev_writes = writes;
//         disk_stats_initialized = true;
//         return 0;
//     }

//     /* Calculate deltas */
//     delta_reads = reads - prev_reads;
//     delta_writes = writes - prev_writes;

//     /* Update previous values */
//     prev_reads = reads;
//     prev_writes = writes;

//     /* Calculate IOPS = (reads + writes) / seconds */
//     if (elapsed_time > 0)
//         iops = ((delta_reads + delta_writes) * 1000) / elapsed_time;

//     return iops;
// }

// // New function to measure and log inflight IO depth

// // static uint64_t prev_time = 0; // 이전 측정 시간
// // static void measure_io_depth(struct ocf_request *req)
// // {
// //     ocf_queue_t queue = req->io_queue;
// //     int pending_func = ocf_queue_pending_io(queue);

// //     OCF_DEBUG_RQ(req, "[OCF inflight] ocf_queue_pending_io: %d", pending_func);
// // }

// /* Function to find the best split ratio for given IO depth and NumJob */
// static void find_best_split_ratio(struct ocf_request *req, int io_depth, int num_job, uint64_t curr_rdma_throughput)
// {
//     int bandwidth_cache_only;   /* A: IOPS when split ratio is 100% (all to cache) */
//     int bandwidth_backend_only; /* B: IOPS when split ratio is 0% (all to backend) */
//     int calculated_split;       /* Calculated optimal split ratio */
//     uint64_t drop_permil = 0;

//     /* Get bandwidth for cache only (split ratio 100%) */
//     bandwidth_cache_only = lookup_bandwidth(io_depth, num_job, 100);
//     /* Get bandwidth for backend only (split ratio 0%) */
//     bandwidth_backend_only = lookup_bandwidth(io_depth, num_job, 0);

//     if (max_average_rdma_throughput == 0)
//     {
//         return;
//     }

//     // Calculate how much RDMA throughput is dropped
//     drop_permil = ((max_average_rdma_throughput - rdma_window_average) * 1000) / max_average_rdma_throughput;
//     // OCF_DEBUG_RQ(req, "drop_permil: %llu", drop_permil);
//     // If current RDMA throughput is less than 90% of max_average_rdma_throughput,
//     // change netCAS_mode to NETCAS_MODE_Congestion
//     if (drop_permil > 90)
//     {
//         netCAS_mode = NETCAS_MODE_CONGESTION;
//         bandwidth_backend_only = (int)((bandwidth_backend_only * (1000 - drop_permil)) / 1000);
//         // OCF_DEBUG_RQ(req, "Congestion detected, bandwidth_backend_only: %d", bandwidth_backend_only);
//     }

//     /* Calculate optimal split ratio using formula A/(A+B) */
//     calculated_split = (bandwidth_cache_only * 100) / (bandwidth_cache_only + bandwidth_backend_only);

//     /* Store the calculated optimal split ratio in the global variable */
//     // optimal_split_ratio = calculated_split;

//     // OCF_DEBUG_RQ(req, "Optimal split ratio for IO_Depth=%d, NumJob=%d is %d:%d (cache_iops=%d, adjusted_backend_iops=%d)",
//     //              io_depth, num_job, calculated_split, 100 - calculated_split,
//     //              bandwidth_cache_only, bandwidth_backend_only);
// }

// /* Function to read RDMA metrics from sysfs */
// static void read_rdma_metrics(struct ocf_request *req, uint64_t *latency, uint64_t *throughput)
// {
//     struct file *latency_file, *throughput_file;
//     char buffer[32];
//     int read_bytes;
//     mm_segment_t old_fs;

//     /* Initialize output values to defaults */
//     *latency = 0;
//     *throughput = 0;

//     /* Prepare for kernel file operations */
//     old_fs = get_fs();
//     set_fs(KERNEL_DS);

//     /* Read latency */
//     latency_file = filp_open("/sys/kernel/rdma_metrics/latency", O_RDONLY, 0);
//     if (!IS_ERR(latency_file))
//     {
//         memset(buffer, 0, sizeof(buffer));
//         read_bytes = kernel_read(latency_file, buffer, sizeof(buffer) - 1, &latency_file->f_pos);
//         if (read_bytes > 0)
//         {
//             buffer[read_bytes] = '\0';
//             if (kstrtoull(buffer, 10, latency))
//             {
//                 OCF_DEBUG_RQ(req, "Failed to parse RDMA latency");
//             }
//         }
//         filp_close(latency_file, NULL);
//     }
//     else
//     {
//         OCF_DEBUG_RQ(req, "Failed to open RDMA latency file: %ld", PTR_ERR(latency_file));
//     }

//     /* Read throughput */
//     throughput_file = filp_open("/sys/kernel/rdma_metrics/throughput", O_RDONLY, 0);
//     if (!IS_ERR(throughput_file))
//     {
//         memset(buffer, 0, sizeof(buffer));
//         read_bytes = kernel_read(throughput_file, buffer, sizeof(buffer) - 1, &throughput_file->f_pos);
//         if (read_bytes > 0)
//         {
//             buffer[read_bytes] = '\0';
//             if (kstrtoull(buffer, 10, throughput))
//             {
//                 OCF_DEBUG_RQ(req, "Failed to parse RDMA throughput");
//             }
//         }
//         filp_close(throughput_file, NULL);
//     }
//     else
//     {
//         OCF_DEBUG_RQ(req, "Failed to open RDMA throughput file: %ld", PTR_ERR(throughput_file));
//     }

//     /* Restore fs */
//     set_fs(old_fs);

//     // OCF_DEBUG_RQ(req, "RDMA metrics - latency: %llu, throughput: %llu", *latency, *throughput);
// }

// void measure_performance(struct ocf_request *req, uint64_t elapsed_time)
// {

//     // TODO: Do we need to measure IOPS?
//     // IOPS variables
//     uint64_t curr_opencas_IOPS;
//     uint64_t curr_disk_IOPS;

//     // 1. Measure IOPS
//     curr_opencas_IOPS = measure_iops_using_opencas_stats(req, elapsed_time);
//     curr_disk_IOPS = measure_iops_using_disk_stats(req, elapsed_time);

//     // 2. Read RDMA metrics
//     read_rdma_metrics(req, &curr_rdma_latency, &curr_rdma_throughput);

//     // Log results
//     // OCF_DEBUG_RQ(req, "curr_opencas_IOPS: %llu, curr_disk_IOPS: %llu",
//     //              curr_opencas_IOPS, curr_disk_IOPS);
// }

// static int _ocf_read_fast_do(struct ocf_request *req)
// {
//     static uint64_t request_counter = 0;
//     static uint64_t cache_quota = 0;
//     static uint64_t backend_quota = 0;
//     static bool last_request_to_cache = false;
//     static uint64_t pattern_position = 0;
//     static uint64_t pattern_cache = 0;
//     static uint64_t pattern_backend = 0;
//     static uint64_t pattern_size = 0;
//     static uint64_t total_requests = 0;
//     static uint64_t cache_requests = 0;
//     static uint64_t backend_requests = 0;
//     static uint64_t last_split_ratio_update = 0;
//     static uint64_t current_split_ratio = 100; /* Default starting split ratio */
//     static uint64_t last_split_ratio_log_time = 0;

//     uint64_t curr_time = env_get_tick_count();
//     uint64_t elapsed_time = 0;
//     const uint64_t window_size = 500;
//     const uint64_t split_ratio_update_interval = 500000; /* 5000 milliseconds in microseconds */
//     const uint64_t max_pattern_size = 10;                /* 패턴 크기 계산 (최대 10개로 제한) */
//     const uint64_t netCAS_log_interval = 100000000;      /* 0.1 seconds in nanoseconds */

//     bool send_to_backend;
//     uint64_t expected_cache_ratio;
//     uint64_t expected_backend_ratio;
//     bool is_miss;
//     uint64_t a, b, gcd, temp;
//     uint64_t drop_permil = 0;
//     static bool rdma_window_initialized = false;

//     /* Log optimal split ratio every 0.5 seconds */
//     if (curr_time - last_split_ratio_log_time >= netCAS_log_interval)
//     {
//         OCF_DEBUG_RQ(req, "[netCAS_LOG] time=%llu ms, optimal_split_ratio=%llu, average_rdma_throughput=%llu", curr_time, optimal_split_ratio, rdma_window_average);
//         if (max_average_rdma_throughput > 0)
//         {
//             // Calculate how much RDMA throughput is dropped
//             drop_permil = ((max_average_rdma_throughput - rdma_window_average) * 1000) / max_average_rdma_throughput;
//             OCF_DEBUG_RQ(req, "drop_permil: %llu", drop_permil);
//         }
//         last_split_ratio_log_time = curr_time;
//     }

//     elapsed_time = curr_time - last_split_ratio_update;

//     /* Update split ratio every 50 ms */
//     if (curr_time - last_split_ratio_update > split_ratio_update_interval)
//     {

//         /* Measure performance and update split ratio periodically */
//         measure_performance(req, elapsed_time);

//         // Always check for throughput == 0 first
//         if (curr_rdma_throughput == 0)
//         {
//             netCAS_mode = NETCAS_MODE_IDLE;
//             just_initialized = true;
//             in_warmup = false;
//             last_nonzero_transition_time = 0;
//         }
//         else if (curr_rdma_throughput > 100 && just_initialized)
//         {
//             // Start warmup
//             netCAS_mode = NETCAS_MODE_WARMUP;
//             in_warmup = true;
//             last_nonzero_transition_time = curr_time;
//             OCF_DEBUG_RQ(req, "Starting warmup");
//             just_initialized = false; // Only start warmup once per initialization
//         }
//         else if (in_warmup)
//         {
//             // If in warmup, check for end condition
//             if (curr_time - last_nonzero_transition_time >= 10000000000ULL)
//             {
//                 in_warmup = false;
//                 OCF_DEBUG_RQ(req, "Warmup period over");
//             }
//         }
//         else if (!in_warmup && !just_initialized)
//         {
//             netCAS_mode = NETCAS_MODE_STABLE;
//         }

//         if (netCAS_mode == NETCAS_MODE_STABLE)
//         {
//             /* Update moving average window before using it */
//             if (curr_rdma_throughput == 0)
//             {
//                 if (!rdma_window_initialized)
//                 {
//                     // Already initialized, do nothing
//                 }
//                 else
//                 {
//                     // Reset window
//                     int i;
//                     for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
//                         rdma_throughput_window[i] = 0;
//                     rdma_window_sum = 0;
//                     rdma_window_index = 0;
//                     rdma_window_count = 0;
//                     rdma_window_initialized = false;
//                 }
//             }
//             else
//             {
//                 // Update window
//                 if (rdma_window_count < RDMA_WINDOW_SIZE)
//                 {
//                     rdma_window_count++;
//                 }
//                 else
//                 {
//                     rdma_window_sum -= rdma_throughput_window[rdma_window_index];
//                 }
//                 rdma_throughput_window[rdma_window_index] = curr_rdma_throughput;
//                 rdma_window_sum += curr_rdma_throughput;
//                 rdma_window_average = rdma_window_sum / rdma_window_count;
//                 rdma_window_index = (rdma_window_index + 1) % RDMA_WINDOW_SIZE;
//                 rdma_window_initialized = true;
//                 if (max_average_rdma_throughput < rdma_window_sum / rdma_window_count)
//                 {
//                     max_average_rdma_throughput = rdma_window_sum / rdma_window_count;
//                     OCF_DEBUG_RQ(req, "max_average_rdma_throughput: %llu", max_average_rdma_throughput);
//                 }
//             }
//             if (rdma_window_count >= RDMA_WINDOW_SIZE)
//             {
//                 /* Call find_best_split_ratio to determine the optimal split ratio */
//                 find_best_split_ratio(req, IO_DEPTH, NUM_JOBS, curr_rdma_throughput);
//             }
//         }

//         /* Check if the split ratio has changed significantly */
//         // if (current_split_ratio != optimal_split_ratio)
//         // {
//         //     OCF_DEBUG_RQ(req, "Split ratio changed from %llu to %llu",
//         //                  current_split_ratio, optimal_split_ratio);

//         //     /* Update to new split ratio */
//         //     current_split_ratio = optimal_split_ratio;

//         //     /* Reset counters for the new split ratio */
//         //     cache_quota = 0;
//         //     backend_quota = 0;
//         //     pattern_position = 0;
//         //     pattern_size = 0;
//         // }

//         /* Reset the timer */
//         last_split_ratio_update = curr_time;
//     }

//     /* 초기화 또는 split_ratio가 변경되었을 때 패턴 재계산 */
//     if (request_counter % window_size == 0 || pattern_size == 0)
//     {
//         /* 최대공약수 계산 (유클리드 알고리즘) */
//         a = current_split_ratio;
//         b = window_size - current_split_ratio;
//         gcd = 1;

//         if (a > 0 && b > 0)
//         {
//             while (b != 0)
//             {
//                 temp = b;
//                 b = a % b;
//                 a = temp;
//             }
//             gcd = a;
//         }

//         /* 패턴 크기 계산 (최대공약수로 나눈 값, 최대 max_pattern_size로 제한) */
//         pattern_size = (current_split_ratio + (window_size - current_split_ratio)) / gcd;
//         if (pattern_size > max_pattern_size)
//         {
//             /* 패턴이 너무 크면 더 작은 패턴으로 나눔 */
//             pattern_size = max_pattern_size;
//         }

//         /* 패턴 내 cache와 backend 요청 수 계산 */
//         pattern_cache = (current_split_ratio * pattern_size) / window_size;
//         pattern_backend = pattern_size - pattern_cache;

//         /* 전체 요청 수 초기화 */
//         total_requests = 0;
//         cache_requests = 0;
//         backend_requests = 0;

//         /* quota 초기화 */
//         cache_quota = current_split_ratio;
//         backend_quota = window_size - current_split_ratio;
//         pattern_position = 0;
//     }

//     /* Increment request counter */
//     request_counter++;
//     total_requests++;

//     /* Miss check - 한 번만 수행 */
//     is_miss = ocf_engine_is_miss(req);
//     if (is_miss)
//     {
//         ocf_read_pt_do(req);
//         return 0;
//     }

//     /* 정확한 비율 계산 */
//     expected_cache_ratio = (total_requests * current_split_ratio) / window_size;
//     expected_backend_ratio = total_requests - expected_cache_ratio;

//     /* split_ratio가 0인 경우 즉시 처리 */
//     if (current_split_ratio == 0)
//     {
//         send_to_backend = true;
//         backend_requests++;
//         ocf_read_pt_do(req);
//         return 0;
//     }

//     /* 현재까지의 실제 분배와 예상 분배 비교 */
//     if (cache_requests < expected_cache_ratio)
//     {
//         /* cache 요청이 부족한 경우 */
//         send_to_backend = false;
//     }
//     else if (backend_requests < expected_backend_ratio)
//     {
//         /* backend 요청이 부족한 경우 */
//         send_to_backend = true;
//     }
//     else
//     {
//         /* 둘 다 충분한 경우, 패턴 기반 분배 사용 */
//         if (pattern_position < pattern_size)
//         {
//             /* 패턴 기반 분배 */
//             send_to_backend = (pattern_position >= pattern_cache);
//             pattern_position = (pattern_position + 1) % pattern_size;
//         }
//         else
//         {
//             /* 패턴이 끝난 경우, quota 기반 분배 */
//             if (cache_quota == 0)
//             {
//                 send_to_backend = true;
//             }
//             else if (backend_quota == 0)
//             {
//                 send_to_backend = false;
//             }
//             else
//             {
//                 /* 둘 다 quota 남았을 때, 교차를 유도 */
//                 send_to_backend = last_request_to_cache;
//             }
//         }
//     }

//     if (send_to_backend)
//     {
//         /* backend로 요청을 보낼 때마다 quota 감소 */
//         backend_quota--;
//         backend_requests++;
//         last_request_to_cache = false;

//         ocf_read_pt_do(req);
//         return 0;
//     }

//     /* Go to cache */
//     /* cache로 요청을 보낼 때마다 quota 감소 */
//     cache_quota--;
//     cache_requests++;
//     last_request_to_cache = true;

//     /* Cache path */
//     ocf_req_get(req);

//     if (ocf_engine_needs_repart(req))
//     {
//         ocf_hb_req_prot_lock_wr(req);
//         ocf_user_part_move(req);
//         ocf_hb_req_prot_unlock_wr(req);
//     }

//     env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));
//     ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, req->byte_length,
//                           ocf_engine_io_count(req), _ocf_read_fast_complete);

//     ocf_engine_update_request_stats(req);
//     ocf_engine_update_block_stats(req);
//     ocf_req_put(req);

//     return 0;
// }

// static const struct ocf_io_if _io_if_read_fast_resume = {
//     .read = _ocf_read_fast_do,
//     .write = _ocf_read_fast_do,
// };

// int ocf_read_fast(struct ocf_request *req)
// {
//     bool hit;
//     int lock = OCF_LOCK_NOT_ACQUIRED;
//     bool part_has_space;

//     /* Get OCF request - increase reference counter */
//     ocf_req_get(req);

//     /* Set resume io_if */
//     req->io_if = &_io_if_read_fast_resume;

//     /*- Metadata RD access -----------------------------------------------*/

//     ocf_req_hash(req);
//     ocf_hb_req_prot_lock_rd(req);

//     /* Traverse request to cache if there is hit */
//     ocf_engine_traverse(req);

//     hit = ocf_engine_is_hit(req);

//     part_has_space = ocf_user_part_has_space(req);

//     if (hit && part_has_space)
//     {
//         ocf_io_start(&req->ioi.io);
//         lock = ocf_req_async_lock_rd(
//             ocf_cache_line_concurrency(req->cache),
//             req, ocf_engine_on_resume);
//     }

//     ocf_hb_req_prot_unlock_rd(req);

//     if (hit && part_has_space)
//     {
//         // OCF_DEBUG_RQ(req, "Fast path success");

//         if (lock >= 0)
//         {
//             if (lock != OCF_LOCK_ACQUIRED)
//             {
//                 /* Lock was not acquired, need to wait for resume */
//                 // OCF_DEBUG_RQ(req, "NO LOCK");
//             }
//             else
//             {
//                 /* Lock was acquired can perform IO */
//                 _ocf_read_fast_do(req);
//             }
//         }
//         else
//         {
//             // OCF_DEBUG_RQ(req, "LOCK ERROR");
//             req->complete(req, lock);
//             ocf_req_put(req);
//         }
//     }
//     else
//     {
//         // OCF_DEBUG_RQ(req, "Fast path failure");
//     }

//     /* Put OCF request - decrease reference counter */
//     ocf_req_put(req);

//     return (hit && part_has_space) ? OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
// }

/*  __          __   _ _         ______        _     _____      _   _
 *  \ \        / /  (_) |       |  ____|      | |   |  __ \    | | | |
 *   \ \  /\  / / __ _| |_ ___  | |__ __ _ ___| |_  | |__) |_ _| |_| |__
 *    \ \/  \/ / '__| | __/ _ \ |  __/ _` / __| __| |  ___/ _` | __| '_ \
 *     \  /\  /| |  | | ||  __/ | | | (_| \__ \ |_  | |  | (_| | |_| | | |
 *      \/  \/ |_|  |_|\__\___| |_|  \__,_|___/\__| |_|   \__,_|\__|_| |_|
 */

// static const struct ocf_io_if _io_if_write_fast_resume = {
//     .read = ocf_write_wb_do,
//     .write = ocf_write_wb_do,
// };

// int ocf_write_fast(struct ocf_request *req)
// {
//     bool mapped;
//     int lock = OCF_LOCK_NOT_ACQUIRED;
//     int part_has_space = false;

//     /* Get OCF request - increase reference counter */
//     ocf_req_get(req);

//     /* Set resume io_if */
//     req->io_if = &_io_if_write_fast_resume;

//     /*- Metadata RD access -----------------------------------------------*/

//     ocf_req_hash(req);
//     ocf_hb_req_prot_lock_rd(req);

//     /* Traverse request to cache if there is hit */
//     ocf_engine_traverse(req);

//     mapped = ocf_engine_is_mapped(req);

//     part_has_space = ocf_user_part_has_space(req);

//     if (mapped && part_has_space)
//     {
//         ocf_io_start(&req->ioi.io);
//         lock = ocf_req_async_lock_wr(
//             ocf_cache_line_concurrency(req->cache),
//             req, ocf_engine_on_resume);
//     }

//     ocf_hb_req_prot_unlock_rd(req);

//     if (mapped && part_has_space)
//     {
//         if (lock >= 0)
//         {
//             // OCF_DEBUG_RQ(req, "Fast path success");

//             if (lock != OCF_LOCK_ACQUIRED)
//             {
//                 /* Lock was not acquired, need to wait for resume */
//                 // OCF_DEBUG_RQ(req, "NO LOCK");
//             }
//             else
//             {
//                 /* Lock was acquired can perform IO */
//                 ocf_write_wb_do(req);
//             }
//         }
//         else
//         {
//             // OCF_DEBUG_RQ(req, "Fast path lock failure");
//             req->complete(req, lock);
//             ocf_req_put(req);
//         }
//     }
//     else
//     {
//         // OCF_DEBUG_RQ(req, "Fast path failure");
//     }

//     /* Put OCF request - decrease reference counter */
//     ocf_req_put(req);

//     return (mapped && part_has_space) ? OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
// }
