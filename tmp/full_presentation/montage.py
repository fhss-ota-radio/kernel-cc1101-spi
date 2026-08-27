from PIL import Image, ImageOps, ImageDraw
from pathlib import Path
p=Path(r"C:\Users\kccistc\Desktop\kernel-cc1101-spi\tmp\full_presentation\preview")
files=sorted(p.glob("slide-*.png")); thumb=(480,270); gap=18; cols=3; rows=(len(files)+cols-1)//cols
canvas=Image.new("RGB",(cols*thumb[0]+(cols+1)*gap,rows*(thumb[1]+28)+(rows+1)*gap),"#D8D2CB")
d=ImageDraw.Draw(canvas)
for i,f in enumerate(files):
    im=Image.open(f).convert("RGB"); im.thumbnail(thumb)
    x=gap+(i%cols)*(thumb[0]+gap); y=gap+(i//cols)*(thumb[1]+28+gap)
    canvas.paste(im,(x,y)); d.text((x,y+thumb[1]+5),f.stem,fill="#171412")
canvas.save(p/"qa-montage.jpg",quality=92)
