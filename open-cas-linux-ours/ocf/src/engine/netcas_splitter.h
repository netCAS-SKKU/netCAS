/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __OCF_ENGINE_NETCAS_SPLITTER_H__
#define __OCF_ENGINE_NETCAS_SPLITTER_H__

#include "ocf/ocf.h"
#include "netcas_common.h"

/**
 * @brief Decide whether to send request to cache or backend
 * @param req The OCF request
 * @return true if request should go to backend, false for cache
 */
bool netcas_should_send_to_backend(struct ocf_request *req);

/**
 * @brief Update the optimal split ratio based on current conditions
 * @param req The OCF request (for context)
 */
void netcas_update_split_ratio(struct ocf_request *req);

/**
 * @brief Initialize the netcas splitter
 */
void netcas_splitter_init(void);

/**
 * @brief Reset all splitter statistics (useful for testing or reconfiguration)
 */
void netcas_reset_splitter(void);

#endif /* __OCF_ENGINE_NETCAS_SPLITTER_H__ */