# Makefile

all: bootstrap run

bootstrap:
	@[ -f binplay ] || ./bootstrap.sh

clean:
	rm -d binplay

run:
	./binplay
