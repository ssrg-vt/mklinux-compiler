##########################################################################
# Copyright (c) 2012, 2013, 2014, 2015, SSRG Virginia Tech
# Copyright (c) 2007, 2008, 2009, ETH Zurich.
# All rights reserved.
#
# This file is distributed under the terms in the attached LICENSE file.
# If you do not find this file, copies can be found by writing to:
# ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
##########################################################################

POPCORN = /usr/local/popcorn

CC     = $(POPCORN)/bin/clang
CFLAGS = -O0 -Wall -g -gdwarf-aranges -DPOSIX -fno-stack-protector \
         -ffunction-sections -fdata-sections -fno-vectorize \
         -Wno-override-module -mllvm -optimize-regalloc -nostdinc

OPT       = $(POPCORN)/bin/opt
OPT_FLAGS = -O3 -disable-loop-vectorization -disable-slp-vectorization \
            -load $(POPCORN)/lib/libAssociateLiteral.so -associate-literal \
            -load $(POPCORN)/lib/libSectionStatic.so -section-static \
            -load $(POPCORN)/lib/libStackInfo.so -stack-info

SRC = kmp.c linux_backend.c omp.c parallel.c popcorn_threadpool.c \
      processing.c self_info.c
OBJ = $(SRC:.c=.o)

# aarch64
TARGET_AARCH64 = -target aarch64-linux-gnu
CFLAGS_AARCH64 = $(CFLAGS) -isystem $(POPCORN)/aarch64/include
BUILD_AARCH64  = build_aarch64
OBJ_AARCH64    = $(addprefix $(BUILD_AARCH64)/,$(OBJ))

# x86-64
TARGET_X86_64 = -target x86_64-linux-gnu
CFLAGS_X86_64 = $(CFLAGS) -isystem $(POPCORN)/x86_64/include -m64
BUILD_X86_64  = build_x86_64
OBJ_X86_64    = $(addprefix $(BUILD_X86_64)/,$(OBJ))

all: libbomp_aarch64.a libbomp_x86-64.a

%/.dir:
	@mkdir -p $*
	@touch $@

$(BUILD_AARCH64)/%.o: %.c
	@echo " [CC (aarch64)] $<"
	@$(CC) $(TARGET_AARCH64) $(CFLAGS_AARCH64) -c -emit-llvm -o $(@:.o=.bc) $<
	@$(OPT) $(OPT_FLAGS) $(@:.o=.bc) -o $(@:.o=.bc)
	@$(CC) $(TARGET_AARCH64) $(CFLAGS) -c -o $@ $(@:.o=.bc)

libbomp_aarch64.a: $(BUILD_AARCH64)/.dir $(OBJ_AARCH64)
	@echo " [AR] $@"
	@$(AR) -q $@ $(OBJ_AARCH64)

$(BUILD_X86_64)/%.o: %.c
	@echo " [CC (x86-64)] $<"
	@$(CC) $(TARGET_X86_64) $(CFLAGS_X86_64) -c -emit-llvm -o $(@:.o=.bc) $<
	@$(OPT) $(OPT_FLAGS) $(@:.o=.bc) -o $(@:.o=.bc)
	@$(CC) $(TARGET_X86_64) $(CFLAGS) -c -o $@ $(@:.o=.bc)

libbomp_x86-64.a: $(BUILD_X86_64)/.dir $(OBJ_X86_64)
	@echo " [AR] $@"
	@$(AR) -q $@ $(OBJ_X86_64)

install: libbomp_aarch64.a libbomp_x86-64.a
	@echo " [CP] libbomp_aarch64.a $(POPCORN)/aarch64/lib/libbomp.a"
	@cp libbomp_aarch64.a $(POPCORN)/aarch64/lib/libbomp.a
	@echo " [CP] omp.h $(POPCORN)/aarch64/include"
	@cp omp.h $(POPCORN)/aarch64/include
	@echo " [CP] libbomp_x86-64.a $(POPCORN)/x86_64/lib/libbomp.a"
	@cp libbomp_x86-64.a $(POPCORN)/x86_64/lib/libbomp.a
	@echo " [CP] omp.h $(POPCORN)/x86_64/include"
	@cp omp.h $(POPCORN)/x86_64/include
	@echo " [CP] libbomp_x86-64.a $(POPCORN)/lib/libbomp.a"
	@cp libbomp_x86-64.a $(POPCORN)/lib/libbomp.a
	@echo " [CP] omp.h $(POPCORN)/include"
	@cp omp.h $(POPCORN)/include

clean:
	@echo " [RM] libbomp_aarch64.a libbomp_x86-64.a $(BUILD_AARCH64) $(BUILD_X86_64)"
	@rm -rf libbomp_aarch64.a libbomp_x86-64.a $(BUILD_AARCH64) $(BUILD_X86_64)

.PHONY: all tests clean
