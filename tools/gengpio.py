#!/usr/bin/env python3
"""Generates drv/gpiotab.h (Intel GPIO pad communities and groups, Bay Trail
pad maps) from a Linux tree:  tools/gengpio.py <linux> drv/gpiotab.h"""
import re, glob, os, sys
D=os.path.join(sys.argv[1], 'drivers/pinctrl/intel')
OUTF=sys.argv[2]
files=[f for f in sorted(glob.glob(D+'/pinctrl-*.c')) if os.path.basename(f) not in
       ('pinctrl-intel.c','pinctrl-baytrail.c','pinctrl-cherryview.c','pinctrl-lynxpoint.c','pinctrl-merrifield.c',
        'pinctrl-moorefield.c','pinctrl-tangier.c','pinctrl-intel-platform.c')]
def num(x, defs):
    x=x.strip()
    if x in defs: return num(defs[x], defs)
    if x=='INTEL_GPIO_BASE_NOMAP': return -1
    if x=='INTEL_GPIO_BASE_ZERO': return -2
    if x=='INTEL_GPIO_BASE_MATCH': return 0
    return int(x,0)
socs=[]
for f in files:
    s=open(f).read()
    s=re.sub(r'/\*.*?\*/','',s,flags=re.S)
    defs={}
    for m in re.finditer(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$', s, re.M): defs[m.group(1)]=m.group(2)
    # community macros
    cmac={}
    for m in re.finditer(r'#define\s+(\w+)\(([^)]*)\)\s*\\\s*\n\s*(INTEL_COMMUNITY_GPPS|INTEL_COMMUNITY_SIZE)\(([^)]*)\)', s):
        cmac[m.group(1)]=(m.group(2).split(','), m.group(3), m.group(4).split(','))
    gppmac={}
    for m in re.finditer(r'#define\s+(\w+)\(([^)]*)\)\s*\\\s*\n\s*INTEL_GPP\(([^)]*)\)', s):
        gppmac[m.group(1)]=(m.group(2).split(','), m.group(3).split(','))
    gpps={}
    for m in re.finditer(r'static const struct intel_padgroup (\w+)\[\]\s*=\s*\{(.*?)\};', s, re.S):
        L=[]
        for g in re.finditer(r'(\w+)\(([^)]*)\)', m.group(2)):
            name, args = g.group(1), [a.strip() for a in g.group(2).split(',')]
            if name in gppmac:
                params=[p.strip() for p in gppmac[name][0]]; body=[b.strip() for b in gppmac[name][1]]
                env=dict(zip(params,args)); args=[env.get(b,b) for b in body]
            elif name!='INTEL_GPP': continue
            r,st,e,gb=[num(a,defs) for a in args]
            L.append((st,e,gb))
        gpps[m.group(1)]=L
    comms={}
    for m in re.finditer(r'static const struct intel_community (\w+)\[\]\s*=\s*\{(.*?)\};', s, re.S):
        L=[]
        for c in re.finditer(r'(\w+)\(([^)]*)\)', m.group(2)):
            name,args=c.group(1),[a.strip() for a in c.group(2).split(',')]
            if name in cmac:
                params=[p.strip() for p in cmac[name][0]]; kind=cmac[name][1]; body=[b.strip() for b in cmac[name][2]]
                env=dict(zip(params,args)); args=[env.get(b,b) for b in body]
            elif name in ('INTEL_COMMUNITY_GPPS','INTEL_COMMUNITY_SIZE'): kind=name
            else: continue
            b,st,e=num(args[0],defs),num(args[1],defs),num(args[2],defs)
            com=-1
            if kind=='INTEL_COMMUNITY_GPPS':
                cm=re.search(r'community(\d+)', args[3])
                if cm: com=int(cm.group(1))
                g=gpps[args[3]]
                G=[]
                for gs,ge,gb in g:
                    if gb==0: gb=gs
                    elif gb==-2: gb=0
                    G.append((gs,ge-gs+1,gb))
            else:
                size=num(args[3],defs)
                G=[]
                p=st
                while p<=e:
                    n=min(size,e-p+1); G.append((p,n,p)); p+=n
            L.append((b,st,e-st+1,G,com))
        comms[m.group(1)]=L
    socd={}; uids={}
    for m in re.finditer(r'static const struct intel_pinctrl_soc_data (\w+)\s*=\s*\{(.*?)\};', s, re.S):
        c=re.search(r'\.communities\s*=\s*(\w+)', m.group(2))
        u=re.search(r'\.uid\s*=\s*"(\w+)"', m.group(2))
        if c: socd[m.group(1)]=c.group(1)
        if u: uids[m.group(1)]=int(u.group(1))
    arrays={}
    for m in re.finditer(r'static const struct intel_pinctrl_soc_data \*(\w+)\[\]\s*=\s*\{(.*?)\};', s, re.S):
        arrays[m.group(1)]=re.findall(r'&(\w+)', m.group(2))
    for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*\(kernel_ulong_t\)\s*(&?)(\w+)\s*\}', s):
        hid,amp,sd=m.group(1),m.group(2),m.group(3)
        if amp and sd in socd: socs.append((hid,None,os.path.basename(f),comms[socd[sd]]))
        elif not amp and sd in arrays:
            for e in arrays[sd]: socs.append((hid,uids[e],os.path.basename(f),comms[socd[e]]))
        else: print('skip',hid,sd,file=sys.stderr)
# emit C
out=[]
seen={}
idx=0
for hid,uid,f,cl in socs:
    key=repr(cl)
    if key not in seen:
        name='g_Soc%d'%idx; idx+=1; seen[key]=name
        out.append('/* %s */'%f)
        gl=[]
        for ci,(b,st,n,G,com) in enumerate(cl):
            gl.append('static const GPP %s_c%d[] = { %s };'%(name,ci,', '.join('{ %d, %d, %d }'%g for g in G)))
        out+=gl
        out.append('static const COMM %s[] = { %s, { 0xFF } };'%(name, ', '.join('{ %d, %d, %d, %d, %d, %s_c%d }'%(b,com,st,n,len(G),name,ci) for ci,(b,st,n,G,com) in enumerate(cl))))
FAM={'pinctrl-sunrisepoint.c':1,'pinctrl-cannonlake.c':2,'pinctrl-icelake.c':2,'pinctrl-tigerlake.c':2,
     'pinctrl-jasperlake.c':2,'pinctrl-alderlake.c':2,'pinctrl-elkhartlake.c':3,'pinctrl-meteorlake.c':4,
     'pinctrl-broxton.c':5,'pinctrl-geminilake.c':6}
out.append('static const SOC g_Socs[] = {')
for hid,uid,f,cl in socs: out.append('    { "%s", %d, %d, %s },  /* %s */'%(hid, 255 if uid is None else uid, FAM.get(f,0), seen[repr(cl)], f[8:-2]))
out.append('};')
# Bay Trail pad maps (ACPI pin -> pad register index)
b=open(os.path.join(D,'pinctrl-baytrail.c')).read()
for name,cname in (('byt_score_pins_map','g_BytScore'),('byt_ncore_pins_map','g_BytNcore'),('byt_sus_pins_map','g_BytSus')):
    m=re.search(r'static const unsigned int %s\[\w+\] = \{(.*?)\};'%name, b, re.S)
    v=[int(x) for x in re.findall(r'\d+', re.sub(r'/\*.*?\*/','',m.group(1),flags=re.S))]
    out.append('static const UCHAR %s[%d] = {'%(cname,len(v)))
    for i in range(0,len(v),24): out.append('    '+', '.join(str(x) for x in v[i:i+24])+',')
    out.append('};')
open(OUTF,'w').write('/* generated by tools/gengpio.py from Linux drivers/pinctrl/intel - do not edit */\n'+'\n'.join(out)+'\n')
print('%s: %d controller ids, %d community tables'%(OUTF,len(socs),len(seen)))
