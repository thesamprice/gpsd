#!/bin/sh
# Distributed under the terms of the GNU General Public License v2
# Copyright (c) 2006 Fernando J. Pereda <ferdy@gentoo.org>
# Copyright (c) 2008 Natanael Copa <natanael.copa@gmail.com>
# Copyright (c) 2010 Eric S. Raymond <esr@thyrsus.com>
#
# Originally based on Git ciabot.pl by Petr Baudis.
# This script contains porcelain and porcelain byproducts.
#
# It is meant to be run either on a post-commit hook or in an update
# hook:
#
# post-commit: It parses latest commit and current HEAD to get the
# information it needs.
#
# update: You have to call it once per merged commit:
#
#       refname=$1
#       oldhead=$2
#       newhead=$3
#       for merged in $(git rev-list ${oldhead}..${newhead} | tac) ; do
#               /path/to/ciabot.bash ${refname} ${merged}
#       done
#

#
# The project as known to CIA. You will want to change this:
#
project="GPSD"

#
# You may not need to change these:
#

# Name of the repository.
# You can hardwire this to make the script faster.
repo="`basename ${PWD}`"

# Fully qualified domain name of the repo host.
# You can hardwire this to make the script faster.
host=`hostname --fqdn`

# Changeset URL prefix for your repo: when the commit ID is appended
# to this, it should point at a CGI that will display the commit
# through gitweb or something similar. The default will probably
# work if you have a typical gitweb setup.
urlprefix="http://${host}/cgi-bin/gitweb.cgi?p=$repo;a=commit;h="

#
# You probably will not need to change the following:
#

# Addresses for the e-mail
from="${LOGNAME}@${host}"
to="cia@cia.vc"

# SMTP client to use - may need to edit the absolute pathname for your system
sendmail="sendmail -t -f ${from}"

#
# No user-serviceable parts below this line:
#

# Should include both places sendmail is likely to lurk 
# and the git private command directory.
PATH="$PATH:/usr/sbin/:`git --exec-path`"

if [ $# -eq 0 ] ; then
	refname=$(git symbolic-ref HEAD 2>/dev/null)
	merged=$(git rev-parse HEAD)
else
	refname=$1
	merged=$2
fi

# This tries to turn your gitwebbish URL into a tinyurl so it will take up
# less space on the IRC notification line. Some repo sites (I'm looking at
# you, berlios.de!) forbid wget calls for security reasons.  On these,
# the code will fall back to the full un-tinyfied URL.
longurl=${urlprefix}${merged}
url=$(wget -O - -q http://tinyurl.com/api-create.php?url=${longurl} 2>/dev/null)
if [ -z "$url" ]; then
	url="${longurl}"
fi

refname=${refname##refs/heads/}

gitver=$(git --version)
gitver=${gitver##* }

rev=$(git describe ${merged} 2>/dev/null)
# ${merged:0:12} here was the only bashism left in the 2008 version of
# this script, according to checkbashisms.  Replace it with ${merged}
# because it was just a fallback anyway, and it's worth taking accepting
# a longer fallback for faster execution and removing the bash deoendency.
[ -z ${rev} ] && rev=${merged}

rawcommit=$(git cat-file commit ${merged})
author=$(echo "$rawcommit" | sed -n -e '/^author .*<\([^@]*\).*$/s--\1-p')
logmessage=$(echo "$rawcommit" | sed -e '1,/^$/d' | head -n 1)
logmessage=$(echo "$logmessage" | sed 's/\&/&amp\;/g; s/</&lt\;/g; s/>/&gt\;/g')
ts=$(echo "$rawcommit" | sed -n -e '/^author .*> \([0-9]\+\).*$/s--\1-p')

out="
<message>
  <generator>
    <name>CIA Shell client for Git</name>
    <version>${gitver}</version>
    <url>http://dev.alpinelinux.org/~ncopa/alpine/ciabot.sh</url>
  </generator>
  <source>
    <project>${project}</project>
    <branch>$repo:${refname}</branch>
  </source>
  <timestamp>${ts}</timestamp>
  <body>
    <commit>
      <author>${author}</author>
      <revision>${rev}</revision>
      <files>
        $(git diff-tree -r --name-only ${merged} |
          sed -e '1d' -e 's-.*-<file>&</file>-')
      </files>
      <log>${logmessage} ${url}</log>
      <url>${url}</url>
    </commit>
  </body>
</message>"

${sendmail} << EOM
Message-ID: <${merged:0:12}.${author}@${project}>
From: ${from}
To: ${to}
Content-type: text/xml
Subject: DeliverXML
${out}
EOM

# vim: set tw=70 :
