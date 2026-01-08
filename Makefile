pokegb: pokegb.c
	gcc -O2 -Wall -Wno-return-type -Wno-misleading-indentation -Wno-parentheses -I/opt/homebrew/include/ -o $@ $< -lSDL2 -L/opt/homebrew/lib

rom.sav: empty.sav
	cp $< $@

clean:
	rm -f pokegb_c rom.sav
