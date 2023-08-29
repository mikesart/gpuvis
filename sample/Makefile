NAME = glxgears

CFG ?= release
ifeq ($(CFG), debug)
    ASAN ?= 1
endif

LD = $(CC)
RM = rm -f
MKDIR = mkdir -p
VERBOSE ?= 0

COMPILER = $(shell $(CC) -v 2>&1 | grep -q "clang version" && echo clang || echo gcc)

WARNINGS = -Wall -Wextra -Wpedantic -Wmissing-include-dirs -Wformat=2 -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers
ifneq ($(COMPILER),clang)
  WARNINGS += -Wsuggest-attribute=format
endif

CFLAGS = $(WARNINGS) -march=native -fno-exceptions -gdwarf-4 -g2
CXXFLAGS = -fno-rtti -Woverloaded-virtual
LDFLAGS = -march=native -gdwarf-4 -g2 -Wl,--build-id=sha1
LIBS = -Wl,--no-as-needed -lGL -lX11 -lm -ldl -lpthread -lstdc++

ifeq (, $(filter-out Linux SunOS,$(shell uname -s)))
CFLAGS += -D_LARGEFILE64_SOURCE=1 -D_FILE_OFFSET_BITS=64
endif

CFILES = \
	glxgears.c

ifeq ($(ASAN), 1)
	# https://gcc.gnu.org/gcc-5/changes.html
	#  -fsanitize=float-cast-overflow: check that the result of floating-point type to integer conversions do not overflow;
	#  -fsanitize=alignment: enable alignment checking, detect various misaligned objects;
	#  -fsanitize=vptr: enable checking of C++ member function calls, member accesses and some conversions between pointers to base and derived classes, detect if the referenced object does not have the correct dynamic type.
	ASAN_FLAGS = -fno-omit-frame-pointer -fno-optimize-sibling-calls
	ASAN_FLAGS += -fsanitize=address # fast memory error detector (heap, stack, global buffer overflow, and use-after free)
	ASAN_FLAGS += -fsanitize=leak # detect leaks
	ASAN_FLAGS += -fsanitize=undefined # fast undefined behavior detector
	ASAN_FLAGS += -fsanitize=float-divide-by-zero # detect floating-point division by zero;
	ASAN_FLAGS += -fsanitize=bounds # enable instrumentation of array bounds and detect out-of-bounds accesses;
	ASAN_FLAGS += -fsanitize=object-size # enable object size checking, detect various out-of-bounds accesses.
	CFLAGS += $(ASAN_FLAGS)
	LDFLAGS += $(ASAN_FLAGS)
endif

ifeq ($(CFG), debug)
	ODIR=_debug
	CFLAGS += -O0 -DDEBUG
	CFLAGS += -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC
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
