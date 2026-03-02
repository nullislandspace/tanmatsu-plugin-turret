BUILD_DIR := build
PLUGIN_NAME := turrent
PLUGIN_SDK := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))/../tanmatsu-launcher/tools/plugin-sdk
TOOLCHAIN := $(PLUGIN_SDK)/toolchain-plugin.cmake
BADGEDIR := /tmp/mnt
DEST := $(BADGEDIR)/int/plugins

.PHONY: all build clean rebuild install info

all: build

build:
	@if [ -z "$$IDF_PATH" ]; then \
		echo "Error: IDF_PATH not set."; \
		exit 1; \
	fi
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) ..
	@cd $(BUILD_DIR) && make

clean:
	@rm -rf $(BUILD_DIR)

rebuild: clean build

info:
	@if [ -f $(BUILD_DIR)/$(PLUGIN_NAME).plugin ]; then \
		ls -lh $(BUILD_DIR)/$(PLUGIN_NAME).plugin; \
	fi

install: build
	@mkdir -p $(BADGEDIR)
	@badgefs $(BADGEDIR)
	@mkdir -p $(DEST)/$(PLUGIN_NAME)
	@cp $(BUILD_DIR)/$(PLUGIN_NAME).plugin $(DEST)/$(PLUGIN_NAME)/
	@cp plugin.json $(DEST)/$(PLUGIN_NAME)/
	@mkdir -p $(BADGEDIR)/sd/turret
	#@cp sounds/*.mp3 $(BADGEDIR)/sd/turret/ # Disabled for debugging speeds, files already on device
	badgefs -u $(BADGEDIR)
	@echo "Installed to $(DEST)/$(PLUGIN_NAME)/"
