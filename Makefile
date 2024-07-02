# Root Makefile

.PHONY: all cetra apps clean copy_lib copy_headers

# Define the build directory relative to the root
BUILD_DIR := ./build

export BUILD_DIR  # Exporting BUILD_DIR to be globally available

all: cetra apps

cetra:
	@echo "Building the cetra..."
	$(MAKE) -C cetra
	@echo "Copying cetra outputs..."
	$(MAKE) copy_lib
	$(MAKE) copy_headers

# Build applications, passing BUILD_DIR explicitly
apps:
	@echo "Building applications..."
	@for dir in apps/*; do \
		if [ -f $$dir/Makefile ]; then \
			echo "Building in $$dir..."; \
			$(MAKE) -C $$dir BUILD_DIR=$(CURDIR)/build; \
		fi \
	done

# Copy the static library and headers to the central build directory
copy_lib:
	mkdir -p $(BUILD_DIR)/lib
	cp cetra/libcetra.a $(BUILD_DIR)/lib/

copy_headers:
	mkdir -p $(BUILD_DIR)/include/cetra
	rsync -av --include='*/' --include='*.h' --exclude='*' --prune-empty-dirs cetra/src/ $(BUILD_DIR)/include/cetra/

# Clean up the build
clean:
	@echo "Cleaning up..."
	$(MAKE) -C cetra clean
	@for dir in apps/*; do \
		if [ -f $$dir/Makefile ]; then \
			$(MAKE) -C $$dir clean; \
		fi \
	done
	rm -rf $(BUILD_DIR)  # Clean up the central build directory
