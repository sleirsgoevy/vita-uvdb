all: libuvdb.a

clean:
	rm -f *.o *.a *.elf *.velf eboot.bin param.sfo *.vpk *.psp2dmp

package: uvdb-test.vpk

deploy: package
	curl -v -T uvdb-test.vpk ftp://$(VITA_IP):1337/ux0:/uvdb-test.vpk 

fetch_dumps:
	curl ftp://$(VITA_IP):1337/ux0:/data/ | grep -o 'psp2core-.*' | while read line; do curl "ftp://$(VITA_IP):1337/ux0:/data/$$line" > "$$line"; curl -v "ftp://$(VITA_IP):1337/" -Q "DELE ux0:/data/$$line" >/dev/null; done

EXTRA_CFLAGS := -O0 -g -I $(VITASDK)/share/gcc-arm-vita-eabi/samples/common
EXTRA_LDFLAGS := $(CFLAGS) -Wl,-q -lSceDisplay_stub -lSceNetPs_stub -lkubridge_stub

%.o: %.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

test.o: test.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

psvDebugScreen.o: $(VITASDK)/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

libuvdb.a: uvdb.o
	ar q $@ $^

test.elf: psvDebugScreen.o test.o libuvdb.a
	arm-vita-eabi-gcc $^ $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@

param.sfo:
	vita-mksfoex -s TITLE_ID=SLRS00001 'UVDB test' $@

test.velf: test.elf
	vita-elf-create $< $@

eboot.bin: test.velf
	vita-make-fself $< $@

uvdb-test.vpk: eboot.bin param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin $@
