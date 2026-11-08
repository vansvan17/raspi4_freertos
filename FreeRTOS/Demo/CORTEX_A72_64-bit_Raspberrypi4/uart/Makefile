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
CFLAGS += -DRAM_LOG=$(RAM_LOG) -DRAM_LOG_REPORTS=$(RAM_LOG_REPORTS)
BENCHMARK_MODE ?= 2
MULTICORE_SMOKE_TEST ?= 0
# RAM_LOG=1 also copies UART output to reserved RAM and resets back to Linux
# after RAM_LOG_REPORTS one-second reports (see tools/pi_ramlog.py).
RAM_LOG ?= 0
RAM_LOG_REPORTS ?= 80
MAIN_SOURCE ?= main
EXTRA_OBJS ?=
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)
GIT_REV := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo 0)
GIT_DIRTY := $(shell git status --porcelain --untracked-files=no 2>/dev/null | grep -q . && echo 1 || echo 0)

.PHONY: benchmark-baseline benchmark-ceiling benchmark-priority benchmark-threshold benchmark-suspend threshold-semantics ready-queue-test multicore-smoke ram-persist-test interference-test led-probe

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

# Blinks the ACT LED, then resets to Linux. BOOT_BREADCRUMBS arms the watchdog
# at the first instruction and leaves stage words at 0x300FF100 for Linux.
led-probe:
	$(MAKE) clean all MAIN_SOURCE=led_probe BOOT_BREADCRUMBS=1

ram-persist-test:
	$(MAKE) clean all MAIN_SOURCE=ram_persist_test

# Cores 1-3 run memory hogs while core 0 times a victim workload with the
# MMU and caches on. See INTERFERENCE.md.
interference-test:
	$(MAKE) clean all MAIN_SOURCE=interference_test MULTICORE_SMOKE_TEST=1 \
		EXTRA_OBJS="build/mmu.o build/pmu.o" \
		EXTRA_CFLAGS="-DBUILD_REV=0x$(GIT_REV)ULL -DBUILD_DIRTY=$(GIT_DIRTY)"

BOOT_BREADCRUMBS ?= 0
ASMFLAGS = -mcpu=cortex-a72 --defsym MULTICORE_SMOKE_TEST=$(MULTICORE_SMOKE_TEST) \
           --defsym BOOT_BREADCRUMBS=$(BOOT_BREADCRUMBS)

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
OBJS +=build/ramlog.o
OBJS +=$(EXTRA_OBJS)

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
