# Makefile for the "shapes" application in the apps/shapes directory

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
    EXE = shapes
    CFLAGS += -I$(HOMEBREW_PREFIX)/include
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
    LIBS += -lglfw -lcglm -lglew -lassimp
    LIBS += -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo
endif
ifeq ($(UNAME_S), Linux)
	EXE = shapes
    LIBS += -lglfw -lcglm -lGLEW -lassimp
    LIBS += -lX11 -lGL -lncurses
	LDFLAGS += -L/lib -L/usr/lib -L/usr/local/lib
endif
ifneq (, $(findstring CYGWIN, $(UNAME_S))$(findstring MINGW, $(UNAME_S))$(findstring MSYS, $(UNAME_S)))
	EXE = shapes.exe
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

.PHONY: all clean

