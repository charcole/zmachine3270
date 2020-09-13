BUILDDIR=build

TARGET=3270.out
SOURCES=zops.c
CPPSOURCES=crc.cpp 3270.cpp Screen.cpp PacketParser.cpp TN3270.cpp Network.cpp FrontEnd.cpp main.cpp
SRCOBJECTS=$(SOURCES:.c=.o)
CPPOBJECTS=$(CPPSOURCES:.cpp=.o)
OBJECTS=$(foreach o, $(SRCOBJECTS), $(BUILDDIR)/$(o))
OBJECTS+=$(foreach o, $(CPPOBJECTS), $(BUILDDIR)/$(o))

DEPENDS=$(OBJECTS:.o=.d)

CC=clang
COMMONCFLAGS=-c -O0 -g -Wall -MMD -fsigned-char -Werror -ffast-math -Wno-unused
CFLAGS=$(COMMONCFLAGS)
CPPFLAGS=$(COMMONCFLAGS) -std=c++11
LDFLAGS=-lm -lstdc++
LIBS=

.PHONY: all clean makedirs

all: makedirs $(TARGET)

makedirs:
	@mkdir -p $(BUILDDIR)
	
$(BUILDDIR)/%.o: %.c
	@echo Compiling $<
	@$(CC) $(CFLAGS) $< -o $@

$(BUILDDIR)/%.o: %.cpp
	@echo Compiling $<
	@$(CC) $(CPPFLAGS) $< -o $@

$(TARGET): $(OBJECTS) $(LIBS)
	@echo Linking $@
	@$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

clean:
	@echo Cleaning
	@rm -f $(BUILDDIR)/*.o
	@rm -f $(BUILDDIR)/*.d
	@rm -f $(TARGET)

-include $(DEPENDS)
