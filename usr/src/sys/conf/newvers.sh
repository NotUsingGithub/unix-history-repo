#!/bin/sh -
#
# Copyright (c) 1980, 1986 Regents of the University of California.
# All rights reserved.  The Berkeley software License Agreement
# specifies the terms and conditions for redistribution.
#
#	@(#)newvers.sh	7.2 (Berkeley) %G%
#
if [ ! -r version ]
then
	echo 0 > version
fi
touch version
v=`cat version` u=${USER-root} d=`pwd` h=`hostname` t=`date`
( echo "char sccs[] = \"@(#)4.3 BSD #${v}: ${t} (${u}@${h}:${d})\\n\";" ;
  echo "char version[] = \"4.3 BSD UNIX #${v}: ${t}\\n    ${u}@${h}:${d}\\n\";"
) > vers.c
echo `expr ${v} + 1` > version
