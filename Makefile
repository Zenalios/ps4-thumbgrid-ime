# ─── ThumbGrid IME GoldHEN Plugin ────────────────────────────────────
# Build with: make
# Clean with: make clean
#
# Requires:
#   OO_PS4_TOOLCHAIN  = path to OpenOrbis-PS4-Toolchain
#   GOLDHEN_SDK       = path to GoldHEN_Plugins_SDK
#   System packages:  clang, lld, llvm (for llvm-ar)
# ─────────────────────────────────────────────────────────────────────

# ─── Validate environment ────────────────────────────────────────────

ifndef OO_PS4_TOOLCHAIN
$(error OO_PS4_TOOLCHAIN is not set)
endif

ifndef GOLDHEN_SDK
$(error GOLDHEN_SDK is not set)
endif

# ─── Project ─────────────────────────────────────────────────────────

PLUGIN_NAME := thumbgrid_ime

# ─── Tools (system clang + OpenOrbis create-fself) ───────────────────

CC       := clang
LD       := ld.lld
FSELF    := $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself

# ─── Directories ─────────────────────────────────────────────────────

SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build
BIN_DIR   := bin

# ─── Sources ─────────────────────────────────────────────────────────

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# ─── Compiler flags (matches GoldHEN SDK build) ─────────────────────

CFLAGS := \
	--target=x86_64-pc-freebsd12-elf \
	-std=c11 \
	-Wall -Wextra \
	-Wno-unused-parameter \
	-Wno-unused-function \
	-fPIC \
	-funwind-tables \
	-D__ORBIS__ \
	-D__PS4__ \
	-DDEBUG=0 \
	-D__USE_KLOG__ \
	-isysroot $(OO_PS4_TOOLCHAIN) \
	-isystem $(OO_PS4_TOOLCHAIN)/include \
	-I$(GOLDHEN_SDK)/include \
	-I$(INC_DIR)

# ─── Linker flags ───────────────────────────────────────────────────

LDFLAGS := \
	-m elf_x86_64 \
	--script $(OO_PS4_TOOLCHAIN)/link.x \
	--eh-frame-hdr \
	-pie \
	-z max-page-size=0x4000 \
	-z common-page-size=0x4000 \
	-L$(OO_PS4_TOOLCHAIN)/lib \
	-L$(GOLDHEN_SDK)

LIBS := \
	$(GOLDHEN_SDK)/libGoldHEN_Hook.a \
	-lkernel \
	-lSceLibcInternal \
	-lSceSysmodule \
	-lScePad \
	-lSceUserService \
	-lSceVideoOut

# CRT object from GoldHEN SDK
CRT := $(GOLDHEN_SDK)/build/crtprx.o

# ─── Build targets ──────────────────────────────────────────────────

.PHONY: all clean info

all: $(BIN_DIR)/$(PLUGIN_NAME).prx
	@echo ""
	@echo "=== Build complete: $(BIN_DIR)/$(PLUGIN_NAME).prx ==="
	@echo ""

# Compile .c -> .o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Link .o -> .elf
$(BUILD_DIR)/$(PLUGIN_NAME).elf: $(CRT) $(OBJS)
	@echo "[LD] $@"
	@$(LD) $(LDFLAGS) $(CRT) $(OBJS) $(LIBS) -o $@

# ELF -> PRX via create-fself
$(BIN_DIR)/$(PLUGIN_NAME).prx: $(BUILD_DIR)/$(PLUGIN_NAME).elf | $(BIN_DIR)
	@echo "[FSELF] $@"
	@$(FSELF) -in $< -out $(BUILD_DIR)/$(PLUGIN_NAME).oelf --lib $@ --paid 0x3800000000000011

# Directories
$(BUILD_DIR):
	@mkdir -p $@

$(BIN_DIR):
	@mkdir -p $@

# Clean
clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned."

# Debug info
info:
	@echo "Plugin:     $(PLUGIN_NAME)"
	@echo "Toolchain:  $(OO_PS4_TOOLCHAIN)"
	@echo "GoldHEN:    $(GOLDHEN_SDK)"
	@echo "CRT:        $(CRT)"
	@echo "FSELF:      $(FSELF)"
	@echo "Sources:    $(SRCS)"
	@echo "Objects:    $(OBJS)"
