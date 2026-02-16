# ─── Shell Overlay - SceShellUI PUI Bridge ─────────────────────────
# Loaded into SceShellUI process via sys_sdk_proc_prx_load.
# Attaches to Mono runtime and creates PUI overlay widgets.
# ───────────────────────────────────────────────────────────────────

ifndef OO_PS4_TOOLCHAIN
$(error OO_PS4_TOOLCHAIN is not set)
endif

ifndef GOLDHEN_SDK
$(error GOLDHEN_SDK is not set)
endif

# ─── Project ─────────────────────────────────────────────────────

PLUGIN_NAME := shell_overlay

# ─── Tools ───────────────────────────────────────────────────────

CC       := clang
LD       := ld.lld
FSELF    := $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself

# ─── Directories ─────────────────────────────────────────────────

SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := bin

# ─── Sources ─────────────────────────────────────────────────────

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# ─── Compiler flags ─────────────────────────────────────────────

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
	-isysroot $(OO_PS4_TOOLCHAIN) \
	-isystem $(OO_PS4_TOOLCHAIN)/include \
	-I$(GOLDHEN_SDK)/include \
	-I../include

# ─── Linker flags ───────────────────────────────────────────────

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
	-lkernel \
	-lSceLibcInternal \
	-lmonosgen

CRT := $(GOLDHEN_SDK)/build/crtprx.o

# ─── Build targets ──────────────────────────────────────────────

.PHONY: all clean

all: $(BIN_DIR)/$(PLUGIN_NAME).prx
	@echo ""
	@echo "=== Build complete: $(BIN_DIR)/$(PLUGIN_NAME).prx ==="
	@echo ""

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PLUGIN_NAME).elf: $(CRT) $(OBJS)
	@echo "[LD] $@"
	@$(LD) $(LDFLAGS) $(CRT) $(OBJS) $(LIBS) -o $@

$(BIN_DIR)/$(PLUGIN_NAME).prx: $(BUILD_DIR)/$(PLUGIN_NAME).elf | $(BIN_DIR)
	@echo "[FSELF] $@"
	@$(FSELF) -in $< -out $(BUILD_DIR)/$(PLUGIN_NAME).oelf --lib $@ --paid 0x3800000000000011

$(BUILD_DIR):
	@mkdir -p $@

$(BIN_DIR):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned."
