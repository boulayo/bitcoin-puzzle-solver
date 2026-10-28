#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bands for the kangaroo species (docs/en/ALGORITHMS.md section 8.10): each starting zone is split into bands with a share of its own,
optimized in the exact coverage model of tools/research/kspecies.py. The bands of scheme 3 (tools/research/kang_schema3.txt) come from:

    python3 tools/research/kbands.py '{"M":12,"W":384,"spec":[[0,[0,3,6,9],-0.08,0.08,8],[1,[3,7,11],-1.2,0.2,14],[3,[9],-3.4,0.4,19]],
                              "init":[0.294,0.349,0.358],"design":"kang_schema3.txt"}'

spec: [coefficient, allowed residues, start, end (in units of W), number of bands]; init: initial share per species; design: file for
tools/research/ksim.c (design=) and for tools/research/kspec_table.py. Needs numpy; takes a few minutes.
"""
import numpy as np, sys, json, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kspecies import geometry, cost_w
# species = (b, allowed residues, zone lo, zone hi, bins); every (b, residue, bin) is a combo with its own weight
def build(spec,M,W):
    ents=[]  # (b, zone) per bin
    for b,res,lo,hi,nb in spec:
        edges=np.linspace(lo,hi,nb+1)
        for k in range(nb): ents.append((b,(edges[k],edges[k+1]),res))
    bs=[e[0] for e in ents]; zones=[e[1] for e in ents]
    G1,G2=geometry(bs,zones,W)
    combos=[(i,r) for i,e in enumerate(ents) for r in e[2]]
    n=len(combos); xr=np.arange(W)%M; A=np.zeros((n,n,W))
    for p,(i,ri) in enumerate(combos):
        for q,(j,rj) in enumerate(combos):
            if q<p: continue
            a,c=min(i,j),max(i,j); bi,bj=bs[i],bs[j]; d1=bi-bj; d2=bi+bj; v=np.zeros(W)
            if d1!=0: v+=G1[a,c]*((ri+d1*xr-rj)%M==0)*M
            if d2!=0: v+=G2[a,c]*((d2*xr+ri+rj)%M==0)*M
            A[p,q]=v*(0.5 if p==q else 1.0)
    return ents,combos,A
def eg(w,A,W,iters=4000):
    c=cost_w(w,A,W); eta=0.3
    for it in range(iters):
        lam=np.einsum('i,ijx,j->x',w,A,w)
        As=A+np.transpose(A,(1,0,2))
        g=-0.25*np.sqrt(np.pi)*np.einsum('x,ijx,j->i',lam**-1.5,As,w)/W/np.sqrt(W)
        w2=w*np.exp(-eta*g/np.abs(g).max()); w2/=w2.sum(); c2=cost_w(w2,A,W)
        if c2<c: w,c=w2,c2; eta=min(eta*1.2,3)
        else: eta*=0.5
        if eta<1e-9: break
    return c,w
if __name__=='__main__':
    cfg=json.loads(sys.argv[1]); M=cfg['M']; W=cfg.get('W',768)
    spec=[(s[0],s[1],s[2],s[3],s[4]) for s in cfg['spec']]
    ents,combos,A=build(spec,M,W)
    w=np.ones(len(combos))/len(combos)
    if 'init' in cfg:   # initial share per species index
        sh=cfg['init']; w=np.array([sh[[k for k,s in enumerate(spec) if s[0]==ents[i][0] and ents[i][2]==s[1]][0]] for i,r in combos],float); w/=w.sum()
    c0=cost_w(w,A,W); c,w=eg(w,A,W)
    print('initial cost %.4f, optimized %.4f (combinations %d)'%(c0,c,len(combos)))
    out=[]
    for (i,r),x in zip(combos,w):
        b,(lo,hi),_=ents[i]
        if x>1e-4: out.append((b,r,lo,hi,x))
    for o in out: print('   %d %2d % .4f % .4f %.4f'%o)
    if 'design' in cfg:
        with open(cfg['design'],'w') as f:
            f.write('%d\n'%M)
            for b,r,lo,hi,x in out: f.write('%d %d %.5f %.5f %.5f\n'%(b,r,lo,hi,x))
