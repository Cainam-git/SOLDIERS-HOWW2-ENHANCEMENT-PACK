import numpy as np
def shift(m,dy,dx):
    o=np.zeros_like(m); H,W=m.shape
    ys=slice(max(dy,0),H+min(dy,0)); yd=slice(max(-dy,0),H+min(-dy,0))
    xs=slice(max(dx,0),W+min(dx,0)); xd=slice(max(-dx,0),W+min(-dx,0))
    o[ys,xs]=m[yd,xd]; return o
def blur(m,s):
    if s<=0: return m.astype(np.float32)
    r=int(3*s+1); k=np.exp(-0.5*(np.arange(-r,r+1)/s)**2); k/=k.sum()
    m=m.astype(np.float32); o=np.zeros_like(m)
    p=np.pad(m,((0,0),(r,r)),mode='edge')
    for i,w in enumerate(k): o+=w*p[:,i:i+m.shape[1]]
    p=np.pad(o,((r,r),(0,0)),mode='edge'); o2=np.zeros_like(m)
    for i,w in enumerate(k): o2+=w*p[i:i+m.shape[0]]
    return o2
def dist_to(mask,R=10):
    """distance from each pixel to the nearest True pixel (capped at R+1)"""
    d=np.full(mask.shape,R+1.0,np.float32)
    for dy in range(-R,R+1):
        for dx in range(-R,R+1):
            r=(dy*dy+dx*dx)**0.5
            if r>R: continue
            s=shift(mask,dy,dx)
            d=np.where(s&(d>r),r,d)
    return d
