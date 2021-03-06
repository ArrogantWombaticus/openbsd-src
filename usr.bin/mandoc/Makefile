#	$OpenBSD: Makefile,v 1.102 2016/07/10 10:03:15 schwarze Exp $

.include <bsd.own.mk>

CFLAGS  += -W -Wall -Wstrict-prototypes -Wno-unused-parameter
DPADD += ${LIBUTIL}
LDADD	+= -lsqlite3 -lutil -lz

SRCS=	mandoc.c mandoc_aux.c mandoc_ohash.c preconv.c read.c \
	roff.c tbl.c tbl_opts.c tbl_layout.c tbl_data.c eqn.c
SRCS+=	mdoc_macro.c mdoc.c mdoc_hash.c \
	mdoc_argv.c mdoc_state.c mdoc_validate.c att.c msec.c st.c
SRCS+=	man_macro.c man.c man_hash.c man_validate.c
SRCS+=	main.c mdoc_term.c tag.c chars.c term.c tree.c man_term.c eqn_term.c
SRCS+=	mdoc_man.c
SRCS+=	html.c mdoc_html.c man_html.c out.c eqn_html.c
SRCS+=	term_ps.c term_ascii.c tbl_term.c tbl_html.c
SRCS+=	manpath.c mandocdb.c mansearch_const.c mansearch.c

PROG=	mandoc

LINKS =	${BINDIR}/mandoc ${BINDIR}/apropos \
	${BINDIR}/mandoc ${BINDIR}/help \
	${BINDIR}/mandoc ${BINDIR}/man \
	${BINDIR}/mandoc ${BINDIR}/whatis \
	${BINDIR}/mandoc /usr/sbin/makewhatis \
	${BINDIR}/mandoc /usr/libexec/makewhatis

MAN =	apropos.1 man.1 mandoc.1 man.conf.5 makewhatis.8

CLEANFILES += man.cgi cgi.o


# ----------------------------------------------------------------------
# Variables and targets to build and install man.cgi(8),
# not used during make build and make release.

# To configure, run:	cp cgi.h.example cgi.h; vi cgi.h
# To build, run:	make man.cgi
# To install, run:	sudo make installcgi
# After that, read:	man man.cgi.8

LIBMDOC_OBJS =	mdoc_argv.o mdoc_hash.o mdoc_macro.o mdoc_state.o \
		mdoc_validate.o mdoc.o att.o st.o
LIBMAN_OBJS =	man.o man_hash.o man_macro.o man_validate.o
LIBROFF_OBJS =	roff.o eqn.o tbl.o tbl_data.o tbl_layout.o tbl_opts.o
LIBMANDOC_OBJS = ${LIBMDOC_OBJS} ${LIBMAN_OBJS} ${LIBROFF_OBJS} \
		mandoc.o mandoc_aux.o mandoc_ohash.o \
		chars.o msec.o preconv.o read.o
HTML_OBJS =	html.o mdoc_html.o man_html.o tbl_html.o eqn_html.o out.o
CGI_OBJS =	${LIBMANDOC_OBJS} ${HTML_OBJS} \
		mansearch.o mansearch_const.o cgi.o

cgi.o: cgi.h main.h manconf.h mandoc.h mandoc_aux.h mansearch.h \
	man.h mdoc.h roff.h

man.cgi: ${CGI_OBJS}
	${CC} ${LDFLAGS} ${STATIC} -o ${.TARGET} ${CGI_OBJS} ${LDADD}

installcgi: man.cgi
	${INSTALL} -d -o root -g wheel -m 755 ${DESTDIR}/var/www/cgi-bin
	${INSTALL} ${INSTALL_COPY} -S ${INSTALL_STRIP} \
	    -o ${BINOWN} -g ${BINGRP} -m ${BINMODE} \
	    man.cgi ${DESTDIR}/var/www/cgi-bin/man.cgi

.include <bsd.prog.mk>
