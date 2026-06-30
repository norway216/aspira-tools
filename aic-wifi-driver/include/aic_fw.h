/*
 * AIC8800 USB WiFi Driver - Firmware Management
 *
 * Firmware loading pipeline: chip identification, manifest parsing,
 * SHA256 verification, download, and boot handshake.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_FW_H__
#define __AIC_FW_H__

#include <linux/firmware.h>
#include <linux/types.h>
#include "aic_hci.h"

/* ================================================================== */
/* Chip Identification                                                 */
/* ================================================================== */

enum aic_chip_model {
	AIC_CHIP_UNKNOWN   = 0,
	AIC_CHIP_8800D80   = 1,
	AIC_CHIP_8800DC    = 2,
	AIC_CHIP_8800D80U  = 3,
};

#define AIC_FW_ABI_VERSION  "1.2"

/* ================================================================== */
/* Firmware File Paths                                                 */
/* ================================================================== */

#define AIC_FW_BASE_PATH     "aic8800"
#define AIC_FW_MANIFEST      "manifest.json"
#define AIC_FW_PATCH_BIN     "fw_patch.bin"
#define AIC_FW_WIFI_BIN      "wifi_fw.bin"
#define AIC_FW_RF_BIN        "rf_config.bin"
#define AIC_FW_CALI_BIN      "cali_config.bin"
#define AIC_FW_COUNTRY_TABLE "common/country_power_table.bin"

/* ================================================================== */
/* Firmware Manifest (JSON parsed into binary struct)                  */
/* ================================================================== */

#define AIC_FW_MANIFEST_CHIP_MAX   32
#define AIC_FW_MANIFEST_VER_MAX    32
#define AIC_FW_MANIFEST_ABI_MAX    16
#define AIC_FW_MANIFEST_SHA256_LEN 64
#define AIC_FW_MANIFEST_PATH_MAX   128

struct aic_fw_manifest {
	char   chip[AIC_FW_MANIFEST_CHIP_MAX];
	u16    vendor_id;
	u16    product_id;
	char   driver_abi[AIC_FW_MANIFEST_ABI_MAX];
	char   firmware_version[AIC_FW_MANIFEST_VER_MAX];
	char   min_kernel[AIC_FW_MANIFEST_VER_MAX];

	/* Per-file SHA256 hashes */
	struct {
		char path[AIC_FW_MANIFEST_PATH_MAX];
		char sha256[AIC_FW_MANIFEST_SHA256_LEN];
	} files[8];
	int    file_count;
};

/* ================================================================== */
/* Firmware Subsystem Structure                                        */
/* ================================================================== */

struct aic_fw {
	enum aic_chip_model   chip_model;
	u16                   vendor_id;
	u16                   product_id;

	char                  fw_path[128];
	char                  fw_version[32];
	char                  driver_abi[16];

	/* Loaded firmware blobs */
	const struct firmware *fw_patch;
	const struct firmware *fw_wifi;
	const struct firmware *fw_rf;
	const struct firmware *fw_cali;

	u32                   fw_crc;
	u32                   heartbeat_counter;

	bool                  loaded;
	bool                  verified;
};

/* ================================================================== */
/* Firmware API                                                        */
/* ================================================================== */

int  aic_fw_identify_chip(struct aic_dev *adev);
int  aic_fw_parse_manifest(struct aic_dev *adev, struct aic_fw_manifest *m);
int  aic_fw_verify_manifest(struct aic_dev *adev, struct aic_fw_manifest *m);
int  aic_fw_load_all(struct aic_dev *adev);
int  aic_fw_download_and_boot(struct aic_dev *adev);
int  aic_fw_wait_ready(struct aic_dev *adev, unsigned long timeout_ms);
void aic_fw_release_all(struct aic_dev *adev);
int  aic_fw_soft_reset(struct aic_dev *adev);
bool aic_fw_check_heartbeat(struct aic_dev *adev);

/* Firmware path helpers */
const char *aic_fw_chip_name(enum aic_chip_model chip);
int  aic_fw_build_path(struct aic_dev *adev, const char *filename,
		       char *out, size_t out_len);

#endif /* __AIC_FW_H__ */
