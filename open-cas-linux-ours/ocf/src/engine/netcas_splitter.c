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
#include "netcas_splitter.h"
#include "netCAS_monitor.h"
#include "../utils/pmem_nvme/pmem_nvme_table.h"

#define OCF_ENGINE_DEBUG 1

#define OCF_ENGINE_DEBUG_IO_NAME "netcas_splitter"
#include "engine_debug.h"

/* NetCAS Splitter - Handles cache/backend request distribution */

// Static variables for tracking split statistics
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

// Configuration constants
static const uint32_t WINDOW_SIZE = 100;
static const uint32_t MAX_PATTERN_SIZE = 10;

// Optimal split ratio management
static uint64_t optimal_split_ratio = SPLIT_RATIO_MAX; // Default 100% to cache
static env_rwlock split_ratio_lock;

// Test app parameters (from netCAS_split.c)
static const uint64_t IO_DEPTH = 16;
static const uint64_t NUM_JOBS = 1;

// Mode management constants (from netCAS_split.c)
static const uint64_t RDMA_THRESHOLD = 100;             /* Threshold for starting warmup */
static const uint64_t CONGESTION_THRESHOLD = 90;        /* 9.0% drop threshold for congestion mode */
static const uint64_t RDMA_LATENCY_THRESHOLD = 1000000; /* 1ms in nanoseconds */
static const uint64_t IOPS_THRESHOLD = 1000;            /* 1000 IOPS */
static const uint64_t WARMUP_PERIOD_NS = 3000000000ULL; /* 3 seconds in nanoseconds */

// Mode management variables (from netCAS_split.c)
static uint64_t last_nonzero_transition_time = 0; // Time when RDMA throughput changed from 0 to non-zero
static bool netCAS_initialized = false;
static bool split_ratio_calculated_in_stable = false; // Track if split ratio was calculated in stable mode
static netCAS_mode_t current_mode = NETCAS_MODE_IDLE;

// Performance monitoring is now handled by netCAS_monitor.c

// Moving average window for RDMA throughput (from netCAS_split.c)
static uint64_t rdma_throughput_window[RDMA_WINDOW_SIZE] = {0};
static uint64_t rdma_window_index = 0;
static uint64_t rdma_window_sum = 0;
static uint64_t rdma_window_count = 0;
static uint64_t rdma_window_average = 0;
static uint64_t max_average_rdma_throughput = 0;

// lookup_bandwidth function is now available from pmem_nvme_table.h

/**
 * @brief Update RDMA throughput window for moving average calculation
 */
static void update_rdma_window(uint64_t curr_rdma_throughput)
{
    // Update window
    if (rdma_window_count < RDMA_WINDOW_SIZE)
    {
        rdma_window_count++;
    }
    else
    {
        rdma_window_sum -= rdma_throughput_window[rdma_window_index];
    }
    rdma_throughput_window[rdma_window_index] = curr_rdma_throughput;
    rdma_window_sum += curr_rdma_throughput;
    rdma_window_average = rdma_window_sum / rdma_window_count;
    rdma_window_index = (rdma_window_index + 1) % RDMA_WINDOW_SIZE;

    if (max_average_rdma_throughput < rdma_window_average)
    {
        max_average_rdma_throughput = rdma_window_average;
    }
}

/**
 * @brief Initialize the netcas splitter
 */
void netcas_splitter_init(void)
{
    int i;

    env_rwlock_init(&split_ratio_lock);
    optimal_split_ratio = SPLIT_RATIO_MAX; // Default 100% to cache

    // Initialize mode management variables
    last_nonzero_transition_time = 0;
    netCAS_initialized = false;
    split_ratio_calculated_in_stable = false;
    current_mode = NETCAS_MODE_IDLE;

    // Performance monitoring variables are handled by netCAS_monitor.c

    // Initialize RDMA throughput window
    for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
        rdma_throughput_window[i] = 0;
    rdma_window_sum = 0;
    rdma_window_index = 0;
    rdma_window_count = 0;
    rdma_window_average = 0;
    max_average_rdma_throughput = 0;
}

/**
 * @brief Determine the current netCAS mode based on performance metrics
 */
static netCAS_mode_t determine_netcas_mode(uint64_t curr_rdma_throughput, uint64_t curr_rdma_latency, uint64_t curr_iops, uint64_t drop_permil)
{
    uint64_t curr_time = env_get_tick_count();

    // No Active RDMA traffic or no IOPS, set netCAS_mode to IDLE
    if (curr_rdma_throughput <= RDMA_THRESHOLD && curr_iops <= IOPS_THRESHOLD)
    {
        current_mode = NETCAS_MODE_IDLE;
        last_nonzero_transition_time = 0;
    }
    // Active RDMA traffic, determine the mode
    else
    {
        // First time active RDMA traffic, set netCAS_mode to WARMUP
        if (current_mode == NETCAS_MODE_IDLE)
        {
            // Idle -> Warmup
            current_mode = NETCAS_MODE_WARMUP;
            last_nonzero_transition_time = curr_time;
            netCAS_initialized = false;
        }
        else if (current_mode == NETCAS_MODE_WARMUP)
        {
            if (curr_time - last_nonzero_transition_time >= WARMUP_PERIOD_NS)
            {
                current_mode = NETCAS_MODE_STABLE;
                split_ratio_calculated_in_stable = false; // Reset flag when entering stable mode
            }
            else
            {
                // Still in warmup, do nothing
            }
        }
        else if (current_mode == NETCAS_MODE_CONGESTION && drop_permil < CONGESTION_THRESHOLD)
        {
            // Congestion -> Stable
            current_mode = NETCAS_MODE_STABLE;
            split_ratio_calculated_in_stable = false; // Reset flag when entering stable mode
        }
        else if (current_mode == NETCAS_MODE_STABLE && drop_permil > CONGESTION_THRESHOLD)
        {
            // Stable -> Congestion
            current_mode = NETCAS_MODE_CONGESTION;
            split_ratio_calculated_in_stable = true; // Set flag when entering congestion
        }
    }
    return current_mode;
}

/**
 * @brief Set split ratio value with writer lock.
 */
static void split_set_optimal_ratio(uint64_t ratio)
{
    env_rwlock_write_lock(&split_ratio_lock);
    optimal_split_ratio = ratio;
    env_rwlock_write_unlock(&split_ratio_lock);
}

/**
 * @brief Calculate split ratio using the formula A/(A+B) * 10000.
 * This is the core formula for determining optimal split ratio.
 * Uses 0-10000 scale where 10000 = 100% for more accurate calculations.
 */
static uint64_t calculate_split_ratio_formula(uint64_t bandwidth_cache_only, uint64_t bandwidth_backend_only)
{
    uint64_t calculated_split;

    /* Calculate optimal split ratio using formula A/(A+B) * 10000 */
    calculated_split = (bandwidth_cache_only * SPLIT_RATIO_SCALE) / (bandwidth_cache_only + bandwidth_backend_only);

    /* Ensure the result is within valid range (0-10000) */
    if (calculated_split < SPLIT_RATIO_MIN)
        calculated_split = SPLIT_RATIO_MIN;
    if (calculated_split > SPLIT_RATIO_MAX)
        calculated_split = SPLIT_RATIO_MAX;

    return calculated_split;
}

/**
 * @brief Function to find the best split ratio for given IO depth and NumJob.
 * Based on the algorithm from netCAS_split.c
 * Returns split ratio in 0-10000 scale where 10000 = 100%.
 */
static uint64_t find_best_split_ratio(uint64_t io_depth, uint64_t numjob, uint64_t drop_permil)
{
    uint64_t bandwidth_cache_only;   /* A: IOPS when split ratio is 100% (all to cache) */
    uint64_t bandwidth_backend_only; /* B: IOPS when split ratio is 0% (all to backend) */
    uint64_t calculated_split;       /* Calculated optimal split ratio */

    /* Get bandwidth for cache only (split ratio 100%) */
    bandwidth_cache_only = (uint64_t)lookup_bandwidth(io_depth, numjob, 100);
    /* Get bandwidth for backend only (split ratio 0%) */
    bandwidth_backend_only = (uint64_t)lookup_bandwidth(io_depth, numjob, 0);

    // Apply drop percentage to backend bandwidth if there's congestion
    if (drop_permil > 0)
    {
        bandwidth_backend_only = (uint64_t)((bandwidth_backend_only * (1000 - drop_permil)) / 1000);
    }

    /* Calculate optimal split ratio using the formula */
    calculated_split = calculate_split_ratio_formula(bandwidth_cache_only, bandwidth_backend_only);

    return calculated_split;
}

/**
 * @brief Calculate GCD using Euclidean algorithm
 */
static uint32_t calculate_gcd(uint32_t a, uint32_t b)
{
    uint32_t temp;

    if (a > 0 && b > 0)
    {
        while (b != 0)
        {
            temp = b;
            b = a % b;
            a = temp;
        }
        return a;
    }
    return 1;
}

/**
 * @brief Initialize or recalculate the splitting pattern
 */
static void initialize_split_pattern(uint64_t split_ratio)
{
    uint32_t gcd;
    uint32_t a = (uint32_t)(split_ratio / 100); // Convert from 0-10000 scale to 0-100
    uint32_t b = WINDOW_SIZE - a;

    // Calculate GCD
    gcd = calculate_gcd(a, b);

    // Calculate pattern size (limited by MAX_PATTERN_SIZE)
    pattern_size = (a + (WINDOW_SIZE - a)) / gcd;
    if (pattern_size > MAX_PATTERN_SIZE)
    {
        pattern_size = MAX_PATTERN_SIZE;
    }

    // Calculate cache and backend requests in pattern
    pattern_cache = (a * pattern_size) / WINDOW_SIZE;
    pattern_backend = pattern_size - pattern_cache;

    // Reset counters
    total_requests = 0;
    cache_requests = 0;
    backend_requests = 0;

    // Initialize quotas
    cache_quota = a;
    backend_quota = WINDOW_SIZE - a;
    pattern_position = 0;
}

/**
 * @brief Decide whether to send request to cache or backend
 * @param req The OCF request
 * @return true if request should go to backend, false for cache
 */
bool netcas_should_send_to_backend(struct ocf_request *req)
{
    bool send_to_backend;
    uint32_t expected_cache_ratio;
    uint32_t expected_backend_ratio;
    uint64_t current_split_ratio;
    uint32_t split_ratio_percent;

    // Get current optimal split ratio
    current_split_ratio = optimal_split_ratio;
    // Convert from 0-10000 scale to 0-100 for internal calculations
    split_ratio_percent = (uint32_t)(current_split_ratio / 100);

    // Initialize or recalculate pattern when needed
    if (request_counter % WINDOW_SIZE == 0 || pattern_size == 0)
    {
        initialize_split_pattern(current_split_ratio);
    }

    // Increment counters
    request_counter++;
    total_requests++;

    // Check for miss first
    if (ocf_engine_is_miss(req))
    {
        OCF_DEBUG_RQ(req, "Backend (miss)");
        return true;
    }

    // Calculate expected ratios
    expected_cache_ratio = (total_requests * split_ratio_percent) / WINDOW_SIZE;
    expected_backend_ratio = total_requests - expected_cache_ratio;

    // Determine where to send request based on current distribution
    if (cache_requests < expected_cache_ratio)
    {
        // Cache requests are below expected ratio
        send_to_backend = false;
    }
    else if (backend_requests < expected_backend_ratio)
    {
        // Backend requests are below expected ratio
        send_to_backend = true;
    }
    else
    {
        // Both are at expected ratios, use pattern-based distribution
        if (pattern_position < pattern_size)
        {
            // Pattern-based distribution
            send_to_backend = (pattern_position >= pattern_cache);
            pattern_position = (pattern_position + 1) % pattern_size;
        }
        else
        {
            // Pattern exhausted, use quota-based distribution
            if (cache_quota == 0)
            {
                send_to_backend = true;
            }
            else if (backend_quota == 0)
            {
                send_to_backend = false;
            }
            else
            {
                // Both quotas available, alternate to maintain balance
                send_to_backend = last_request_to_cache;
            }
        }
    }

    // Update counters and quotas
    if (send_to_backend)
    {
        backend_quota--;
        backend_requests++;
        last_request_to_cache = false;
        OCF_DEBUG_RQ(req, "Backend (hit) - split_ratio: %llu.%02llu%%",
                     current_split_ratio / 100, current_split_ratio % 100);
    }
    else
    {
        cache_quota--;
        cache_requests++;
        last_request_to_cache = true;
        OCF_DEBUG_RQ(req, "Cache (hit) - split_ratio: %llu.%02llu%%",
                     current_split_ratio / 100, current_split_ratio % 100);
    }

    return send_to_backend;
}

/**
 * @brief Update the optimal split ratio based on current conditions
 */
void netcas_update_split_ratio(struct ocf_request *req)
{
    uint64_t new_split_ratio;
    uint64_t drop_permil = 0;
    uint64_t curr_rdma_throughput = 0;
    uint64_t curr_rdma_latency = 0;
    uint64_t curr_iops = 0;
    uint64_t elapsed_time = 100; // Default 100ms interval
    struct performance_metrics metrics;
    netCAS_mode_t netCAS_mode;

    // Measure current performance metrics using netCAS_monitor
    metrics = measure_performance(elapsed_time);
    curr_rdma_throughput = metrics.rdma_throughput;
    curr_rdma_latency = metrics.rdma_latency;
    curr_iops = metrics.iops;

    // Update RDMA throughput window for moving average calculation
    update_rdma_window(curr_rdma_throughput);

    // Calculate drop percentage if we have enough data
    if (max_average_rdma_throughput > 0)
    {
        drop_permil = ((max_average_rdma_throughput - rdma_window_average) * 1000) / max_average_rdma_throughput;
    }

    // Determine current mode based on performance metrics
    netCAS_mode = determine_netcas_mode(curr_rdma_throughput, curr_rdma_latency, curr_iops, drop_permil);

    switch (netCAS_mode)
    {
    case NETCAS_MODE_IDLE:
        if (!netCAS_initialized)
        {
            // Initialize with default values
            optimal_split_ratio = SPLIT_RATIO_MAX;
            split_set_optimal_ratio(optimal_split_ratio);
            netCAS_initialized = true;
        }
        break;

    case NETCAS_MODE_WARMUP:
        // In warmup mode, calculate split ratio without drop (assuming no contention in startup)
        new_split_ratio = find_best_split_ratio(IO_DEPTH, NUM_JOBS, 0);
        if (new_split_ratio != optimal_split_ratio)
        {
            optimal_split_ratio = new_split_ratio;
            split_set_optimal_ratio(optimal_split_ratio);
            OCF_DEBUG_RQ(req, "WARMUP: Updated split ratio to: %llu.%02llu%% (RDMA: %llu, IOPS: %llu)",
                         new_split_ratio / 100, new_split_ratio % 100, curr_rdma_throughput, curr_iops);
        }
        break;

    case NETCAS_MODE_STABLE:
        // Only calculate split ratio once in stable mode
        if (!split_ratio_calculated_in_stable)
        {
            new_split_ratio = find_best_split_ratio(IO_DEPTH, NUM_JOBS, drop_permil);
            optimal_split_ratio = new_split_ratio;
            split_set_optimal_ratio(optimal_split_ratio);
            split_ratio_calculated_in_stable = true; // Mark as calculated
            OCF_DEBUG_RQ(req, "STABLE: Calculated split ratio: %llu.%02llu%% (RDMA: %llu, IOPS: %llu, Drop: %llu%%)",
                         new_split_ratio / 100, new_split_ratio % 100, curr_rdma_throughput, curr_iops, drop_permil / 10);
        }
        break;

    case NETCAS_MODE_CONGESTION:
        // Continuously calculate split ratio in congestion mode
        new_split_ratio = find_best_split_ratio(IO_DEPTH, NUM_JOBS, drop_permil);

        // Update the split ratio if it changed
        if (new_split_ratio != optimal_split_ratio)
        {
            optimal_split_ratio = new_split_ratio;
            split_set_optimal_ratio(new_split_ratio);
            OCF_DEBUG_RQ(req, "CONGESTION: Updated split ratio to: %llu.%02llu%% (RDMA: %llu, IOPS: %llu, Drop: %llu%%)",
                         new_split_ratio / 100, new_split_ratio % 100, curr_rdma_throughput, curr_iops, drop_permil / 10);
        }
        break;

    case NETCAS_MODE_FAILURE:
        // In failure mode, keep current ratio or set to safe default
        OCF_DEBUG_RQ(req, "FAILURE: Keeping current split ratio: %llu.%02llu%% (RDMA: %llu, IOPS: %llu)",
                     optimal_split_ratio / 100, optimal_split_ratio % 100, curr_rdma_throughput, curr_iops);
        break;
    }
}

/**
 * @brief Reset all splitter statistics (useful for testing or reconfiguration)
 */
void netcas_reset_splitter(void)
{
    int i;

    request_counter = 0;
    cache_quota = 0;
    backend_quota = 0;
    last_request_to_cache = false;
    pattern_position = 0;
    pattern_cache = 0;
    pattern_backend = 0;
    pattern_size = 0;
    total_requests = 0;
    cache_requests = 0;
    backend_requests = 0;

    // Reset optimal split ratio to default
    split_set_optimal_ratio(SPLIT_RATIO_MAX);

    // Reset mode management variables
    last_nonzero_transition_time = 0;
    netCAS_initialized = false;
    split_ratio_calculated_in_stable = false;
    current_mode = NETCAS_MODE_IDLE;

    // Performance monitoring variables are handled by netCAS_monitor.c

    // Reset RDMA throughput window
    for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
        rdma_throughput_window[i] = 0;
    rdma_window_sum = 0;
    rdma_window_index = 0;
    rdma_window_count = 0;
    rdma_window_average = 0;
    max_average_rdma_throughput = 0;
}