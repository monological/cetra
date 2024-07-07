#
# ┏┓┏┓┏┳┓┳┓┏┓
# ┃ ┣  ┃ ┣┫┣┫
# ┗┛┗┛ ┻ ┛┗┛┗
#
# Cetra Graphics Engine
#

# Color codes
RED = \033[0;31m
GREEN = \033[0;32m
YELLOW = \033[0;33m
BLUE = \033[0;34m
NC = \033[0m # No Color

.PHONY: all clean shaders

# Compiler and linker configurations
CC = gcc
CFLAGS = -Wall -g -std=c11 -DGL_SILENCE_DEPRECATION -D_POSIX_C_SOURCE=200809L 
LDFLAGS =
LIBS = -lm -lpthread

# Define executable extension based on the OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Darwin) # macOS specifics
    HOMEBREW_PREFIX := /opt/homebrew
    CFLAGS += -I$(HOMEBREW_PREFIX)/include
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
    LIBS += -lglfw -lcglm -lglew -lassimp
    LIBS += -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo
endif
ifeq ($(UNAME_S), Linux) # Linux specifics
    LIBS += -lglfw -lcglm -lGLEW -lassimp
    LIBS += -lX11 -lGL -lncurses
	LDFLAGS += -L/lib -L/usr/lib -L/usr/local/lib
endif
ifneq (, $(findstring CYGWIN, $(UNAME_S))$(findstring MINGW, $(UNAME_S))$(findstring MSYS, $(UNAME_S)))
    # Windows specifics
    LIBS += -lglfw -lcglm -lGLEW -lassimp
    LIBS += -lglu32 -lgdi32 -lopengl32 -lkernel32
    CFLAGS += -I/mingw/include
    LDFLAGS += -L/mingw/lib
endif

# Source files and object files
SRC = $(wildcard src/*.c src/ext/*.c)
OBJ = $(SRC:.c=.o)

SHADER_DIR = ./shaders
SHADER_HEADER = ./src/shader_strings.h
SHADER_FILES = $(wildcard $(SHADER_DIR)/*.glsl)

# Define targets
all: libcetra.a shaders 

# Shader header generation depends on all shader files
shaders: $(SHADER_HEADER)

$(SHADER_HEADER): $(SHADER_FILES)
	@echo -e "$(BLUE)Updating shader header file...$(NC)"
	python3 gen_shader_header.py $(SHADER_DIR) $(SHADER_HEADER)

# Build the cetra library
libcetra.a: $(OBJ)
	@echo -e "$(YELLOW)Generating Cetra static library...$(NC)"
	ar rcs $@ $^

# Generic rule for building objects
%.o: %.c $(SHADER_HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean the build
clean:
	@echo -e "$(RED)Cleaning up Cetra...$(NC)"
	rm -f $(OBJ) libcetra.a
	rm -f $(SHADER_HEADER)


