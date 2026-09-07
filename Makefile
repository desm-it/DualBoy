BUILD_DIR ?= build
BUILD_TYPE ?= RelWithDebInfo
CMAKE ?= cmake
CTEST ?= ctest
NINJA ?= ninja
DOCKER ?= docker
LINUX_IMAGE ?= dualboy-linux-x86_64:bookworm
LINUX_BUILD_DIR ?= build-linux-x86_64
LINUX_NATIVE_IMAGE ?= dualboy-linux-native:bookworm
LINUX_NATIVE_ASAN_BUILD_DIR ?= build-linux-native-asan

.PHONY: all configure test clean linux-image linux-x86_64 test-linux-x86_64 \
	linux-native-image asan-linux-native asan-linux-x86_64 \
	symbols-linux-x86_64

all: $(BUILD_DIR)/build.ninja
	$(CMAKE) --build $(BUILD_DIR)

configure: $(BUILD_DIR)/build.ninja

$(BUILD_DIR)/build.ninja: CMakeLists.txt
	$(CMAKE) -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

test: all
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

linux-image:
	$(DOCKER) build --platform linux/amd64 \
		-t $(LINUX_IMAGE) tools/linux-x86_64

linux-native-image:
	$(DOCKER) build \
		-t $(LINUX_NATIVE_IMAGE) tools/linux-x86_64

linux-x86_64: linux-image
	$(DOCKER) run --rm --platform linux/amd64 \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):/src" -w /src $(LINUX_IMAGE) \
		cmake -S . -B $(LINUX_BUILD_DIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DDUALBOY_WARNINGS_AS_ERRORS=ON
	$(DOCKER) run --rm --platform linux/amd64 \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):/src" -w /src $(LINUX_IMAGE) \
		cmake --build $(LINUX_BUILD_DIR) --parallel 2

test-linux-x86_64: linux-x86_64
	$(DOCKER) run --rm --platform linux/amd64 \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):/src" -w /src $(LINUX_IMAGE) \
		ctest --test-dir $(LINUX_BUILD_DIR) --output-on-failure

asan-linux-x86_64: linux-image
	$(DOCKER) run --rm --platform linux/amd64 \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):/src" -w /src $(LINUX_IMAGE) \
		sh -eu -c 'cmake -S . -B build-linux-x86_64-asan -G Ninja \
		-DCMAKE_BUILD_TYPE=Debug -DDUALBOY_ENABLE_ASAN=ON && \
		cmake --build build-linux-x86_64-asan --parallel 2 && \
		ctest --test-dir build-linux-x86_64-asan --output-on-failure'

# ASan requires native virtual-address-space semantics. This target is the
# portable sanitizer gate on hosts where linux/amd64 itself is emulated.
asan-linux-native: linux-native-image
	$(DOCKER) run --rm \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):/src" -w /src $(LINUX_NATIVE_IMAGE) \
		sh -eu -c 'cmake -S . -B $(LINUX_NATIVE_ASAN_BUILD_DIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=Debug -DDUALBOY_ENABLE_ASAN=ON && \
		cmake --build $(LINUX_NATIVE_ASAN_BUILD_DIR) --parallel 2 && \
		ctest --test-dir $(LINUX_NATIVE_ASAN_BUILD_DIR) --output-on-failure'

symbols-linux-x86_64: linux-x86_64
	$(DOCKER) run --rm --platform linux/amd64 \
		-v "$(CURDIR):/src:ro" -w /src $(LINUX_IMAGE) \
		sh -eu tools/check-libretro-symbols.sh \
		$(LINUX_BUILD_DIR)/dualboy_libretro.so
