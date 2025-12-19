###############################################
# Unit test makefile (tests/makefile)
#  - ビルド済みの lib/libcan.{a,so} に対するユニットテスト
#  - ラッパ(-Wl,-wrap,...)でシステムコール等をモック
#  - 成果物: builds/tests/test_can_manager
###############################################

MKFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT       := $(abspath $(MKFILE_DIR)/..)

INCDIR ?= $(ROOT)/include
LIBDIR ?= $(ROOT)/lib
OBJDIR ?= $(ROOT)/builds/tests/obj
OUTDIR ?= $(ROOT)/builds/tests

CC     ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -fPIC -MMD -MP
CPPFLAGS ?= -I$(INCDIR) -I$(ROOT)/src/can -I$(MKFILE_DIR)/src
LDLIBS  ?= -pthread

TEST_BIN := $(OUTDIR)/test_can_manager

SRCS := $(MKFILE_DIR)/src/test_can_manager.c \
        $(MKFILE_DIR)/src/mocks.c
OBJS := $(patsubst $(MKFILE_DIR)/src/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# ラップ対象シンボル（必要に応じて拡張）
WRAP_SYMS := socket setsockopt fcntl ioctl bind close \
             timerfd_create timerfd_settime \
             can_tx_start can_rx_start can_tx_stop can_rx_stop

# make でカンマを扱うためのヘルパ
comma := ,
WRAP_FLAGS := $(foreach s,$(WRAP_SYMS),-Wl$(comma)-wrap$(comma)$(s))

.PHONY: all test run clean dirs

all: test

test: $(TEST_BIN)

run: test
	$(TEST_BIN)

dirs:
	@mkdir -p $(OBJDIR) $(OUTDIR)

# まずライブラリをビルド
$(LIBDIR)/libcan.a:
	$(MAKE) -C $(ROOT) libs

$(OBJDIR)/%.o: $(MKFILE_DIR)/src/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(OBJS) $(LIBDIR)/libcan.a | dirs
	$(CC) -o $@ $(WRAP_FLAGS) $(OBJS) $(LIBDIR)/libcan.a $(LDLIBS)

-include $(DEPS)

clean:
	@$(RM) -r $(ROOT)/builds/tests
