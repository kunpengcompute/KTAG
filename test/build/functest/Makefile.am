# Header file path of gtest and gmock
GTEST_DIR = ../../googletest-release-1.8.0/googletest
GMOCK_DIR = ../../googletest-release-1.8.0/googlemock

# Source file of function
KDADK_APPID_modules_DIR = ../../..
TEST_modules_DIR = ../../
KDADK_APPID_OUTPUT_DIR := $(abspath $(KDADK_APPID_modules_DIR)/output)
ONNXRUNTIME_LIB_DIR := $(abspath $(KDADK_APPID_modules_DIR)/util/onnxruntime-linux-aarch64-1.22.0/lib)

# Header files path
KDADK_APPID_DIR += -I $(GTEST_DIR)/include/
KDADK_APPID_DIR += -I $(GMOCK_DIR)/include/

KDADK_APPID_DIR += -I $(TEST_modules_DIR)/common/include/
KDADK_APPID_DIR += -I $(TEST_modules_DIR)/common/functest/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/include/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/src/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/tools/demo/plugin/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/plugins/proxy_features/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/util/onnxruntime-linux-aarch64-1.22.0/include/
CFLAGS += -DLIB_KDADK_APPID_NAME=\"libkdadk_appidlib.so\"
CXXFLAGS += -DLIB_KDADK_APPID_NAME=\"libkdadk_appidlib.so\"

# Options for preprocessing stage, here are mainly header files.
CPPFLAGS += $(KDADK_APPID_DIR)

# Flags passed to the C++ compiler.
CXXFLAGS += -g -Wall -Wextra -pthread -std=c++17 -DWITH_OMP -fopenmp
CFLAGS += -DWITH_OMP -fopenmp

# asan Flags passed to the C++ compiler.
ifeq ($(cover), asan)
CXXFLAGS += -DWITH_ASAN
CFLAGS += -DWITH_ASAN
endif

# Test Source Code files
SRC_FILES_DIRS += $(TEST_modules_DIR)/common/functest/

# Static and shared libraries that need to be linked to the program.
LD_LIBRARIES := -L$(TEST_modules_DIR)/test_tool_bins/gtest \
				-L$(KDADK_APPID_OUTPUT_DIR) \
				-lkdadk_appidlib \
				-lktag_capture \
				-lktag_feature \
				-lKDADK_feature_extract \
				-lktag_inference \
				-lKDADK_inference \
				-L$(KDADK_APPID_OUTPUT_DIR)/plugins \
				-lktag_proxy_features \
				-L$(ONNXRUNTIME_LIB_DIR) \
				-lonnxruntime

LD_LIBRARIES += -pthread -lgtest -lgtest_main -lgmock -lgmock_main -lpcap
LD_LIBRARIES += -lyaml-cpp -ldl

ifeq ($(cover), y)
LD_LIBRARIES += -lgcov
endif

# The name of the compiled executable file.
PROGRAM = kdadk_appid_functest

# The directories in which source files reside. At least one path should be specified.
ifndef SILENCE
	SILENCE = @
endif

# C/C++ program
SRCEXTS   := .c .cc .cpp

SOURCES_MUD = $(foreach d,$(SRC_FILES_DIRS),$(wildcard $(addprefix $(d)/*,$(SRCEXTS))))
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/pipeline_config.cpp
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/pipeline_runner.cpp
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/schema_feature_writer.cpp
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/ub_feature_wire.cpp
# pipeline_runner.cpp drives capture through the demo_capture_source factory,
# so the factory has to be linked in even when the DPDK backend is disabled.
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/demo_capture_source.cpp
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/plugin/feature_plugin_loader.cpp
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/utils.cpp

# The Provider Pipeline lifecycle test intentionally uses private headers and
# symbols. Keep normal public-ABI functional builds independent of internals;
# run it explicitly with `internal=y` against a visibility-disabled test build.
ifneq ($(internal),y)
SOURCES_MUD := $(filter-out $(TEST_modules_DIR)/common/functest/func_feature_provider.cpp,$(SOURCES_MUD))
endif

# DPDK poll-mode capture backend. libktag_capture_dpdk only exists in a
# KTAG_ENABLE_DPDK=ON build, so these cases are opt-in: run `make dpdk=y`.
# Without the switch the file is dropped from the build, the same way the
# internal-symbol test above is.
ifeq ($(dpdk), y)
DPDK_PC_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_PC_LIBS := $(shell pkg-config --libs libdpdk)
CPPFLAGS += $(DPDK_PC_CFLAGS) -DKTAG_DEMO_HAVE_DPDK=1
LD_LIBRARIES += -lktag_capture_dpdk $(DPDK_PC_LIBS)
SOURCES_MUD += $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/demo_capture_dpdk_source.cpp
else
# Matched on the suffix: wildcard() yields "../..//common/functest//<file>",
# so a literal prefix pattern never matches.
SOURCES_MUD := $(filter-out %/func_capture_dpdk.cpp,$(SOURCES_MUD))
endif

SOURCES = $(shell echo $(SOURCES_MUD) | \
			awk -F ' ' '{for(i=1;i<=NF;i++)a[i]=$$i}END{for(i=1;i<=asort(a);i++)printf a[i]" "}')

OBJS    = $(foreach x,$(SRCEXTS), \
      $(patsubst %$(x), %.o, $(filter %$(x),$(SOURCES))))

# The header dependence list
SRC_DEPS = $(foreach x,$(SRCEXTS), \
      $(patsubst %$(x), %.o.d, $(filter %$(x),$(SOURCES))))

# Header dependence flag
DEPS_FLAG = -MD -MF $@.d

CC_COMPILE = $(SILENCE) $(CC) $(DEPS_FLAG) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<
CXX_COMPILE = $(SILENCE) $(CXX) $(DEPS_FLAG) -c $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

ifeq ($(func), )
	FILTER = *
else
	FILTER = $(func).*
endif

all : $(PROGRAM)
run:
	@test -f "$(KDADK_APPID_OUTPUT_DIR)/libkdadk_appidlib.so" || \
		(echo "missing $(KDADK_APPID_OUTPUT_DIR)/libkdadk_appidlib.so; run 'sh build.sh -lib' from the repository root"; exit 1)
	@test -f "$(KDADK_APPID_OUTPUT_DIR)/libktag_feature.so" || \
		(echo "missing $(KDADK_APPID_OUTPUT_DIR)/libktag_feature.so; rebuild phase-8 libraries"; exit 1)
	env LD_LIBRARY_PATH=$(KDADK_APPID_OUTPUT_DIR):$(KDADK_APPID_OUTPUT_DIR)/plugins:$(ONNXRUNTIME_LIB_DIR):/usr/local/lib64:$$LD_LIBRARY_PATH ../../gtest-parallel/gtest-parallel ./kdadk_appid_functest --gtest_filter=$(FILTER)
rrun:
	env LD_LIBRARY_PATH=$(KDADK_APPID_OUTPUT_DIR)/plugins:/usr/local/ksl/lib/:$$LD_LIBRARY_PATH ../../gtest-parallel/gtest-parallel ./kdadk_appid_functest --gtest_filter=$(FILTER)

clean :
	@-$(RM) *.d *.a *.so
	@-$(RM)	./$(PROGRAM) $(SRC_DEPS)
	@find $(TEST_modules_DIR)/common/functest/ -name '*.o' -exec $(RM) {} \;
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/pipeline_config.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/pipeline_runner.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/schema_feature_writer.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/ub_feature_wire.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/demo_capture_source.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/pipeline/demo_capture_dpdk_source.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/plugin/feature_plugin_loader.o
	@-$(RM) $(KDADK_APPID_modules_DIR)/tools/demo/utils.o

# Builds a sample test.

# Rules for producing the objects.
#---------------------------------------------------
objs : $(OBJS)

%.o : %.c
	@echo compiling $(notdir $<)
	$(CC_COMPILE)

%.o : %.C
	$(CXX_COMPILE)

%.o : %.cc
	@echo compiling $(notdir $<)
	$(CXX_COMPILE)

%.o : %.cpp
	@echo compiling $(notdir $<)
	$(CXX_COMPILE)

%.o : %.CPP
	$(CXX_COMPILE)

%.o : %.c++
	$(CXX_COMPILE)

%.o : %.cp
	$(CXX_COMPILE)

%.o : %.cxx
	$(CXX_COMPILE)

# Rules for producing the executable.
#----------------------------------------------
$(PROGRAM) : $(OBJS)
ifeq ($(strip $(SRCEXTS)), .c)  # C file
	$(CC)  -o $(PROGRAM)  $(OBJS) $(LD_LIBRARIES) -fopenmp
else                            # C++ file
	@echo Linking $@
	$(SILENCE) $(CXX)  -o $(PROGRAM)  $(OBJS) $(LD_LIBRARIES) -fopenmp
endif

# Include the header dependence list file
-include $(SRC_DEPS)
