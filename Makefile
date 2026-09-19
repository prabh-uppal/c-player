# ============================================================================
#  asciiplayer — terminal RGB ASCII video player
#
#  make            build ./asciiplayer
#  make run        build, then open the file browser
#  make clean      remove the binary and all object files
#  make legacy     build the original single-file version as ./asciiplayer-legacy
#
#  Everything is compiled from src/, with headers found under include/.
# ============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
INCLUDES  = -Iinclude

TARGET  = asciiplayer
BUILD   = build

# Every .cpp under src/ is part of the program — no list to keep in sync.
SRCS := $(shell find src -name '*.cpp')
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all run clean legacy

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

# -MMD -MP writes a .d file listing the headers each .cpp included, so editing
# a header rebuilds exactly the files that use it.
$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

legacy:
	$(CXX) -std=c++17 -O2 -o asciiplayer-legacy legacy/asciiplayer.cpp

clean:
	rm -rf $(BUILD) $(TARGET) asciiplayer-legacy
