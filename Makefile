BEZEL := python3 bezel.py
OUT   := chromium/src/out
BIN   := builds

# What chrome needs next to it at runtime (from `gn desc <out> //chrome:chrome runtime_deps`,
# minus test-only Vulkan layers). Missing optional files are skipped.
DESKTOP_FILES := \
	chrome_100_percent.pak chrome_200_percent.pak resources.pak \
	icudtl.dat snapshot_blob.bin v8_context_snapshot.bin \
	locales resources MEIPreload IwaKeyDistribution initial_preferences
LINUX_FILES := $(DESKTOP_FILES) \
	chrome chrome_crashpad_handler chrome_sandbox \
	libEGL.so libGLESv2.so libvk_swiftshader.so libvulkan.so.1 vk_swiftshader_icd.json \
	libqt5_shim.so libqt6_shim.so
WINDOWS_FILES := $(DESKTOP_FILES) \
	chrome.exe chrome.dll chrome_elf.dll chrome_proxy.exe chrome_pwa_launcher.exe \
	libEGL.dll libGLESv2.dll vk_swiftshader.dll vulkan-1.dll vk_swiftshader_icd.json \
	d3dcompiler_47.dll

.PHONY: help apply build-linux build-windows build-apk test clean-builds

help:
	@echo "make build-linux    apply + build -> $(BIN)/linux/, install launcher + icons"
	@echo "make build-windows  apply + build -> $(BIN)/windows/ (not maintained)"
	@echo "make build-apk      apply + build -> $(BIN)/android/"
	@echo "make apply          overlay + uBlock + patches (idempotent)"
	@echo "make test           unit tests (1 GB memory cap)"
	@echo "make clean-builds   remove $(BIN)/"

apply:
	$(BEZEL) apply

# $(1): platform, $(2): file list. Replaces $(BIN)/$(1) so no stale files are left behind.
define stage
	rm -rf $(BIN)/$(1)
	mkdir -p $(BIN)/$(1)
	cd $(OUT)/Bezel-$(1) && cp -a $$(ls -d $(2) 2>/dev/null) $(CURDIR)/$(BIN)/$(1)/
	@echo "-> $(BIN)/$(1)/"
endef

build-linux: apply
	$(BEZEL) build linux
	$(call stage,linux,$(LINUX_FILES))
	$(BEZEL) desktop

build-windows: apply
	$(BEZEL) build windows
	$(call stage,windows,$(WINDOWS_FILES))

build-apk: apply
	$(BEZEL) build android
	rm -rf $(BIN)/android
	mkdir -p $(BIN)/android
	cp $(OUT)/Bezel-android/apks/*.apk $(BIN)/android/
	@echo "-> $(BIN)/android/"

test:
	systemd-run --user --scope -p MemoryMax=1G -p MemorySwapMax=0 --quiet \
		python3 -m unittest discover -s tests

clean-builds:
	rm -rf $(BIN)
