# Root makefile
# - src/main.c -> builds/obj/main.o
# - link with lib/libcan.a (static) or lib/libcan.so (shared)
# - purge: remove builds/ and lib/

ROOT      := $(abspath .)
INCDIR    ?= $(ROOT)/include
OBJDIR    ?= $(ROOT)/builds/obj
LIBDIR    ?= $(ROOT)/lib

CANMK     := $(ROOT)/src/can/makefile

CC        ?= cc
CFLAGS    ?= -O2 -g -Wall -Wextra
CPPFLAGS  ?= -I$(INCDIR)
# 依存関係の自動生成
CFLAGS    += -MMD -MP

# 実行ファイル名
APP_NAME  ?= cansocket
APP       := $(ROOT)/builds/$(APP_NAME)

# ライブラリ
LIBCAN_A  := $(LIBDIR)/libcan.a
LIBCAN_SO := $(LIBDIR)/libcan.so

# メインソース/オブジェクト
MAIN_SRC  := $(ROOT)/src/main.c
MAIN_OBJ  := $(OBJDIR)/main.o
MAIN_DEP  := $(OBJDIR)/main.d

# pthread が必要
LDLIBS    += -pthread

.PHONY: all app app-static app-shared libs dirs clean help

all: app

## リンク方式の既定（上書き可: make LINK_MODE=shared）
LINK_MODE ?= static

# 一貫した単一ターゲット: app → $(APP)
app: $(APP)

# 明示的なラッパー（同一バイナリを生成、方式のみ切替）
app-static:
	$(MAKE) LINK_MODE=static app

app-shared:
	$(MAKE) LINK_MODE=shared app

# 条件付きでリンク先ライブラリ・コマンドを切替
ifeq ($(LINK_MODE),shared)
$(APP): $(MAIN_OBJ) $(LIBCAN_SO) | dirs
	$(CC) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/../lib' -o $@ $(MAIN_OBJ) -L$(LIBDIR) -lcan $(LDLIBS)
else
$(APP): $(MAIN_OBJ) $(LIBCAN_A) | dirs
	$(CC) $(LDFLAGS) -o $@ $(MAIN_OBJ) $(LIBCAN_A) $(LDLIBS)
endif

libs: $(LIBCAN_A) $(LIBCAN_SO)

# ライブラリ生成はサブmakeに委譲
$(LIBCAN_A):
	$(MAKE) -f $(CANMK) static

$(LIBCAN_SO):
	$(MAKE) -f $(CANMK) shared

# main.o のビルド
$(MAIN_OBJ): $(MAIN_SRC) | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(MAIN_DEP)

dirs:
	@mkdir -p $(OBJDIR) $(LIBDIR) $(dir $(APP))

# 依頼通り、builds/ と lib/ を丸ごと削除
clean:
	@$(RM) -r $(ROOT)/builds $(ROOT)/lib

# ヘルプ
help:
	@echo "make [LINK_MODE=static|shared]          # 実行ファイルを作成 (既定: static)"
	@echo "make app-static | app-shared            # 明示的にリンク方式を指定"
	@echo "make libs                               # lib/libcan.{a,so} を作成"
	@echo "make clean                              # builds/ と lib/ を削除"
