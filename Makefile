.PHONY: all debug test install clean changelog-preview changelog

BUILD_DIR ?= build
BUILD_TYPE ?= Release
INSTALL_PREFIX ?= $(HOME)/.ssrx
CHANGELOG_VERSION ?= $(shell cat VERSION)
CHANGELOG_TAG ?= v$(CHANGELOG_VERSION)

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j

debug:
	$(MAKE) all BUILD_TYPE=Debug

test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

install:
	cmake --install $(BUILD_DIR) --prefix $(INSTALL_PREFIX)

clean:
	@if [ -d "$(BUILD_DIR)" ]; then cmake --build "$(BUILD_DIR)" --target clean; fi

changelog-preview:
	git-cliff --unreleased --tag $(CHANGELOG_TAG)

changelog:
	@if [ -f CHANGELOG.md ]; then \
		git-cliff --unreleased --tag $(CHANGELOG_TAG) --prepend CHANGELOG.md; \
	else \
		git-cliff --tag $(CHANGELOG_TAG) --output CHANGELOG.md; \
	fi
