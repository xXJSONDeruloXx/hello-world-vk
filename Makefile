### Minimal Makefile fallback for building the implicit Vulkan layer without CMake
## Usage:
##   make            # build shared library
##   make install    # install into ~/.local (manifest + .so)
##   make clean

CXX ?= g++
CXXFLAGS ?= -O2 -g -fPIC -std=c++17 -Wall -Wextra -Wno-unused-parameter
INCLUDES = -I/usr/include -I/usr/include/vulkan
LDFLAGS ?= -shared
LDLIBS ?= -lvulkan

LIB = libVK_LAYER_LUNARG_test_vk.so
SRC = layers/hello_layer.cpp

all: $(LIB)

$(LIB): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

install: $(LIB)
	install -Dm755 $(LIB) "$(HOME)/.local/lib/$(LIB)"
	install -Dm644 layers/VK_LAYER_LUNARG_test_vk.json "$(HOME)/.local/share/vulkan/explicit_layer.d/VK_LAYER_LUNARG_test_vk.json"
	@echo "Installed: $(HOME)/.local/lib/$(LIB)"
	@echo "Manifest:  $(HOME)/.local/share/vulkan/explicit_layer.d/VK_LAYER_LUNARG_test_vk.json"

clean:
	rm -f $(LIB)

.PHONY: all install clean
