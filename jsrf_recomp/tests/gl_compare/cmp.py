import struct,sys
from PIL import Image
def load(f):
    d=open(f,'rb').read(); px=[]
    im=Image.new('RGB',(640,480))
    P=im.load()
    for y in range(480):
        for x in range(640):
            v=struct.unpack_from('<H',d,(y*640+x)*2)[0]
            P[x,y]=(((v>>11)&31)<<3,((v>>5)&63)<<2,(v&31)<<3)
    return im
a=load('sw.raw'); b=load('gl.raw')
a.save('sw.png'); b.save('gl.png')
A=a.load(); B=b.load(); bad=0; big=0
diff=Image.new('RGB',(640,480)); D=diff.load()
for y in range(480):
    for x in range(640):
        d=max(abs(A[x,y][i]-B[x,y][i]) for i in range(3))
        if d>8: bad+=1
        if d>40: big+=1; D[x,y]=(255,0,0)
diff.save('diff.png')
print('pixels differing >8:',bad,' >40:',big)
