###############################################
# CAN module makefile (src/can/makefile)
#  - src/can 以下の .c を自動検出してビルド
#  - builds/obj/can に .o/.d を生成
#  - lib/libcan.a (静的) と lib/libcan.so (共有) を作成
#  - 将来ファイルが増えても自動で追従
###############################################

# この makefile の所在からパスを安全に解決
MKFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT       := $(abspath $(MKFILE_DIR)/../..)

# ディレクトリ設定
SRCDIR ?= $(MKFILE_DIR)
INCDIR ?= $(ROOT)/include
OBJDIR ?= $(ROOT)/builds/obj/can
LIBDIR ?= $(ROOT)/lib

# ツール
CC     ?= cc
AR     ?= ar
RANLIB ?= ranlib

# フラグ
CFLAGS   ?= -O2 -g -Wall -Wextra -fPIC
CPPFLAGS ?= -I$(INCDIR)
# 依存関係ファイルを自動生成
CFLAGS   += -MMD -MP

# 検出されたソースと派生オブジェクト
SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

# ライブラリ名
LIBNAME := can
STATIC  := $(LIBDIR)/lib$(LIBNAME).a
SHARED  := $(LIBDIR)/lib$(LIBNAME).so

.PHONY: all objs static shared clean distclean dirs

all: static shared

# 個別オブジェクトのビルドルール
$(OBJDIR)/%.o: $(SRCDIR)/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# 依存関係を取り込む（初回はファイルが無くてもOK）
-include $(DEPS)

objs: $(OBJECTS)

static: $(STATIC)
$(STATIC): $(OBJECTS) | dirs
	$(AR) rcs $@ $(OBJECTS)
	@$(RANLIB) $@ 2>/dev/null || true

shared: $(SHARED)
$(SHARED): $(OBJECTS) | dirs
	$(CC) -shared -o $@ $(OBJECTS)

dirs:
	@mkdir -p $(OBJDIR) $(LIBDIR)

clean:
	@$(RM) -r $(OBJDIR)

distclean: clean
	@$(RM) -f $(STATIC) $(SHARED)

# 参考:
#  - オブジェクトのみ:   make -f src/can/makefile objs
#  - 静的ライブラリ:     make -f src/can/makefile static
#  - 共有ライブラリ:     make -f src/can/makefile shared
#  - 全部(静的+共有):    make -f src/can/makefile
