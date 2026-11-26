# Makes: uiscale_hd/dcc063ce1d707cf1_64x64.raw, the redrawn close button (three states), byte-identical to the release.
# Needs: the 1x dump uiscale_hd/todo/dcc063ce1d707cf1_64x64.raw from the dll; numpy + Pillow.
# Called as: python3 -I tools/xredraw.py GAME_DIR/uiscale_hd/todo/dcc063ce1d707cf1_64x64.raw OUT.raw PREVIEW_DIR
# xredraw.py SRC_1X_RAW OUT_3X_RAW PREVIEW_DIR : vector-like 3x redraw of the 64x64 close-button sheet (three 24x24 states),
# geometry fitted to the game's own 1x pixels (rounded square, 1 px frame, X of two strokes); colours sampled from the 1x art.
import sys, itertools, numpy as np
from PIL import Image
src,out,pv=sys.argv[1:4]
A=np.frombuffer(open(src,'rb').read(),np.uint8).reshape(64,64,4).astype(np.float64)
RGB=A[:,:,2::-1]; AL=A[:,:,3]/255.0          # RGB order
def grid(n,ss):                                # sample centres for an n-px cell, ss samples per px, in 1x units
    t=(np.arange(n*ss)+0.5)/ss; return np.meshgrid(t,t)
def sdf_rr(x,y,r,s=24.0):                      # rounded square [0,s]^2
    qx=np.abs(x-s/2)-(s/2-r); qy=np.abs(y-s/2)-(s/2-r)
    return np.hypot(np.maximum(qx,0),np.maximum(qy,0))+np.minimum(np.maximum(qx,qy),0)-r
def segd(x,y,p,q):
    p=np.array(p);q=np.array(q);d=q-p;t=np.clip(((x-p[0])*d[0]+(y-p[1])*d[1])/(d@d),0,1)
    return np.hypot(x-p[0]-t*d[0],y-p[1]-t*d[1])
def render(P,n,ss,interior):
    m=ss*n//24; x,y=grid(24,m); sd=sdf_rr(x,y,P['r'])
    shape=(sd<0)*1.0; frame=((sd<0)&(sd>-P['wf']))*1.0
    cx,cy,hx,hy=P['cx'],P['cy'],P['hx'],P['hy']
    dx=np.minimum(segd(x,y,(cx-hx,cy-hy),(cx+hx,cy+hy)),segd(x,y,(cx+hx,cy-hy),(cx-hx,cy+hy)))
    xm=(dx<P['wx']/2)*1.0
    col=interior(x,y)
    col=col*(1-frame[...,None])+np.array(P['cf'])*frame[...,None]
    col=col*(1-xm[...,None])+np.array(P['cxc'])*xm[...,None]
    a=shape*P['a']
    pm=np.concatenate([col*a[...,None],a[...,None]],2)
    k=ss; h=pm.shape[0]//k
    return pm.reshape(h,k,h,k,4).mean((1,3))  # premultiplied, box-filtered to n px
def unpm(pm):
    a=pm[...,3]; c=np.where(a[...,None]>1e-6,pm[...,:3]/np.maximum(a[...,None],1e-6),0); return c,a
res=np.zeros((192,192,4))
for name,(x0,y0) in (('normal',(0,24)),('grey',(0,0)),('red',(24,0))):
    c=RGB[y0:y0+24,x0:x0+24]; al=AL[y0:y0+24,x0:x0+24]; tgt=np.concatenate([c*al[...,None],al[...,None]],2)
    yy,xx=np.mgrid[0:24,0:24]+0.5
    # interior: radial profile from pixels away from frame and X diagonals
    far=(sdf_rr(xx,yy,6.5)<-2)&(np.minimum(np.abs((xx-12)-(yy-12)),np.abs((xx-12)+(yy-12)))>2.5)
    d=np.hypot(xx-12,yy-12)[far]; v=c[far]
    co=[np.polyfit(d,v[:,i],2) for i in range(3)]
    interior=lambda X,Y,co=co: np.stack([np.polyval(co[i],np.hypot(X-12,Y-12)) for i in range(3)],-1)
    a0=al.max(); cf=list(c[10,0])
    xs=np.argmax(c.sum(2)*(far==False)*(sdf_rr(xx,yy,6.5)<-1.5)); cxc=list(c.reshape(-1,3)[xs])
    best=None
    for r,wf,cx,cy,hx,hy,wx in itertools.product((6.5,7,7.5,8),(0.8,0.9,1.0),(11.75,12,12.25),(11.75,12,12.25),(5,5.25,5.5,5.75,6),(5,5.25,5.5,5.75,6),(1.0,1.2,1.4,1.6)):
        P=dict(r=r,wf=wf,cx=cx,cy=cy,hx=hx,hy=hy,wx=wx,cf=cf,cxc=cxc,a=a0)
        e=np.abs(render(P,24,4,interior)-tgt/np.array([1,1,1,1])).mean()
        if best is None or e<best[0]: best=(e,P)
    e,P=best; print(name,'err(1x, 0..255 scale) %.2f'%(e*1), {k:v for k,v in P.items() if k not in('cf','cxc')})
    pm=render(P,72,6,interior)                 # 3x, 6x6 supersampling per 3x pixel
    res[y0*3:y0*3+72,x0*3:x0*3+72]=pm
col,a=unpm(res)
o=np.dstack([col[...,2],col[...,1],col[...,0],a*255.0]).round().clip(0,255).astype(np.uint8)
open(out,'wb').write(o.tobytes())
im=Image.frombytes('RGBA',(192,192),o.tobytes(),'raw','BGRA'); bg=Image.new('RGBA',(192,192),(0,0,0,255)); bg.alpha_composite(im)
bg.resize((576,576),Image.NEAREST).save(pv+'/x_redraw3x_onblack_x3.png')
