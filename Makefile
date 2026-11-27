ENCLAVE := python3 enclave.py
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

.PHONY: help apply stage-linux build-linux package-linux build-windows build-apk test clean-builds

help:
	@echo "make build-linux    apply + build -> $(BIN)/linux/, install launcher + icons"
	@echo "make package-linux  apply + build -> $(BIN)/packages/ (PKG=deb rpm archlinux)"
	@echo "make build-windows  apply + build -> $(BIN)/windows/ (not maintained)"
	@echo "make build-apk      apply + build -> $(BIN)/android/"
	@echo "make apply          overlay + uBlock + patches (idempotent)"
	@echo "make test           unit tests (1 GB memory cap)"
	@echo "make clean-builds   remove $(BIN)/"

apply:
	$(ENCLAVE) apply

# $(1): platform, $(2): file list. Replaces $(BIN)/$(1) so no stale files are left behind.
define stage
	rm -rf $(BIN)/$(1)
	mkdir -p $(BIN)/$(1)
	cd $(OUT)/Enclave-$(1) && cp -a $$(ls -d $(2) 2>/dev/null) $(CURDIR)/$(BIN)/$(1)/
	@echo "-> $(BIN)/$(1)/"
endef

stage-linux: apply
	$(ENCLAVE) build linux
	$(call stage,linux,$(LINUX_FILES))

# Installs the launcher and icons for this build into ~/.local/share. Remove them
# (applications/enclave.desktop, icons/hicolor/*/apps/enclave.*) after switching to the
# package: user-level files hide the packaged ones.
build-linux: stage-linux
	$(ENCLAVE) desktop

# Linux packages from builds/linux, described in packaging/linux/nfpm.yaml.
# PKG picks the formats (deb, rpm, archlinux; all three by default), e.g.
#   make package-linux PKG=archlinux
# The version is the Chromium base version. nfpm runs through `go run` unless installed.
PKG ?= deb rpm archlinux
NFPM ?= $(or $(shell command -v nfpm 2>/dev/null),go run github.com/goreleaser/nfpm/v2/cmd/nfpm@v2.49.0)

package-linux: stage-linux
	$(ENCLAVE) desktop-entry --binary /opt/selfhold/enclave/chrome > $(BIN)/enclave.desktop
	mkdir -p $(BIN)/packages
	@for p in $(PKG); do \
		PKG_VERSION=$$(cat chromium_version.txt) $(NFPM) package -f packaging/linux/nfpm.yaml -p $$p -t $(BIN)/packages/ || exit 1; \
	done

build-windows: apply
	$(ENCLAVE) build windows
	$(call stage,windows,$(WINDOWS_FILES))

build-apk: apply
	$(ENCLAVE) build android
	rm -rf $(BIN)/android
	mkdir -p $(BIN)/android
	cp $(OUT)/Enclave-android/apks/*.apk $(BIN)/android/
	@echo "-> $(BIN)/android/"

test:
	systemd-run --user --scope -p MemoryMax=1G -p MemorySwapMax=0 --quiet \
		python3 -m unittest discover -s tests

clean-builds:
	rm -rf $(BIN)
