
#
# ┏┓┏┓┏┳┓┳┓┏┓
# ┃ ┣  ┃ ┣┫┣┫
# ┗┛┗┛ ┻ ┛┗┛┗
#
# Build system for the Cetra Engine
#

.PHONY: all cetra apps clean lib headers

BUILD_DIR := ./build

export BUILD_DIR

all: cetra apps

cetra:
	@echo "Building Cetra Engine..."
	$(MAKE) -C cetra
	@echo "Copying Cetra outputs..."
	$(MAKE) lib
	$(MAKE) headers

apps:
	@echo "Building apps..."
	@for dir in apps/*; do \
		if [ -f $$dir/Makefile ]; then \
			echo "Building in $$dir..."; \
			$(MAKE) -C $$dir BUILD_DIR=$(CURDIR)/build; \
		fi \
	done

lib:
	@mkdir -p $(BUILD_DIR)/lib
	cp cetra/libcetra.a $(BUILD_DIR)/lib/

headers:
	@mkdir -p $(BUILD_DIR)/include/cetra
	rsync -q -av --include='*/' --include='*.h' --exclude='*' --prune-empty-dirs cetra/src/ $(BUILD_DIR)/include/cetra/

clean:
	@echo "Cleaning up..."
	$(MAKE) -C cetra clean
	@for dir in apps/*; do \
		if [ -f $$dir/Makefile ]; then \
			$(MAKE) -C $$dir clean; \
		fi \
	done
	rm -rf $(BUILD_DIR)

