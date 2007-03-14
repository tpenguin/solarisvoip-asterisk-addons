ASTDIR=/usr/src/asterisk
ASTXS=$(ASTDIR)/contrib/scripts/astxs
ASTSQLITE=/var/lib/asterisk/sqlite
SQLITE2=/usr/local/bin/sqlite
SQLITE3=/usr/local/bin/sqlite3
SQLITEBASE=http://www.sqlite.org/
SQLITEFILE=sqlite-3.2.1

CFLAGS=-I$(SQLITEFILE) -I $(SQLITEFILE)/src

all: .sqlite res_sqlite.so


res_sqlite.so:
	ASTSRC=$(ASTDIR) $(ASTXS) "-append=CFLAGS:$(CFLAGS)" "-append=EXTOBJ:./$(SQLITEFILE)/.libs/libsqlite3.a" res_sqlite.c

.sqlite:
	wget $(SQLITEBASE)/$(SQLITEFILE).tar.gz
	tar -zxvf $(SQLITEFILE).tar.gz
	cd $(SQLITEFILE) && test -f ./config.status || ./configure --enable-threadsafe
	cd $(SQLITEFILE) && make
	touch .sqlite

clean:
	/bin/rm -f *.o *.so *~

distclean: clean
	/bin/rm -fr .sqlite sqlite $(SQLITEFILE) $(SQLITEFILE).tar.gz

install:
	$(ASTXS) -autoload -install res_sqlite.so
	/bin/mkdir -p $(ASTSQLITE)
	if [ ! -f  /etc/asterisk/res_sqlite.conf ] ; then /bin/cp res_sqlite.conf /etc/asterisk; fi

sqlite_install: .sqlite
	cd sqlite && make install
	test -f /etc/ld.so.conf && ldconfig

upgrade: $(SQLITE2) $(SQLITE3)
	@echo upgrading database files 
	@upgrade.sh $(ASTSQLITE) $(SQLITE2) $(SQLITE3)



