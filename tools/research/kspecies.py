#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Walker species of the kangaroo with residues (docs/en/ALGORITHMS.md section 8.8): scheme 2 of the program.

Each species s has E = b_s * x + u, with u in a zone (tame: [-eps, eps] W; b > 0: [-(b + 0.005), 0.005] W) and in classes
of residues mod M; the jumps are multiples of M, so every walker keeps its residue. The exact pair-coverage model
(equations eq1 with D = b - b' and eq2 with D = b + b', on the integer grid) gives lambda(x); the expected cost is the mean
of (sqrt(pi)/2) / sqrt(lambda(x)) (random walks, Jensen penalty included).

Step 1: enumerate the assignments of cosets (a mod m, m | M) to the species with equal weights; step 2: starting from the
best ones, optimize the share of every (species, residue) pair by exponentiated gradient.

    python3 -I tools/research/kspecies.py 0,1,3 12 768 0.023 40
prints the cost of the best scheme and the shares (b, residue, share) above 0.5%, up to a translation x -> x + t that
shifts the residue of each species by b * t (KSPEC is the printed solution with t = 1). Needs numpy. The result for
0,1,3 mod 12 (cost ~1.045 in the model, against ~1.09 for the mixtures without residues of docs/en/RESEARCH.md 8.7) is the KSPEC table of
src/kangaroo.c; tools/research/kang_schema2.txt is the same for tools/research/ksim.c (design=).
"""
import numpy as np, itertools, sys, math
# Species s: coefficient b_s, zone (tame: [-eps, eps]; b>0: [-b*(1+dl), dl]), offsets in a coset C_s = {r : r = a mod m} of Z_M.
# lambda(x) = sum_{i<=j} w_i w_j (1/2 if i=j) [G1_ij(x) F1_ij(x mod M) + G2_ij(x) F2_ij(x mod M)]
# G: continuous overlap (exact on the integer grid with M = 1), F: residue factor M |{r in C_i : r + D1 x in C_j}| / (|C_i||C_j|) (eq2: r_i + r_j = -D2 x).
def dist(lo,hi,W,off,L):
    a=int(np.floor(lo*W)); b=int(np.ceil(hi*W)); u=np.arange(a,b+1)
    d=np.zeros(L); d[u+off]=1.0/len(u); return d
def geometry(bs,zones,W):
    span=max(max(abs(z[0]),abs(z[1])) for z in zones); bmax=max(bs)
    off=int((span+0.01)*W)+2; L=1
    while L<4*off+4*bmax*W+8: L*=2
    F=[np.fft.rfft(dist(z[0],z[1],W,off,L)) for z in zones]
    n=len(bs); x=np.arange(W); G1=np.zeros((n,n,W)); G2=np.zeros((n,n,W))
    for i in range(n):
        for j in range(i,n):
            if bs[i]!=bs[j]:
                R=np.fft.irfft(np.conj(F[i])*F[j],L); G1[i,j]=R[((bs[i]-bs[j])*x)%L]
            if bs[i]+bs[j]!=0:
                C=np.fft.irfft(F[i]*F[j],L); G2[i,j]=C[(-(bs[i]+bs[j])*x+2*off)%L]
    return G1,G2
def cosets(M):
    out=[]
    for m in range(1,M+1):
        if M%m==0:
            for a in range(m): out.append(tuple(r for r in range(M) if r%m==a))
    return out
def factors(bs,cs,M):
    n=len(bs); F1=np.zeros((n,n,M)); F2=np.zeros((n,n,M))
    for i in range(n):
        for j in range(i,n):
            Ci,Cj=cs[i],set(cs[j]); norm=M/(len(cs[i])*len(cs[j]))
            d1=bs[i]-bs[j]; d2=bs[i]+bs[j]
            for xr in range(M):
                if d1!=0: F1[i,j,xr]=norm*sum(1 for r in Ci if (r+d1*xr)%M in Cj)
                if d2!=0: F2[i,j,xr]=norm*sum(1 for r in Ci if (-d2*xr-r)%M in Cj)
    return F1,F2
def lam_parts(G1,G2,F1,F2,M,W):
    n=G1.shape[0]; xr=np.arange(W)%M
    A=np.zeros((n,n,W))
    for i in range(n):
        for j in range(i,n):
            v=G1[i,j]*F1[i,j][xr]+G2[i,j]*F2[i,j][xr]
            f=0.5 if i==j else 1.0
            A[i,j]=v*f
    return A
def cost_w(w,A,W):
    lam=np.einsum('i,ijx,j->x',w,A,w)
    if lam.min()<=0: return 9.0
    return float(np.mean(np.sqrt(np.pi)/2/np.sqrt(lam))/np.sqrt(W))
def best_weights(A,W,n):
    # simplex grid then local refinement
    best=(9,None); step=0.05
    grid=[w for w in itertools.product(np.arange(0.05,0.951,step),repeat=n-1) if sum(w)<0.96]
    for g in grid:
        w=np.array(list(g)+[1-sum(g)]); c=cost_w(w,A,W)
        if c<best[0]: best=(c,w)
    c,w=best
    for s in (0.02,0.01,0.005):
        improved=True
        while improved:
            improved=False
            for i in range(n):
                for j in range(n):
                    if i==j: continue
                    w2=w.copy(); w2[i]+=s; w2[j]-=s
                    if w2[j]<=0: continue
                    c2=cost_w(w2,A,W)
                    if c2<c: c,w,improved=c2,w2,True
    return c,w
# full residue distributions: combos (species s, residue r); lambda = sum_{c<=c'} ...; build per-combo factor via cosets of single residues
def combo_A(bs,M,G1,G2,W):
    combos=[(s,r) for s in range(len(bs)) for r in range(M)]
    n=len(combos); xr=np.arange(W)%M
    A=np.zeros((n,n,W))
    for i,(si,ri) in enumerate(combos):
        for j,(sj,rj) in enumerate(combos):
            if j<i: continue
            bi,bj=bs[si],bs[sj]; a,b=min(si,sj),max(si,sj)
            d1=bi-bj; d2=bi+bj; v=np.zeros(W)
            if d1!=0:
                ok=((ri+d1*xr-rj)%M==0); v+=G1[a,b]*ok*M
            if d2!=0:
                ok=((d2*xr+ri+rj)%M==0); v+=G2[a,b]*ok*M
            A[i,j]=v*(0.5 if i==j else 1.0)
    return combos,A
def eg(w,A,W,iters=1500):
    c=cost_w(w,A,W); eta=0.3
    for it in range(iters):
        lam=np.einsum('i,ijx,j->x',w,A,w)
        Asym=A+np.transpose(A,(1,0,2))
        g=-0.25*np.sqrt(np.pi)*np.einsum('x,ijx,j->i',lam**-1.5,Asym,w)/W/np.sqrt(W)
        w2=w*np.exp(-eta*g/np.abs(g).max()); w2/=w2.sum()
        c2=cost_w(w2,A,W)
        if c2<c: w,c=w2,c2; eta=min(eta*1.2,3)
        else: eta*=0.5
        if eta<1e-7: break
    return c,w
if __name__=='__main__':
    bs=[int(v) for v in sys.argv[1].split(',')]; M=int(sys.argv[2]); W=int(sys.argv[3]); eps=float(sys.argv[4])
    zones=[(-eps,eps) if b==0 else (-b-0.005,0.005) for b in bs]
    G1,G2=geometry(bs,zones,W)
    # make G symmetric-indexed (geometry fills i<=j)
    combos,A=combo_A(bs,M,G1,G2,W)
    cs_all=cosets(M)
    # seeds: coset assignments screened by equal weights
    scr=[]
    for cs in itertools.product(cs_all,repeat=len(bs)):
        F1,F2=factors(bs,cs,M); Ac=lam_parts(G1,G2,F1,F2,M,W)
        w0=np.array([1/3]+[(2/3)/(len(bs)-1)]*(len(bs)-1)); scr.append((cost_w(w0,Ac,W),cs))
    scr.sort(key=lambda t:t[0])
    rng=np.random.default_rng(3); best=(9,None)
    seeds=[cs for _,cs in scr[:int(sys.argv[5]) if len(sys.argv)>5 else 40]]
    for cs in seeds:
        w=np.zeros(len(combos))
        for i,(s,r) in enumerate(combos):
            if r in cs[s]: w[i]=(1/3 if bs[s]==0 else (2/3)/(len(bs)-1))/len(cs[s])
        w+=1e-4*rng.random(len(w)); w/=w.sum()
        c,w=eg(w,A,W)
        if c<best[0]: best=(c,w); print('  %.4f'%c); sys.stdout.flush()
    c,w=best
    print('BEST %s M=%d eps=%.3f: %.4f'%(bs,M,eps,c))
    for (s,r),x in zip(combos,w):
        if x>0.005: print('   b=%d r=%2d %.3f'%(bs[s],r,x))
