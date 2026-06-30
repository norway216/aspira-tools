# AIC8800 USB WiFi Driver — Top-Level Makefile
#
# Builds the out-of-tree kernel module against the running kernel.
#
# Usage:
#   make                    Build the module
#   make clean              Clean build artifacts
#   make install            Install module and firmware (requires root)
#   make check              Run static analysis (sparse/cppcheck)
#   make dkms-install       Install via DKMS
#   make dkms-remove        Remove DKMS installation
#

MODNAME  := aic8800_fdrv
KERNEL   := $(shell uname -r)
KERN_DIR ?= /lib/modules/$(KERNEL)/build
PWD      := $(shell pwd)
INSTALL_MOD_PATH ?=

# Installation paths
MOD_DIR   := /lib/modules/$(KERNEL)/kernel/drivers/net/wireless/aic8800
FW_DIR    := /lib/firmware/aic8800
CONF_DIR  := /etc/modprobe.d
UDEV_DIR  := /etc/udev/rules.d

# Default target
.PHONY: all
all: modules

# Kernel build invocation
.PHONY: modules
modules:
	@echo "==> Building $(MODNAME).ko for kernel $(KERNEL)"
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules

# Clean build artifacts
.PHONY: clean
clean:
	@echo "==> Cleaning $(MODNAME)"
	$(MAKE) -C $(KERN_DIR) M=$(PWD) clean
	@rm -f modules.order Module.symvers *.o *.ko *.mod.c .*.cmd
	@find . -name '*.o' -delete
	@find . -name '*.ko' -delete
	@find . -name '*.mod.c' -delete
	@find . -name '.*.cmd' -delete
	@find . -name '*.mod' -delete

# Install module, firmware, and config
.PHONY: install
install: modules
	@echo "==> Installing $(MODNAME).ko to $(MOD_DIR)"
	install -d $(INSTALL_MOD_PATH)$(MOD_DIR)
	install -m 644 $(MODNAME).ko $(INSTALL_MOD_PATH)$(MOD_DIR)/
	@echo "==> Running depmod"
	depmod -a $(KERNEL)
	@echo "==> Installing firmware to $(FW_DIR)"
	mkdir -p $(INSTALL_MOD_PATH)$(FW_DIR)
	cp -r firmware/* $(INSTALL_MOD_PATH)$(FW_DIR)/
	@echo "==> Installing modprobe config"
	install -d $(INSTALL_MOD_PATH)$(CONF_DIR)
	install -m 644 modprobe.d/aic8800.conf $(INSTALL_MOD_PATH)$(CONF_DIR)/
	@echo "==> Installing udev rules"
	install -d $(INSTALL_MOD_PATH)$(UDEV_DIR)
	install -m 644 udev/99-aic8800.rules $(INSTALL_MOD_PATH)$(UDEV_DIR)/
	@echo "==> Installation complete"
	@echo "    Load with: modprobe $(MODNAME)"

# Uninstall
.PHONY: uninstall
uninstall:
	@echo "==> Uninstalling $(MODNAME)"
	rm -f $(MOD_DIR)/$(MODNAME).ko
	depmod -a $(KERNEL)
	rm -rf $(FW_DIR)
	rm -f $(CONF_DIR)/aic8800.conf
	rm -f $(UDEV_DIR)/99-aic8800.rules

# Module information
.PHONY: info
info:
	@echo "Module:    $(MODNAME)"
	@echo "Kernel:    $(KERNEL)"
	@echo "Kernel Dir: $(KERN_DIR)"
	@echo "Build Dir:  $(PWD)"
	modinfo $(MODNAME).ko 2>/dev/null || echo "(module not yet built)"

# Static analysis with sparse (if available)
.PHONY: check
check:
	@echo "==> Running sparse"
	$(MAKE) -C $(KERN_DIR) M=$(PWD) C=2 CF="-Wall -Wsparse-all" modules 2>&1 | \
		grep -v "warning: context imbalance" || true

# DKMS integration
DKMS_NAME := aic8800
DKMS_VER  := 1.0.0

.PHONY: dkms-install
dkms-install:
	@echo "==> Installing via DKMS"
	dkms add $(PWD) --name $(DKMS_NAME) --version $(DKMS_VER) || true
	dkms build $(DKMS_NAME)/$(DKMS_VER) || true
	dkms install $(DKMS_NAME)/$(DKMS_VER) || true

.PHONY: dkms-remove
dkms-remove:
	@echo "==> Removing DKMS module"
	dkms remove $(DKMS_NAME)/$(DKMS_VER) --all || true

# Help
.PHONY: help
help:
	@echo "AIC8800 USB WiFi Driver Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              Build kernel module"
	@echo "  make clean        Remove build artifacts"
	@echo "  make install      Install module + firmware + config"
	@echo "  make uninstall    Remove installation"
	@echo "  make info         Show module info"
	@echo "  make check        Run static analysis (sparse)"
	@echo "  make dkms-install Install via DKMS"
	@echo "  make dkms-remove  Remove DKMS installation"
	@echo "  make help         This help"
	@echo ""
	@echo "Module Parameters (in /etc/modprobe.d/aic8800.conf):"
	@echo "  log_level=2       0=ERR 1=WARN 2=INFO 3=DEBUG 4=TRACE"
	@echo "  rx_urb_num=32     Number of RX URBs (8-64)"
	@echo "  tx_urb_num=32     Number of TX URBs (8-64)"
	@echo "  disable_usb_autosuspend=1  0=allow  1=disable"
	@echo "  power_save=0      0=off  1=on"
	@echo "  low_latency=1     0=off  1=on"
	@echo "  recovery_enable=1 0=off  1=on"
	@echo "  firmware_verify=1 0=skip  1=verify"
