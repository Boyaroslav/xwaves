install:
	gcc xwaves.c -o xwaves -lxcb -lxcb-image -lm
	sudo mv xwaves /usr/bin/

