BUILD_DIR ?= build
BUILD_TYPE ?= Release

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j$$(nproc)
	@echo "VPK: $(BUILD_DIR)/red_viper.vpk"

debug:
	$(MAKE) BUILD_TYPE=Debug

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all debug clean
