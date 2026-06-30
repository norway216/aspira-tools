# AIC8800 USB WiFi Driver — Firmware Installation Guide

## Directory Structure

The firmware should be placed at:

```
/lib/firmware/aic8800/
├── manifest.json                  # Firmware manifest (REQUIRED)
├── aic8800d80/
│   ├── fw_patch.bin               # Patch/ramcode binary
│   ├── wifi_fw.bin                # WiFi firmware binary
│   ├── rf_config.bin              # RF calibration data (optional)
│   └── cali_config.bin            # Factory calibration (optional)
├── aic8800dc/
│   ├── fw_patch.bin
│   ├── wifi_fw.bin
│   └── rf_config.bin
└── common/
    └── country_power_table.bin    # Regulatory power table
```

## Firmware Sources

The actual firmware binary files (fw_patch.bin, wifi_fw.bin, etc.) are
chip vendor proprietary and MUST be obtained from:

1. The chip vendor SDK / BSP package
2. Your hardware module supplier
3. The original driver package (often in a `firmware/` subdirectory)

Common sources for AIC8800 firmware:
- https://github.com/radxa-pkg/aic8800 (Radxa packages)
- https://github.com/shenmintao/aic8800d80 (Community mirror)
- Gentoo package: net-wireless/aic8800

## Manifest Format (manifest.json)

```json
{
  "chip": "AIC8800D80",
  "vendor_id": "0xa69c",
  "product_id": "0x5721",
  "driver_abi": "1.2",
  "firmware_version": "2026.06.30",
  "min_kernel": "5.10",
  "sha256": {
    "wifi_fw.bin": "0000000000000000000000000000000000000000000000000000000000000000",
    "rf_config.bin": "0000000000000000000000000000000000000000000000000000000000000000"
  }
}
```

Update the SHA256 hashes with actual values by running:
```bash
sha256sum /lib/firmware/aic8800/aic8800d80/wifi_fw.bin
sha256sum /lib/firmware/aic8800/aic8800d80/rf_config.bin
```

## Verifying Firmware

After installation, verify the firmware is detected:
```bash
ls -la /lib/firmware/aic8800/aic8800d80/
cat /lib/firmware/aic8800/manifest.json
```

Check driver logs after module load:
```bash
dmesg | grep -i aic | grep -i firmware
```

## Troubleshooting

If you see "firmware not found" errors:
1. Verify the exact chip model (check `lsusb` for VID/PID)
2. Verify the firmware path matches the driver's expected path:
   `aic8800/<chip_name_lowercase>/`
3. Run `depmod -a` after installing firmware
4. Check file permissions (should be readable by root)
