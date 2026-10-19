CROSS ?= aarch64-none-elf-

APP_NAME = uart

CFLAGS = -mcpu=cortex-a72 \
         -fpic \
         -ffreestanding \
         -std=gnu99 \
         -O2 \
         -Wall \
         -Wextra \
         -DGUEST \
         -I$(INCLUDEPATH1) \
         -I$(INCLUDEPATH2) \
         -I$(INCLUDEPATH3) \
         -I$(INCLUDEPATH4) 

CFLAGS += -DBENCHMARK_MODE=$(BENCHMARK_MODE)
CFLAGS += -DMULTICORE_SMOKE_TEST=$(MULTICORE_SMOKE_TEST)
BENCHMARK_MODE ?= 2
MULTICORE_SMOKE_TEST ?= 0
MAIN_SOURCE ?= main

.PHONY: benchmark-baseline benchmark-ceiling benchmark-priority benchmark-threshold benchmark-suspend threshold-semantics ready-queue-test multicore-smoke

benchmark-baseline:
	$(MAKE) clean all BENCHMARK_MODE=0

benchmark-ceiling:
	$(MAKE) clean all BENCHMARK_MODE=1

# Compatibility alias retained for earlier experiment notes.
benchmark-priority: benchmark-ceiling

benchmark-threshold:
	$(MAKE) clean all BENCHMARK_MODE=2

benchmark-suspend:
	$(MAKE) clean all BENCHMARK_MODE=3

threshold-semantics:
	$(MAKE) clean all MAIN_SOURCE=threshold_semantics

ready-queue-test:
	$(MAKE) clean all MAIN_SOURCE=ready_queue_test

multicore-smoke:
	$(MAKE) clean all BENCHMARK_MODE=0 MULTICORE_SMOKE_TEST=1

ASMFLAGS = -mcpu=cortex-a72 --defsym MULTICORE_SMOKE_TEST=$(MULTICORE_SMOKE_TEST)

INCLUDEPATH1 ?= ./src
INCLUDEPATH2 ?= ../mmu
INCLUDEPATH3 ?= ../../../Source/include
INCLUDEPATH4 ?= ../../../Source/portable/GCC/ARM_CA72_64_BIT

# From ./src
OBJS = build/startup.o 
OBJS +=build/FreeRTOS_asm_vector.o
OBJS +=build/FreeRTOS_tick_config.o
OBJS +=build/interrupt.o
OBJS +=build/$(MAIN_SOURCE).o
OBJS +=build/runtime.o
OBJS +=build/uart.o

# From ../cache
OBJS +=build/cache.o

# From ../../../Source/portable/GCC/ARM_CA72_64_BIT
OBJS +=build/port.o
OBJS +=build/portASM.o

OBJS +=build/list.o
OBJS +=build/queue.o
OBJS +=build/tasks.o
OBJS +=build/timers.o

OBJS +=build/heap_1.o

BUILDDIR =./build

.PHONY: all clean

all : builddir $(APP_NAME).elf kernel8.img

builddir : | $(BUILDDIR)

$(BUILDDIR) :
	mkdir $@

clean :
	rm -rf ./build
	rm -f *.elf
	rm -f *.map
	rm -f kernel8.img

$(APP_NAME).elf : src/raspberrypi4.ld $(OBJS)
	$(CROSS)gcc -Wl,-Map=$(APP_NAME).map -Wl,--build-id=none -std=gnu99 -T src/raspberrypi4.ld -o $@ -ffreestanding -nostdlib $(OBJS) -lgcc

kernel8.img : $(APP_NAME).elf
	$(CROSS)objcopy -O binary $< $@

build/%.o : src/%.S
	$(CROSS)as $(ASMFLAGS) -c -o $@ $<
	
build/%.o : src/%.c
	$(CROSS)gcc $(CFLAGS)  -c -o $@ $<

build/%.o : ../mmu/%.c
	$(CROSS)gcc $(CFLAGS)  -c -o $@ $<

build/%.o : ../cache/%.S
	$(CROSS)gcc $(CFLAGS)  -c -o $@ $<

build/%.o : ../../../Source/%.c
	$(CROSS)gcc $(CFLAGS)  -c -o $@ $<

build/%.o : ../../../Source/portable/GCC/ARM_CA72_64_BIT/%.c
	$(CROSS)gcc $(CFLAGS)  -c -o $@ $<

build/%.o : ../../../Source/portable/GCC/ARM_CA72_64_BIT/%.S
	$(CROSS)as $(ASMFLAGS) -c -o $@ $<

build/%.o : ../../../Source/portable/MemMang/%.c
	$(CROSS)gcc $(CFLAGS)  -c -o $@ $<
