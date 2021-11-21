# Makefile

PROG=binplay
INSTALL_PATH=/usr/local/bin

all: bootstrap run

bootstrap:
	@[ -f ${PROG} ] || ./bootstrap.sh

clean:
	rm -d ${PROG}

run:
	./${PROG}

install:
	chmod o+x ${PROG}
	cp ${PROG} ${INSTALL_PATH}

uninstall:
	rm ${INSTALL_PATH}/${PROG}
