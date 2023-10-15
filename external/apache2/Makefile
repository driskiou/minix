#	$NetBSD: Makefile,v 1.4 2021/10/12 17:24:36 nia Exp $

.include <bsd.own.mk>

.if (${MKLLVM} != "no" || ${MKLLVMRT} != "no")
.if (${MKLLVMCMDS} != "no") && defined(__MINIX)
SUBDIR+= llvm
.endif # (${MKLLVMCMDS} != "no") && defined(__MINIX)
.endif

.include <bsd.subdir.mk>
