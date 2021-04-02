#!/usr/bin/python3
import os
import subprocess

def conv(n, c, h, w, f, wc, r, s, ph, pw, sh, sw, dh, dw):
    cmd = "./conv -n %d -c %d -h %d -w %d -f %d -g %d -r %d -s %d -ph %d -pw %d -sh %d -sw %d -dh %d -pw %d" % (n, wc, h, w, f, c/wc, r, s, ph, pw, sh, sw, dh, dw)
    result = subprocess.run(cmd, shell=True, capture_output=True)
    pos = result.stdout.decode().find('best time')
    t = float(result.stdout[pos:].split()[-1])
    return t

def expr_origin():
    t1 = conv(1, 768, 18, 18, 192, 768, 1, 1, 0, 0, 1, 1, 1, 1)
    t2 = conv(1, 768, 18, 18, 160, 768, 1, 1, 0, 0, 1, 1, 1, 1)
    print("origin time: %f" % ((t1 + t2) *2))

def expr_opt():
    t1 = conv(1, 768*4, 18, 18, 192, 768, 1, 1, 0, 0, 1, 1, 1, 1)
    t2 = conv(1, 768*11, 18, 18, 32, 768, 1, 1, 0, 0, 1, 1, 1, 1)
    print("opt time: %f" % min(t1, t2))

if __name__ == "__main__":
    expr_origin()
    expr_opt()
