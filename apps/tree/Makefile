# Makefile for the "tree" application in the apps/tree directory
SHELL := /bin/bash

CC = gcc
CFLAGS = -Wall -g -std=c11 -I$(BUILD_DIR)/include
LDFLAGS =
LIBS = -lm -lpthread -lcetra

# Check if BUILD_DIR is set
ifndef BUILD_DIR
$(error BUILD_DIR is not set. Please ensure the environment variable BUILD_DIR is exported from the root Makefile)
endif

CFLAGS += -I$(BUILD_DIR)/include
LDFLAGS += -L$(BUILD_DIR)/lib

# Define executable extension based on the OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Darwin)
	HOMEBREW_PREFIX := /opt/homebrew
    EXE = tree
    CFLAGS += -I$(HOMEBREW_PREFIX)/include
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
    LIBS += -lglfw -lcglm -lglew -lassimp
    LIBS += -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo
endif
ifeq ($(UNAME_S), Linux)
	EXE = tree
    LIBS += -lglfw -lcglm -lGLEW -lassimp
    LIBS += -lX11 -lGL -lncurses
	LDFLAGS += -L/lib -L/usr/lib -L/usr/local/lib
endif
ifneq (, $(findstring CYGWIN, $(UNAME_S))$(findstring MINGW, $(UNAME_S))$(findstring MSYS, $(UNAME_S)))
	EXE = tree.exe
    LIBS += -lglfw -lcglm -lGLEW -lassimp
    LIBS += -lglu32 -lgdi32 -lopengl32 -lkernel32
	CFLAGS += -I/mingw/include
    LDFLAGS += -L/mingw/lib
endif

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
OUT_DIR = $(BUILD_DIR)/bin

all: $(OUT_DIR)/$(EXE)

$(OUT_DIR)/$(EXE): $(OBJ)
	mkdir -p $(OUT_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OUT_DIR)/$(EXE)

# macOS app bundle
APP_NAME = Tree
APP_DIR = $(BUILD_DIR)/$(APP_NAME).app
APP_CONTENTS = $(APP_DIR)/Contents

bundle: $(OUT_DIR)/$(EXE)
ifeq ($(UNAME_S), Darwin)
	@rm -rf $(APP_DIR)
	@mkdir -p $(APP_CONTENTS)/MacOS $(APP_CONTENTS)/Resources $(APP_CONTENTS)/libs
	@cp $(OUT_DIR)/$(EXE) $(APP_CONTENTS)/MacOS/$(EXE)
	@echo '<?xml version="1.0" encoding="UTF-8"?>' > $(APP_CONTENTS)/Info.plist
	@echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' >> $(APP_CONTENTS)/Info.plist
	@echo '<plist version="1.0"><dict>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>CFBundleName</key><string>$(APP_NAME)</string>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>CFBundleDisplayName</key><string>Procedural Tree</string>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>CFBundleIdentifier</key><string>com.cetra.tree</string>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>CFBundleVersion</key><string>1.0</string>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>CFBundleExecutable</key><string>$(EXE)</string>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>CFBundlePackageType</key><string>APPL</string>' >> $(APP_CONTENTS)/Info.plist
	@echo '<key>NSHighResolutionCapable</key><true/>' >> $(APP_CONTENTS)/Info.plist
	@echo '</dict></plist>' >> $(APP_CONTENTS)/Info.plist
	@dylibbundler -od -b -x "$(APP_CONTENTS)/MacOS/$(EXE)" -d "$(APP_CONTENTS)/libs" -p '@executable_path/../libs'
	@echo "Created self-contained $(APP_DIR)"
else
	@echo "App bundles are only supported on macOS"
endif

.PHONY: all clean bundle
