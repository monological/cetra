# Compiler and linker configurations
CC = gcc
CFLAGS = -Wall -g -std=c11
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
    EXE =
endif
ifeq ($(UNAME_S), Linux) # Linux specifics
    EXE =
    LIBS += -lglfw -lcglm -lglew -lassimp
    LIBS += -lX11 -lGL -lncurses
endif
ifneq (, $(findstring CYGWIN, $(UNAME_S))$(findstring MINGW, $(UNAME_S))$(findstring MSYS, $(UNAME_S)))
    # Windows specifics
    EXE = .exe
    LIBS += -lglfw -lcglm -lglew -lassimp
    LIBS += -lglu32 -lgdi32 -lopengl32 -lkernel32
    CFLAGS += -I/mingw/include
    LDFLAGS += -L/mingw/lib
endif

# Source files and object files
SRC = $(wildcard src/*.c src/apps/*.c src/ext/*.c)
OBJ = $(SRC:.c=.o)

SHADER_DIR = ./src/shaders
SHADER_HEADER = ./src/shader_strings.h
SHADER_FILES = $(wildcard $(SHADER_DIR)/*.glsl)

# Define targets
all: shaders render$(EXE) shapes$(EXE) pcb$(EXE)

# Shader header generation depends on all shader files
shaders: $(SHADER_HEADER)

$(SHADER_HEADER): $(SHADER_FILES)
	python3 gen_shader_header.py $(SHADER_DIR) $(SHADER_HEADER)

# Build the applications
render$(EXE): src/apps/render.o libengine.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

shapes$(EXE): src/apps/shapes.o libengine.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

pcb$(EXE): src/apps/pcb/pcb.o libengine.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Build the engine library
libengine.a: $(filter-out src/apps/%.o, $(OBJ))
	ar rcs $@ $^

# Generic rule for building objects
%.o: %.c $(SHADER_HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean the build
clean:
	rm -f $(OBJ) render$(EXE) shapes$(EXE) pcb$(EXE) libengine.a
	rm -f $(SHADER_HEADER)

# Additional commands for copying resources
resources:
	cp -r models textures src/shaders $(BUILD_DIR)

.PHONY: all clean resources shaders
