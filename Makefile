pokegb: pokegb.c
	gcc -O3 -march=native -flto -Wall -Wno-return-type -Wno-misleading-indentation -Wno-parentheses -I/opt/homebrew/include/ -o $@ $< -lSDL2 -L/opt/homebrew/lib

rom.sav: empty.sav
	cp $< $@

clean:
	rm -f pokegb_c rom.sav
