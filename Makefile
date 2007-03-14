#
# Asterisk -- A telephony toolkit for Linux.
# 
# Makefile for CDR backends (dynamically loaded)
#
# Copyright (C) 1999, Mark Spencer
#
# Mark Spencer <markster@linux-support.net>
#
# This program is free software, distributed under the terms of
# the GNU General Public License
#

.EXPORT_ALL_VARIABLES:

MODS=format_mp3/format_mp3.so app_saycountpl.so
# MODS=

OSARCH=$(shell uname -s)
OSREV=$(shell uname -r)

ifneq ($(OSARCH),SunOS)
ASTLIBDIR=$(INSTALL_PREFIX)/usr/lib/asterisk
else
ASTLIBDIR=$(INSTALL_PREFIX)/opt/asterisk/lib
CC=gcc
endif

ifeq ($(OSARCH),SunOS)
  GREP=/usr/xpg4/bin/grep
  M4=/usr/local/bin/m4
  ID=/usr/xpg4/bin/id
endif

ifeq ($(OSARCH),SunOS)
  CFLAGS+=-Wcast-align -DSOLARIS
  CFLAGS+=-I../asterisk/include/solaris-compat -L/usr/sfw/lib -L/usr/local/lib -R/usr/sfw/lib -I$(CROSS_COMPILE_TARGET)/usr/local/ssl/include
endif

CFLAGS+=-fPIC
CFLAGS+=-I../asterisk/include  
CFLAGS+=-D_GNU_SOURCE

INSTALL=install
INSTALL_PREFIX=
MODULES_DIR=$(ASTLIBDIR)/modules

#
# MySQL stuff...  Autoconf anyone??
#

# find the default mysql packages from Sun
MODS+=res_config_mysql.so cdr_addon_mysql.so
CFLAGS+=-I/usr/sfw/include/mysql
MLFLAGS+=-L/usr/sfw/lib -R/usr/sfw/lib

# MODS+=$(shell if [ -d /opt/csw/mysql5/include ]; then echo "res_config_mysql.so cdr_addon_mysql.so"; fi)
# CFLAGS+=$(shell if [ -d /opt/csw/mysql5/include ]; then echo "-I/opt/csw/mysql5/include/mysql"; fi)
# MLFLAGS+=$(shell if [ -d /opt/csw/mysql5/lib ]; then echo "-L/opt/csw/mysql5/lib/mysql"; fi)


ifeq (${OSARCH},Darwin)
SOLINK=-dynamic -bundle -undefined suppress -force_flat_namespace
else
SOLINK=-shared -Xlinker -x
endif
ifeq (${OSARCH},SunOS)
SOLINK=-shared -fpic -L$(CROSS_COMPILE_TARGET)/usr/local/ssl/lib
OBJS+=strcompat.o
endif

all: depend $(MODS)

format_mp3/format_mp3.so:
	$(MAKE) -C format_mp3 all

install: all
	for x in $(MODS); do $(INSTALL) -m 755 $$x $(DESTDIR)$(MODULES_DIR) ; done

clean:
	rm -f *.so *.o .depend
	$(MAKE) -C format_mp3 clean

%.so : %.o
	$(CC) $(SOLINK) -o $@ $<

ifneq ($(wildcard .depend),)
include .depend
endif

cdr_addon_mysql.so: cdr_addon_mysql.o
	$(CC) $(SOLINK) -o $@ $< -lmysqlclient -lz $(MLFLAGS)

res_config_mysql.so: res_config_mysql.o
	$(CC) $(SOLINK) -o $@ $< -lmysqlclient -lz $(MLFLAGS)

# app_addon_sql_mysql.so: app_addon_sql_mysql.o
#	$(CC) $(SOLINK) -o $@ $< -lmysqlclient -lz $(MLFLAGS)

depend: .depend

.depend:
	./mkdep $(CFLAGS) `ls *.c`

update:
	@if [ -d .svn ]; then \
		echo "Updating from Subversion..." ; \
		svn update -q; \
	elif [ -d CVS ]; then \
		echo "Updating from CVS..." ; \
		cvs -q -z3 update -Pd; \
	else \
		echo "Not under version control"; \
	fi

REV     = $(shell date +'%Y.%m.%d.%H.%M')
ifneq ($(wildcard /usr/bin/pkgparam),)
ARCH    = $(shell pkgparam SUNWcsr ARCH)
else
ARCH    = $(shell uname -p)
endif
PKGARCHIVE = $(shell pwd)/$(shell uname -s)-$(shell uname -r).$(shell uname -m)

pkg: svnver all pkgdepend $(PKGARCHIVE) pkginfo
	pkgmk -oa `uname -m` -d $(PKGARCHIVE) -f prototype
	pkgtrans -s $(PKGARCHIVE) SVasterisk-addons-`uname -m`-`uname -r`.pkg SVasterisk-addons

pkgdepend:
	[ -f res_config_mysql.so ] || ( echo "Asterisk is required at ../asterisk" && exit 1 )

$(PKGARCHIVE):
	test -d $@ || mkdir -p $@

pkginfo:
	sed 's/<version>/$(ASTERISKVERSIONNUM),REV=$(REV)/' < pkginfo_src | sed 's/<arch>/$(ARCH)/' > $@

svnver:
	echo "asterisk-adonns-1.2.7.1-solvoip-`svn info | grep Revision | awk '{ print $$2 }'`" >.version


