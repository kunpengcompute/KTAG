# Header file path of gtest and gmock
GTEST_DIR = ../../googletest-release-1.8.0/googletest
GMOCK_DIR = ../../googletest-release-1.8.0/googlemock

# Source file of function
KTAG_APPID_modules_DIR = ../../..
TEST_modules_DIR = ../../
KTAG_APPID_OUTPUT_DIR := $(abspath $(KTAG_APPID_modules_DIR)/output)
ONNXRUNTIME_LIB_DIR := $(abspath $(KTAG_APPID_modules_DIR)/util/onnxruntime-linux-aarch64-1.22.0/lib)

# Header files path
KTAG_APPID_DIR += -I $(GTEST_DIR)/include/
KTAG_APPID_DIR += -I $(GMOCK_DIR)/include/

KTAG_APPID_DIR += -I $(TEST_modules_DIR)/common/include/
KTAG_APPID_DIR += -I $(TEST_modules_DIR)/common/perftest/
KTAG_APPID_DIR += -I $(KTAG_APPID_modules_DIR)/include/
KTAG_APPID_DIR += -I $(KTAG_APPID_modules_DIR)/util/onnxruntime-linux-aarch64-1.22.0/include/
CFLAGS += -DLIB_KTAG_APPID_NAME=\"libktag_appidlib.so\"
CXXFLAGS += -DLIB_KTAG_APPID_NAME=\"libktag_appidlib.so\"


# Options for preprocessing stage, here are mainly header files.
KTAG_APPID_DIR += -I $(KTAG_APPID_modules_DIR)/plugins/traffic_features/
KTAG_APPID_DIR += -I $(KTAG_APPID_modules_DIR)/tools/KFlowLens/
CPPFLAGS += $(KTAG_APPID_DIR)

# Flags passed to the C++ compiler.
CXXFLAGS += -g -Wall -Wextra -pthread -std=c++17 -O2
CFLAGS +=

# Test Source Code files
SRC_FILES_DIRS += $(TEST_modules_DIR)/common/perftest/

# Static and shared libraries that need to be linked to the program.
LD_LIBRARIES := -L$(TEST_modules_DIR)/test_tool_bins/gtest \
                -L$(KTAG_APPID_OUTPUT_DIR) \
                -lktag_appidlib \
				-lktag_feature \
				-L$(ONNXRUNTIME_LIB_DIR) \
				-lonnxruntime

LD_LIBRARIES += -pthread -lgtest -lgtest_main -lgmock -lgmock_main -lpcap


# The name of the compiled executable file.
PROGRAM = ktag_appid_perftest

# The directories in which source files reside. At least one path should be specified.
ifndef SILENCE
	SILENCE = @
endif

# C/C++ program
SRCEXTS   := .c .cc .cpp

SOURCES_MUD = $(foreach d,$(SRC_FILES_DIRS),$(wildcard $(addprefix $(d)/*,$(SRCEXTS))))
SOURCES = $(shell echo $(SOURCES_MUD) | \
			awk -F ' ' '{for(i=1;i<=NF;i++)a[i]=$$i}END{for(i=1;i<=asort(a);i++)printf a[i]" "}')

SOURCES += $(KTAG_APPID_modules_DIR)/plugins/traffic_features/legacy/statistics.cpp

OBJS    = $(foreach x,$(SRCEXTS), \
      $(patsubst %$(x), %.o, $(filter %$(x),$(SOURCES))))

# The header dependence list
SRC_DEPS = $(foreach x,$(SRCEXTS), \
      $(patsubst %$(x), %.o.d, $(filter %$(x),$(SOURCES))))

# Header dependence flag
DEPS_FLAG = -MD -MF $@.d

CC_COMPILE = $(SILENCE) $(CC) $(DEPS_FLAG) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<
CXX_COMPILE = $(SILENCE) $(CXX) $(DEPS_FLAG) -c $(CPPFLAGS) $(CXXFLAGS) -fpermissive -o $@ $<

ifeq ($(func), )
	FILTER = *
else
	FILTER = $(func).*
endif

all : $(PROGRAM)
run:
	@test -f "$(KTAG_APPID_OUTPUT_DIR)/libktag_appidlib.so" || \
		(echo "missing $(KTAG_APPID_OUTPUT_DIR)/libktag_appidlib.so; run 'bash buildtools/build.sh -lib' from the repository root"; exit 1)
	@test -f "$(KTAG_APPID_OUTPUT_DIR)/libktag_feature.so" || \
		(echo "missing $(KTAG_APPID_OUTPUT_DIR)/libktag_feature.so; rebuild phase-8 libraries"; exit 1)
	env LD_LIBRARY_PATH=$(KTAG_APPID_OUTPUT_DIR):$(ONNXRUNTIME_LIB_DIR):/usr/local/lib64:$$LD_LIBRARY_PATH ../../gtest-parallel/gtest-parallel ./ktag_appid_perftest --gtest_filter=$(FILTER)
rrun:
	env LD_LIBRARY_PATH=/usr/local/ksl/lib/:$$LD_LIBRARY_PATH ../../gtest-parallel/gtest-parallel ./ktag_appid_perftest --gtest_filter=$(FILTER)

clean :
	@-$(RM) $(KTAG_APPID_modules_DIR)/plugins/traffic_features/legacy/*.o $(KTAG_APPID_modules_DIR)/plugins/traffic_features/legacy/*.o.d
	@-$(RM) *.d *.a *.so
	@-$(RM)	./$(PROGRAM) $(SRC_DEPS)
	@find $(TEST_modules_DIR)/common/perftest/ -name '*.o' -exec $(RM) {} \;

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
	$(CC)  -o $(PROGRAM)  $(OBJS) $(LD_LIBRARIES)
else                            # C++ file
	@echo Linking $@
	$(SILENCE) $(CXX)  -o $(PROGRAM)  $(OBJS) $(LD_LIBRARIES)
endif

# Include the header dependence list file
-include $(SRC_DEPS)
