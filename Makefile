### Minimal Makefile fallback for building the implicit Vulkan layer without CMake
## Usage:
##   make            # build shared library
##   make install    # install into ~/.local (manifest + .so)
##   make clean

CXX ?= g++
CXXFLAGS ?= -O2 -g -fPIC -std=c++17 -Wall -Wextra -Wno-unused-parameter -DFFX_OF -include layers/ffx_compat.h -MMD -MP

FFX_SDK_DIR := third_party/FidelityFX-SDK/sdk

INCLUDES = -I/usr/include -I/usr/include/vulkan \
	-I$(FFX_SDK_DIR)/include \
	-I$(FFX_SDK_DIR)/src/backends/shared \
	-I$(FFX_SDK_DIR)/src/shared

LDFLAGS ?= -shared
LDLIBS ?= -lvulkan

LIB = libVK_LAYER_LUNARG_test_vk.so
LAYER_SRC = layers/hello_layer.cpp layers/ffx_framegen_stub.cpp

# Minimal subset of FidelityFX SDK sources needed for Optical Flow (vk backend + shared + opticalflow component)
FFX_SRC = \
	$(FFX_SDK_DIR)/src/backends/vk/ffx_vk.cpp \
	$(FFX_SDK_DIR)/src/backends/shared/ffx_shader_blobs.cpp \
	$(FFX_SDK_DIR)/src/components/opticalflow/ffx_opticalflow.cpp \
	$(FFX_SDK_DIR)/src/shared/ffx_assert.cpp \
	$(FFX_SDK_DIR)/src/shared/ffx_object_management.cpp \
	$(FFX_SDK_DIR)/src/shared/ffx_message.cpp \
	$(FFX_SDK_DIR)/src/shared/ffx_breadcrumbs_list.cpp

SRC = $(LAYER_SRC) $(FFX_SRC)
OBJ = $(SRC:.cpp=.o)
DEPS = $(OBJ:.o=.d)

all: $(LIB)

$(LIB): $(OBJ)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

install: $(LIB)
	install -Dm755 $(LIB) "$(HOME)/.local/lib/$(LIB)"
	install -Dm644 layers/VK_LAYER_LUNARG_test_vk.json "$(HOME)/.local/share/vulkan/explicit_layer.d/VK_LAYER_LUNARG_test_vk.json"
	@echo "Installed: $(HOME)/.local/lib/$(LIB)"
	@echo "Manifest:  $(HOME)/.local/share/vulkan/explicit_layer.d/VK_LAYER_LUNARG_test_vk.json"

clean:
	rm -f $(LIB) $(OBJ) $(DEPS)

-include $(DEPS)

.PHONY: all install clean
