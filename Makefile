BUILD_DIR := build
PLUGIN_NAME := turret
APP_SLUG_NAME := at.cavac.turret
PLUGIN_SDK := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))/../tanmatsu-launcher/tools/plugin-sdk
TOOLCHAIN := $(PLUGIN_SDK)/toolchain-plugin.cmake
BADGEDIR := /tmp/mnt
DEST := $(BADGEDIR)/int/plugins

.PHONY: all build clean rebuild install info apprepo

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
	@mkdir -p $(DEST)/$(APP_SLUG_NAME)
	@cp $(BUILD_DIR)/$(PLUGIN_NAME).plugin $(DEST)/$(APP_SLUG_NAME)/
	@cp metadata/plugin.json $(DEST)/$(APP_SLUG_NAME)/
	@cp metadata/startup_*.mp3 $(DEST)/$(APP_SLUG_NAME)/
	@cp metadata/standby_*.mp3 $(DEST)/$(APP_SLUG_NAME)/
	badgefs -u $(BADGEDIR)
	@echo "Installed to $(DEST)/$(APP_SLUG_NAME)/"

APP_REPO_PATH ?= ../tanmatsu-app-repository/$(APP_SLUG_NAME)

apprepo: build
	@echo "=== Updating app repository ==="
	mkdir -p $(APP_REPO_PATH)
	cp metadata/metadata.json $(APP_REPO_PATH)/metadata.json
	cp metadata/plugin.json $(APP_REPO_PATH)/plugin.json
	cp metadata/startup_*.mp3 $(APP_REPO_PATH)/
	cp metadata/standby_*.mp3 $(APP_REPO_PATH)/
	cp $(BUILD_DIR)/$(PLUGIN_NAME).plugin $(APP_REPO_PATH)/$(PLUGIN_NAME).plugin
	@echo "=== App repository updated at $(APP_REPO_PATH) ==="
