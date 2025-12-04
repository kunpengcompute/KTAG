# Header file path of gtest and gmock
GTEST_DIR = ../../googletest-release-1.8.0/googletest
GMOCK_DIR = ../../googletest-release-1.8.0/googlemock

# Source file of function
KDADK_APPID_modules_DIR = ../../..
TEST_modules_DIR = ../../

# Header files path
KDADK_APPID_DIR += -I $(GTEST_DIR)/include/
KDADK_APPID_DIR += -I $(GMOCK_DIR)/include/

KDADK_APPID_DIR += -I $(TEST_modules_DIR)/common/include/
KDADK_APPID_DIR += -I $(TEST_modules_DIR)/common/perftest/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/include/
KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/util/libboundscheck/include

KDADK_APPID_DIR += -I $(KDADK_APPID_modules_DIR)/include/
CFLAGS += -DLIB_KDADK_APPID_NAME=\"libkdadk_appidlib.so\"
CXXFLAGS += -DLIB_KDADK_APPID_NAME=\"libkdadk_appidlib.so\"


# Options for preprocessing stage, here are mainly header files.
CPPFLAGS += $(KDADK_APPID_DIR)

# Flags passed to the C++ compiler.
CXXFLAGS += -g -Wall -Wextra -pthread -std=c++17 -O2
CFLAGS +=

# Test Source Code files
SRC_FILES_DIRS += $(TEST_modules_DIR)/common/perftest/

# Static and shared libraries that need to be linked to the program.
LD_LIBRARIES := -L$(TEST_modules_DIR)/test_tool_bins/gtest \
                -L$(KDADK_APPID_modules_DIR)/output/ \
                -L$(KDADK_APPID_modules_DIR)/util/libboundscheck/lib \
                -lkdadk_appidlib \
                -lonnxInference \
                -lboundscheck \
                -Wl,-rpath,$(KDADK_APPID_modules_DIR)/output/ \
                -Wl,-rpath,$(KDADK_APPID_modules_DIR)/util/libboundscheck/lib

LD_LIBRARIES += -pthread -lgtest -lgtest_main -lgmock -lgmock_main

# The name of the compiled executable file.
PROGRAM = kdadk_appid_perftest

# The directories in which source files reside. At least one path should be specified.
ifndef SILENCE
	SILENCE = @
endif

# C/C++ program
SRCEXTS   := .c .cc .cpp

SOURCES_MUD = $(foreach d,$(SRC_FILES_DIRS),$(wildcard $(addprefix $(d)/*,$(SRCEXTS))))
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
CXX_COMPILE = $(SILENCE) $(CXX) $(DEPS_FLAG) -c $(CPPFLAGS) $(CXXFLAGS) -fpermissive -o $@ $<

ifeq ($(func), )
	FILTER = *
else
	FILTER = $(func).*
endif

all : $(PROGRAM)
run:
	env LD_LIBRARY_PATH=$(KDADK_APPID_modules_DIR)/output/:$(KDADK_APPID_modules_DIR)/util/libboundscheck/lib:$$LD_LIBRARY_PATH \
						../../gtest-parallel/gtest-parallel ./kdadk_appid_perftest --gtest_filter=$(FILTER)
rrun:
	env LD_LIBRARY_PATH=/usr/local/ksl/lib/:$(KDADK_APPID_modules_DIR)/util/libboundscheck/lib:$$LD_LIBRARY_PATH \
						../../gtest-parallel/gtest-parallel ./kdadk_appid_perftest --gtest_filter=$(FILTER)

clean :
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