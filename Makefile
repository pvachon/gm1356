OBJ=splread.o

TARGET=splread

OFLAGS=-O0 -ggdb
DEFINES=-DTSL_DEBUG

TSL_CFLAGS=`pkg-config --cflags tsl`
TSL_LIBS=`pkg-config --libs tsl`
HIDAPI_CFLAGS=`pkg-config --cflags hidapi-libusb`
HIDAPI_LIBS=`pkg-config --libs hidapi-libusb`

inc=$(OBJ:%.o=%.d)

CFLAGS=$(OFLAGS) -Wall -Wextra -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wno-trigraphs \
	   -std=c11 -fno-strict-aliasing -fno-common -Werror-implicit-function-declaration -Wuninitialized \
	   -Wmissing-include-dirs -Wshadow -Wframe-larger-than=2047 -D_GNU_SOURCE \
	   -I. $(TSL_CFLAGS) $(HIDAPI_CFLAGS) $(DEFINES)
LDFLAGS=$(TSL_LIBS) $(HIDAPI_LIBS)

$(TARGET): $(OBJ)
	$(CC) -o $(TARGET) $(OBJ) $(LDFLAGS)

-include $(inc)

.c.o:
	$(CC) $(CFLAGS) -MMD -MP -c $<

clean:
	$(RM) $(OBJ) $(TARGET)
	$(RM) $(inc)

.PHONY: clean
