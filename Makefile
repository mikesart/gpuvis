# $@: name of the target file (one before colon)
# $<: name of first prerequisite file (first one after colon)
# $^: names of all prerequisite files (space separated)
# $*: stem (bit which matches the % wildcard in rule definition)
#
# VAR = val: Normal setting - values within are recursively expand when var used.
# VAR := val: Setting of var with simple expansion of values inside - values are expanded at decl time.
# VAR ?= val: Set var only if it doesn't have a value.
# VAR += val: Append val to existing value (or set if var didn't exist).

# To use static analyzer:
#   http://clang-analyzer.llvm.org/scan-build.html
# Ie:
#   scan-build -k -V --use-analyzer ~/bin/clang make

.PHONY: all
all:

NAME = gpuvis

USE_GTK3 ?= 1
USE_I915_PERF ?= 0
USE_RAPIDJSON ?= 1
CFG ?= release
ifeq ($(CFG), debug)
    ASAN ?= 1
endif

LD = $(CC)
RM = rm -f
MKDIR = mkdir -p
VERBOSE ?= 0

COMPILER = $(shell $(CC) -v 2>&1 | grep -q "clang version" && echo clang || echo gcc)

WARNINGS = -Wall -Wextra -Wpedantic -Wmissing-include-dirs -Wformat=2 -Wshadow \
	-Wno-unused-parameter -Wno-missing-field-initializers -Wno-variadic-macros
CXXWARNINGS =

ifneq ($(COMPILER),clang)
  # https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
  WARNINGS += -Wsuggest-attribute=format -Wimplicit-fallthrough=2
  CXXWARNINGS += -Wno-class-memaccess

  NO_MAYBE_UNINITIALIZED = -Wno-maybe-uninitialized
  NO_OLD_STYLE_DECLARATION = -Wno-old-style-declaration
  NO_STRINGOP_TRUNCATION = -Wno-stringop-truncation
  NO_COMPARE_DISTINCT_POINTER_TYPES =
  NO_FORMAT_NONLITERAL =
  NO_POINTER_SIGN =
  NO_CLOBBERED = -Wno-clobbered
  NO_SUGGEST_ATTRIBUTE_FORMAT = -Wno-suggest-attribute=format
else
  NO_MAYBE_UNINITIALIZED =
  NO_OLD_STYLE_DECLARATION =
  NO_STRINGOP_TRUNCATION =
  NO_COMPARE_DISTINCT_POINTER_TYPES = -Wno-compare-distinct-pointer-types
  NO_FORMAT_NONLITERAL = -Wno-format-nonliteral
  NO_POINTER_SIGN = -Wno-pointer-sign
  NO_CLOBBERED =
  NO_SUGGEST_ATTRIBUTE_FORMAT =
endif

# Investigate: Improving C++ Builds with Split DWARF
#  http://www.productive-cpp.com/improving-cpp-builds-with-split-dwarf/

CFLAGS = $(WARNINGS) -fno-exceptions -gdwarf-4 -g2 -ggnu-pubnames -gsplit-dwarf
CFLAGS += -DUSE_FREETYPE -D_LARGEFILE64_SOURCE=1 -D_FILE_OFFSET_BITS=64
CXXFLAGS = -fno-rtti -Woverloaded-virtual $(CXXWARNINGS)
LDFLAGS = -gdwarf-4 -g2 -Wl,--build-id=sha1
LIBS = -Wl,--no-as-needed -lm -ldl -lpthread -lstdc++

CFLAGS += $(shell sdl2-config --cflags)
LIBS += $(shell sdl2-config --libs)

ifeq ($(USE_GTK3), 1)
CFLAGS += $(shell pkg-config --cflags gtk+-3.0) -DUSE_GTK3
endif

ifeq ($(USE_I915_PERF), 1)
CFLAGS += $(shell pkg-config --cflags i915-perf) -DUSE_I915_PERF
LIBS += $(shell pkg-config --libs i915-perf)
endif

ifeq ($(USE_RAPIDJSON), 1)
CFLAGS += $(shell pkg-config --cflags RapidJSON) -DHAVE_RAPIDJSON
endif

CFLAGS += $(shell pkg-config --cflags freetype2)
LIBS += $(shell pkg-config --libs freetype2)

CFLAGS +=  -Isrc/libtraceevent/include

ifneq ("$(wildcard /usr/bin/ld.gold)","")
  $(info Using gold linker...)
  LDFLAGS += -fuse-ld=gold -Wl,--gdb-index
endif

# https://gcc.gnu.org/onlinedocs/libstdc++/manual/profile_mode.html#manual.ext.profile_mode.intro
# To resolve addresses from libstdcxx-profile.conf.out: addr2line -C -f -e _debug/gpuvis 0x42cc6a 0x43630a 0x46654d
# CFLAGS += -D_GLIBCXX_PROFILE -D_GLIBCXX_PROFILE_CONTAINERS

CFILES = \
	src/gpuvis.cpp \
	src/gpuvis_etl.cpp \
	src/gpuvis_graph.cpp \
	src/gpuvis_framemarkers.cpp \
	src/gpuvis_plots.cpp \
	src/gpuvis_graphrows.cpp \
	src/gpuvis_ftrace_print.cpp \
	src/gpuvis_utils.cpp \
	src/tdopexpr.cpp \
	src/ya_getopt.c \
	src/MurmurHash3.cpp \
	src/miniz.c \
	src/stlini.cpp \
	src/imgui/imgui_impl_sdl_gl3.cpp \
	src/imgui/imgui.cpp \
	src/imgui/imgui_demo.cpp \
	src/imgui/imgui_draw.cpp \
	src/GL/gl3w.c \
	src/i915-perf/i915-perf-read.cpp \
	src/libtraceevent/src/event-parse.c \
	src/libtraceevent/src/event-parse-api.c \
	src/libtraceevent/src/event-plugin.c \
	src/libtraceevent/src/kbuffer-parse.c \
	src/libtraceevent/src/parse-utils.c \
	src/libtraceevent/src/trace-seq.c \
	src/trace-cmd/trace-read.cpp \
	src/imgui/imgui_freetype.cpp \
	src/gpuvis_i915_perfcounters.cpp

ifeq ($(PROF), 1)
	# To profile with google perftools:
	#   http://htmlpreview.github.io/?https://github.com/gperftools/gperftools/blob/master/doc/cpuprofile.html
	# PROF=1 make -j 30 && CPUPROFILE_FREQUENCY=10000 _release/gpuvis && pprof --stack --text _release/gpuvis ./gpuvis.prof | vi -
	# PROF=1 make -j 30 && CPUPROFILE_FREQUENCY=10000 _release/gpuvis && pprof --web _release/gpuvis ./gpuvis.prof
	ASAN = 0
	CFLAGS += -DGPROFILER
	LDFLAGS += -Wl,--no-as-needed -lprofiler
endif

# Useful GCC address sanitizer checks not enabled by default
# https://kristerw.blogspot.com/2018/06/useful-gcc-address-sanitizer-checks-not.html

ifeq ($(ASAN), 1)
	# https://gcc.gnu.org/gcc-5/changes.html
	#  -fsanitize=float-cast-overflow: check that the result of floating-point type to integer conversions do not overflow;
	#  -fsanitize=alignment: enable alignment checking, detect various misaligned objects;
	#  -fsanitize=vptr: enable checking of C++ member function calls, member accesses and some conversions between pointers to base and derived classes, detect if the referenced object does not have the correct dynamic type.
	ASAN_FLAGS = -fno-omit-frame-pointer -fno-optimize-sibling-calls
	ASAN_FLAGS += -fsanitize=address # fast memory error detector (heap, stack, global buffer overflow, and use-after free)
	ASAN_FLAGS += -fsanitize=leak # detect leaks
	ASAN_FLAGS += -fsanitize=float-divide-by-zero # detect floating-point division by zero;
	ASAN_FLAGS += -fsanitize=bounds # enable instrumentation of array bounds and detect out-of-bounds accesses;
ifneq ($(COMPILER),clang)
	ASAN_FLAGS += -fsanitize=undefined # fast undefined behavior detector
	ASAN_FLAGS += -fsanitize=object-size # enable object size checking, detect various out-of-bounds accesses.
endif
	CFLAGS += $(ASAN_FLAGS)
	LDFLAGS += $(ASAN_FLAGS)
endif

ifeq ($(CFG), debug)
	ODIR=_debug
	CFLAGS += -O0 -DDEBUG
	CFLAGS += -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC -D_GLIBCXX_SANITIZE_VECTOR -D_LIBCPP_DEBUG=1
else
	ODIR=_release
	CFLAGS += -O2 -DNDEBUG
endif

ifeq ($(VERBOSE), 1)
	VERBOSE_PREFIX=
else
	VERBOSE_PREFIX=@
endif

PROJ = $(ODIR)/$(NAME)
$(info Building $(ODIR)/$(NAME)...)

C_OBJS = ${CFILES:%.c=${ODIR}/%.o}
OBJS = ${C_OBJS:%.cpp=${ODIR}/%.o}

all: $(PROJ)

$(ODIR)/$(NAME): $(OBJS)
	@echo "Linking $@...";
	$(VERBOSE_PREFIX)$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

-include $(OBJS:.o=.d)

ifneq ($(COMPILER),clang)
$(ODIR)/src/imgui/imgui.o: CFLAGS += -Wno-stringop-truncation
endif

$(ODIR)/src/libtraceevent/src/event-parse.o: CFLAGS += -Wno-pedantic -Wno-format-nonliteral -Wno-sign-compare \
	-Wno-shadow -Wno-implicit-function-declaration \
	$(NO_MAYBE_UNINITIALIZED) $(NO_OLD_STYLE_DECLARATION) \
	$(NO_FORMAT_NONLITERAL) $(NO_POINTER_SIGN) $(NO_COMPARE_DISTINCT_POINTER_TYPES)
$(ODIR)/src/libtraceevent/src/trace-seq.o: CFLAGS += -Wno-pedantic
$(ODIR)/src/libtraceevent/src/event-plugin.o: CFLAGS += -Wno-pedantic -Wno-shadow -Wno-implicit-function-declaration
$(ODIR)/src/libtraceevent/src/trace-seq.o: CFLAGS += -Wno-pedantic -Wno-sign-compare
$(ODIR)/src/libtraceevent/src/kbuffer-parse.o: CFLAGS += -Wno-pedantic -Wno-sign-compare $(NO_COMPARE_DISTINCT_POINTER_TYPES)
$(ODIR)/src/libtraceevent/src/parse-utils.o: CFLAGS += -Wno-pedantic -Wno-implicit-function-declaration \
	$(NO_FORMAT_NONLITERAL) $(NO_SUGGEST_ATTRIBUTE_FORMAT)
$(ODIR)/src/libtraceevent/src/event-parse-api.o: CFLAGS += -Wno-pedantic
$(ODIR)/src/imgui/imgui.o: CXXFLAGS += $(NO_STRINGOP_TRUNCATION)
$(ODIR)/src/trace-cmd/trace-read.o: CXXFLAGS += $(NO_CLOBBERED)

$(ODIR)/%.o: %.c Makefile
	$(VERBOSE_PREFIX)echo "---- $< ----";
	@$(MKDIR) $(dir $@)
	$(VERBOSE_PREFIX)$(CC) -MMD -MP -std=gnu99 $(CFLAGS) -o $@ -c $<

$(ODIR)/%.o: %.cpp Makefile
	$(VERBOSE_PREFIX)echo "---- $< ----";
	@$(MKDIR) $(dir $@)
	$(VERBOSE_PREFIX)$(CXX) -MMD -MP -std=c++11 $(CFLAGS) $(CXXFLAGS) -o $@ -c $<

.PHONY: clean

clean:
	@echo Cleaning...
	$(VERBOSE_PREFIX)$(RM) $(PROJ)
	$(VERBOSE_PREFIX)$(RM) $(OBJS)
	$(VERBOSE_PREFIX)$(RM) $(OBJS:.o=.d)
	$(VERBOSE_PREFIX)$(RM) $(OBJS:.o=.dwo)
