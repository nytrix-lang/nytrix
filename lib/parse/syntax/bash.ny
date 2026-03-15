;; Keywords: syntax bash shell
;; Bash syntax highlighter
module std.parse.syntax.bash(tokenize)
use std.parse.syntax.helpers as _h

def KW = "if;then;else;elif;fi;case;esac;for;while;until;do;done;in;function;select;time;coproc;true;false;return;exit;break;continue;shift;exec;eval;set;unset;export;local;readonly;declare;typeset;let;source;alias;unalias;builtin;command;type;hash;help;bind;complete;compgen;shopt;trap;wait;suspend;logout;umask;ulimit;getopts;read;mapfile;readarray"
def FN = "echo;printf;cat;grep;sed;awk;sort;uniq;wc;head;tail;cut;tr;tee;find;xargs;mkdir;rm;cp;mv;ln;chmod;chown;chgrp;ls;cd;pwd;pushd;popd;dirs;test;date;sleep;yes;seq;basename;dirname;realpath;readlink;mktemp;touch;stat;file;diff;patch;tar;gzip;gunzip;zip;unzip;ssh;scp;rsync;curl;wget;ping;netstat;ss;ip;ifconfig;route;ps;top;kill;pkill;killall;jobs;bg;fg;nohup;disown"

fn tokenize(str: source, list: out_tokens): list {
   _h.tokenize_c_like(source, out_tokens, KW, "", FN, "", ".xeE+-", "|&;><!=-+*/%^~", "()[]{}\",\\`", -1, false, 4, false, true)
}
