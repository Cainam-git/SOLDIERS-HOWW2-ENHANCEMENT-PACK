# Makes: the vehicle-ammo shell file f2a1fe95c1442398_256x64.raw and the MG drum file dc229a7b0bb80a22_64x32.raw.
# Method: Lanczos 3x with premultiplied alpha + unsharp mask; then tools/art_straighten.py.
# Called as: python3 -I tools/make3x.py GAME_DIR/uiscale_hd/todo OUT_DIR f2a1fe95c1442398_256x64 dc229a7b0bb80a22_64x32
# make3x.py TODO_DIR OUT_DIR hash_wxh ... : Lanczos 3x (premultiplied alpha, edge pad) + unsharp r2 100% (method used for the vehicle-ammo shell and MG drum art files)
import sys, numpy as np
from PIL import Image
def blur(x,s):
    r=int(3*s); k=np.exp(-np.arange(-r,r+1)**2/(2*s*s)); k/=k.sum()
    y=np.pad(x,((r,r),(r,r),(0,0)),mode='edge')
    y=sum(k[i]*y[:,i:i+x.shape[1],:] for i in range(2*r+1))
    return sum(k[i]*y[i:i+x.shape[0],:,:] for i in range(2*r+1))
src,out=sys.argv[1],sys.argv[2]
for n in sys.argv[3:]:
    w,h=map(int,n.split('_')[1].split('x'))
    a=np.frombuffer(open(f'{src}/{n}.raw','rb').read(),np.uint8).reshape(h,w,4).astype(np.float32)
    al=a[:,:,3:]/255.0
    pm=np.concatenate([a[:,:,:3]*al,a[:,:,3:]],2); P=3
    pp=np.pad(pm,((P,P),(P,P),(0,0)),mode='edge')
    up=np.stack([np.array(Image.fromarray(pp[:,:,i],'F').resize((pp.shape[1]*3,pp.shape[0]*3),Image.LANCZOS)) for i in range(4)],2)[P*3:-P*3,P*3:-P*3]
    up[:,:,:3]=up[:,:,:3]+1.0*(up[:,:,:3]-blur(up[:,:,:3],2.0))
    A=np.clip(up[:,:,3],0,255)
    rgb=np.where(A[:,:,None]>0.5, up[:,:,:3]/np.maximum(A[:,:,None],1e-3)*255.0,0)
    o=np.concatenate([np.clip(rgb,0,255),A[:,:,None]],2).round().astype(np.uint8)
    assert o.shape==(3*h,3*w,4)
    open(f'{out}/{n}.raw','wb').write(o.tobytes()); print(n,o.nbytes)
