#!/usr/bin/env python3
# Generate the JXMPOSE Studio colour theme from the JXMPOSE Hardware theme:
#   tools/jxmp-mktheme.py gtk2_ardour/themes/jxmp-jxmpose.colors gtk2_ardour/themes/jxmp_studio-jxmpose.colors
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
t = t.replace('theme-name="JXMPOSE Hardware"', 'theme-name="JXMPOSE Studio"')
pal = {
 "alert:blue":"3F7FD9","alert:cyan":"3BB8D0","alert:green":"49C46E","alert:greenish":"3FA867",
 "alert:orange":"F08C2E","alert:red":"E5484D","alert:ruddy":"C24A3A","alert:yellow":"F2C94C",
 "neutral:background":"2D2F31","neutral:background2":"3A3D40","neutral:backgroundest":"111314",
 "neutral:foreground":"D8DADB","neutral:foreground2":"8B8D8E","neutral:foregroundest":"F2F2F2",
 "neutral:midground":"3F4245",
 "theme:bg":"232628","theme:bg1":"1C1E20","theme:bg2":"17191A",
 "theme:contrasting":"E8E8E8","theme:contrasting alt":"F2B33D","theme:contrasting clock":"E6E8E9",
 "theme:contrasting less":"4FA36B","theme:contrasting selection":"3F7FD9",
 "widget:bg":"444648","widget:blue":"3F7FD9","widget:blue darker":"2B2E31","widget:blue lighter":"6AA0F0",
 "widget:gray":"5A5D60","widget:green":"3E6E55","widget:green darker":"28322D","widget:orange":"D9873A",
 "widget:ruddy":"7A3530","widget:yellow":"F2C94C","widget:cap white":"A4A9AD","widget:cap grey":"656768",
}
alias = {
 "mute button: fill active":"alert:yellow","mute button: led active":"alert:yellow",
 "solo button: fill active":"alert:red","solo button: led active":"alert:red",
 "monitor button: fill active":"alert:orange","monitor button: led active":"alert:orange",
 "record enable button: fill active":"alert:red","record enable button: led active":"alert:red",
 "transport recenable button: fill active":"alert:red",
 "transport button: fill active":"alert:green",
 "grid line major":"neutral:midground","grid line minor":"neutral:background2",
 "grid line micro":"neutral:background2",
 "selected time axis frame":"widget:blue lighter",
 "ruler text":"neutral:foreground2",
 "big clock: text":"theme:contrasting clock","transport clock: text":"theme:contrasting clock",
}
def sub(kind, attr, table):
    global t
    for k, v in table.items():
        pat = re.compile(r'(<%s name="%s" %s=")[^"]*(")' % (kind, re.escape(k), attr))
        t, n = pat.subn(lambda m: m.group(1) + (v + "ff" if kind == "Color" else v) + m.group(2), t)
        if n != 1: print("WARN", kind, k, n)
sub("Color", "value", pal)
sub("ColorAlias", "alias", alias)
open(dst, "w").write(t)
