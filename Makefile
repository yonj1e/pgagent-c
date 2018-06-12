MODULE_big = pgagent

MYBGW_VERSION=5.0

OBJS = pgagent.o
PG_CPPFLAGS = -std=c99 -Wall -Wformat -Wmissing-prototypes -Wmissing-declarations -Wmissing-format-attribute -lpthread -fPIC -shared -Iinclude -I$(libpq_srcdir)

EXTENSION = pgagent
DATA = pgagent--3.4.sql

SHLIB_LINK += $(filter -lm, $(LIBS)) 
USE_PGXS=1
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
SHLIB_LINK = $(libpq)
include $(PGXS)
else
subdir = contrib/pgagent
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

distrib:
	rm -f *.o
	rm -rf results/ regression.diffs regression.out tmp_check/ log/
	#cd .. ; tar --exclude=.svn -chvzf pgagent-$(MYBGW_VERSION).tar.gz pgagent
