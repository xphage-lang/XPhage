# ============================================================
# X-Phage — cmake-free build (make only)
#
#   make                       transpiler-only compiler  -> build/std/xphage
#   make LLVM=1 LLVM_ROOT=DIR  + native LLVM backend     -> build/llvm/xphage
#   make xprt                  runtime archive           -> xprt/libxprt.a
#   make check                 golden suite (default backend)
#   make check BACKEND=xil     golden suite (xil | llvm | transpiler)
#
# Every object tracks its header dependencies (-MMD), so editing any
# header rebuilds every translation unit that includes it. (A stale
# object silently keeping an old struct default cost real debugging
# time on the hand-built LLVM tree before this file existed.)
#
# LLVM_ROOT is the directory that contains include/llvm-18 and
# lib/llvm-18 (e.g. an extracted llvm-18-dev .deb: <dir>/usr).
# ============================================================

CXX      ?= g++
CC       ?= gcc
BACKEND  ?= transpiler
LLVM     ?= 0
LLVM_ROOT?=

CXXFLAGS ?= -O1
CXXFLAGS += -std=c++17 -Wno-unused-parameter -Wno-unused-function -MMD -MP

INCS := -Iinclude -Ixprt \
        $(addprefix -I,$(wildcard compiler/*/include))

SRCS := $(wildcard src/runtime/*.cpp) $(wildcard compiler/*/src/*.cpp)

ifeq ($(LLVM),1)
  ifeq ($(LLVM_ROOT),)
    $(error LLVM=1 requires LLVM_ROOT=<dir containing include/llvm-18>)
  endif
  BUILD     := build/llvm
  CXXFLAGS  += -DENABLE_LLVM
  INCS      += -I$(LLVM_ROOT)/include/llvm-18 -I$(LLVM_ROOT)/include/llvm-c-18
  LLVM_LIBS := $(wildcard $(LLVM_ROOT)/lib/llvm-18/lib/libLLVM*.a)
  # zstd/xml2/tinfo often lack an unversioned .so (no -dev package);
  # link the versioned runtime objects directly when that is the case.
  SYSLIB = $(firstword $(wildcard /usr/lib/x86_64-linux-gnu/$(1).so /usr/lib/x86_64-linux-gnu/$(1).so.*))
  LDLIBS    := -Wl,--start-group $(LLVM_LIBS) -Wl,--end-group \
               -lz $(call SYSLIB,libzstd) $(call SYSLIB,libxml2) \
               $(call SYSLIB,libtinfo) -lffi -lpthread -lstdc++fs -lm -ldl
else
  BUILD     := build/std
  LDLIBS    := -lpthread -lstdc++fs -lm
endif

OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS)) $(BUILD)/xprt/xprt.o
BIN  := $(BUILD)/xphage

.PHONY: all xprt check clean
all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(OBJS) $(LDLIBS) -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

$(BUILD)/xprt/xprt.o: xprt/xprt.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -MMD -MP -Ixprt -c $< -o $@

# The LLVM backend shells out to `cc ... <xprt_lib>`; interface.cpp looks
# for ./build/xprt/libxprt.a, ./xprt/libxprt.a, ./libxprt.a (relative to
# the CWD of the run), so keep an archive in xprt/.
xprt: xprt/libxprt.a
xprt/libxprt.a: xprt/xprt.c xprt/xprt.h
	$(CC) -O2 -Ixprt -c xprt/xprt.c -o build/xprt_lib.o 2>/dev/null || { mkdir -p build && $(CC) -O2 -Ixprt -c xprt/xprt.c -o build/xprt_lib.o; }
	ar rcs $@ build/xprt_lib.o

check: $(BIN) xprt
	XPHAGE=$(abspath $(BIN)) tests/golden/run_golden.sh --backend=$(BACKEND)

clean:
	rm -rf build xprt/libxprt.a

-include $(OBJS:.o=.d)
