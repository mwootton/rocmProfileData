
PREFIX = /usr/local

HIP_PATH ?= $(or $(wildcard /opt/rocm/),$(wildcard /opt/venv/lib/python3.12/site-packages/_rocm_sdk_devel))
CUDA_PATH ?= $(wildcard /usr/local/cuda/)
CPPTRACE_INCLUDE_PATH ?= $(wildcard ../cpptrace_install/include)

ROCM_VERSION ?= $(shell cat $(HIP_PATH)/.info/version)

RPD_LIBS = -lsqlite3 -lfmt -ldl
RPD_INCLUDES =
RPD_SRCS = Table.cpp BufferedTable.cpp OpTable.cpp KernelApiTable.cpp CopyApiTable.cpp ApiTable.cpp StringTable.cpp UStringTable.cpp MetadataTable.cpp MonitorTable.cpp StackFrameTable.cpp ApiIdList.cpp DbResource.cpp Logger.cpp Unwind.cpp

ifneq (,$(HIP_PATH))
        $(info rocm_version is ${ROCM_VERSION})
        ifneq (,$(wildcard $(HIP_PATH)/include/rocprofiler-sdk))
        ifneq ($(shell echo ${ROCM_VERSION} | cut -d. -f1), 6)
                $(info Building with rocprofiler-sdk)
		RPD_LIBS += -lrocprofiler-sdk -lrocprofiler-sdk-roctx
		RPD_SRCS += RocprofDataSource.cpp
        endif
        endif

        ifneq (,$(wildcard $(HIP_PATH)/include/roctracer))
                $(info Building with roctracer)
		RPD_LIBS += -lroctracer64 -lroctx64
		RPD_SRCS += RoctracerDataSource.cpp
        endif

        ifneq (,$(wildcard $(HIP_PATH)/include/hip/amd_detail/hip_profiler_ext.h))
                $(info Building with ClrDataSource)
                RPD_SRCS += ClrDataSource.cpp
        endif

        RPD_LIBS += -L$(HIP_PATH)/lib
        RPD_LIBS += -lamdhip64 #-lrocm_smi64

        RPD_INCLUDES += -D__HIP_PLATFORM_AMD__
        RPD_INCLUDES += -I$(HIP_PATH)/include -I$(HIP_PATH)/include/hsa
        RPD_INCLUDES += -Ijson/include
endif

ifneq ($(CUDA_PATH),)
        $(info Building with cupti)
        RPD_LIBS += -L/usr/local/cuda/lib64 -lcupti
        RPD_INCLUDES += -I/usr/local/cuda/include
        RPD_SRCS += CuptiDataSource.cpp
endif

ifneq ($(CPPTRACE_INCLUDE_PATH),)
        $(info Building with cpptrace)
        RPD_INCLUDES += -DRPD_STACKFRAME_SUPPORT
        RPD_LIBS += -L$(CPPTRACE_INCLUDE_PATH)/../lib -lcpptrace -ldwarf -lz -lzstd
        RPD_INCLUDES += -I $(CPPTRACE_INCLUDE_PATH)
endif

RPD_OBJS = $(RPD_SRCS:.cpp=.o)
RPD_DEPS = $(RPD_OBJS:.o=.d)

RPD_MAIN = librpd_tracer.so
RPD_SCRIPT = runTracer.sh loadTracer.sh

PYTHON = python3
PIP = pip3


all: $(RPD_MAIN)

.PHONY: all


$(RPD_MAIN): $(RPD_OBJS)
	$(CXX) -o $@ $^ -shared -rdynamic -std=c++17 $(RPD_LIBS) -g

%.o: %.cpp
	$(CXX) -o $@ -c $< $(RPD_INCLUDES) -DAMD_INTERNAL_BUILD -std=c++17 -fPIC -g -O3 -MMD -MP

-include $(RPD_DEPS)

.PHONY: install
install: all
	cp $(RPD_MAIN)  $(PREFIX)/lib/
	cp $(RPD_SCRIPT) $(PREFIX)/bin/
	ldconfig
	$(PIP) install .

.PHONY: uninstall
uninstall:
	rm $(PREFIX)/lib/$(RPD_MAIN)
	rm $(PREFIX)/bin/$(RPD_SCRIPT)

.PHONY: clean
clean:
	rm -f *.o *.so *.d
