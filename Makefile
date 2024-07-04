#
# ┏┓┏┓┏┳┓┳┓┏┓
# ┃ ┣  ┃ ┣┫┣┫
# ┗┛┗┛ ┻ ┛┗┛┗
#
# Build system for the Cetra Engine
#

# Color codes
RED = \033[0;31m
GREEN = \033[0;32m
YELLOW = \033[0;33m
BLUE = \033[0;34m
NC = \033[0m # No Color

.PHONY: all cetra apps clean lib headers

BUILD_DIR := ./build

export BUILD_DIR

all: cetra apps

cetra:
	@echo "$(GREEN)Building Cetra Engine...$(NC)"
	$(MAKE) -C cetra
	@echo "$(BLUE)Copying Cetra outputs...$(NC)"
	$(MAKE) lib
	$(MAKE) headers

apps:
	@echo "$(YELLOW)Building apps...$(NC)"
	@for dir in apps/*; do \
		if [ -f $$dir/Makefile ]; then \
			echo "$(BLUE)Building in $$dir...$(NC)"; \
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
	@echo "$(RED)Cleaning up...$(NC)"
	$(MAKE) -C cetra clean
	@for dir in apps/*; do \
		if [ -f $$dir/Makefile ]; then \
			$(MAKE) -C $$dir clean; \
		fi \
	done
	rm -rf $(BUILD_DIR)

