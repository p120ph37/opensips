# New ports collection makefile for:    opensips
#
# $OpenBSD$
#

COMMENT= "OpenSIPS"

PORTNAME= opensips
PORTVERSION= 3.1-dev
CATEGORIES= net
MASTER_SITES= https://opensips.org/pub/opensips/3.1.0/

MAINTAINER= bogdan@opensips.org

MAN8=opensips.8
MAN5=opensips.cfg.5
# MANCOMPRESSED must not be defined in openbsd (defined==yes)

# GPL
PERMIT_PACKAGE_CDROM=	Yes
PERMIT_PACKAGE_FTP=	Yes
PERMIT_DISTFILES_CDROM=	Yes
PERMIT_DISTFILES_FTP=	Yes


USE_GMAKE= yes
# we override the DISTFILE NAME
DISTNAME= ${PORTNAME}-${PORTVERSION}
DISTFILES= ${DISTNAME}_src.tar.gz
WRKSRC= ${WRKDIR}/${PORTNAME}-${PORTVERSION}

# build by default mysql and jabber
MAKE_ENV= include_modules="mysql jabber"

# depends on mysql and libexpat because of the above modules
BUILD_DEPENDS= :expat-1.*:textproc/expat \
               :mysql-client-3.23.*:databases/mysql
LIB_DEPENDS=   expat.2:expat-1.*:textproc/expat \
               lib/mysql/mysqlclient.10:mysql-client-3.23.*:databases/mysql



pre-fetch:


.include <bsd.port.mk>


