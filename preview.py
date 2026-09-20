"""Software layout illustration; sample time/spectrum, not a device capture."""
import math
import re
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
block = source.split("const uint8_t digits[10][7] = {", 1)[1].split(";", 1)[0]
digits = [list(map(int, row.split(","))) for row in re.findall(r"\{([\d,]+)\}", block)]
ORANGE, WHITE, DIM = "#ff2910", "#bdbdab", "#202420"


def base():
    im = Image.new("RGB", (240, 240))
    d = ImageDraw.Draw(im)
    for y in range(8, 235, 6):
        for x in range(8, 235, 6):
            d.point((x, y), fill="#101410")
    return im, d


def dot(d, x, y, r, color):
    d.ellipse((x-r, y-r, x+r, y+r), fill=color)


def number(d, x, y, value, color):
    for n, char in enumerate(f"{value:02}"):
        for row in range(7):
            for col in range(5):
                dot(d, x+n*36+col*6, y+row*6, 2,
                    color if digits[int(char)][row] & (1 << (4-col)) else DIM)


clock, d = base()
d.text((18, 14), "DOT / MIC", fill=ORANGE)
d.text((192,14),"CLOCK",fill=DIM)
for y, val, color, label in [(49,22,WHITE,"H"),(103,27,WHITE,"M"),(157,44,ORANGE,"S")]:
    number(d,28,y,val,color)
    d.text((16,y+11),label,fill=DIM)
for x in range(126,222,6):
    for y in range(49,198,6):
        dot(d,x,y,2,ORANGE if (y-49)//6 < 44*25//60+1 else "#400000")
d.text((18,209),"SAT 09.19",fill=WHITE)
d.text((168,209),"USB READY",fill=DIM)
d.text((18,227),"HOLD AI TO TALK",fill=DIM)
d.text((198,227),"G4 L",fill=DIM)

OUT=ROOT/"docs/preview"; OUT.mkdir(parents=True,exist_ok=True)
pages=[]
levels=[0.0]*24
frames=[]
def blend(color, amount):
    base=tuple(bytes.fromhex(DIM[1:])); color=tuple(bytes.fromhex(color[1:]))
    return tuple(round(a+(b-a)*amount) for a,b in zip(base,color))

# Illustrative moving test spectrum, not a microphone recording.
for frame in range(100):
    phase=frame*2*math.pi/100
    values=[max(0, (5+3*math.sin(phase))*math.exp(-((i-5-2*math.sin(phase))/4)**2)
                 +(5+2*math.cos(phase))*math.exp(-((i-15)/5)**2)) for i in range(24)]
    if frame>70: values=[0]*24
    targets=[sum(w*values[max(0,min(23,i+j))] for j,w in zip(range(-2,3),(1,2,3,2,1)))/9 for i in range(24)]
    levels=[v+(t-v)*(1-math.exp(-25/(60 if t>v else 180))) for v,t in zip(levels,targets)]
    voice,d=base()
    d.text((18,14),"DOT / MIC",fill=ORANGE)
    d.text((156,14),"LISTENING",fill=ORANGE)
    dot(d,220,19,2,ORANGE)
    for i in range(32):
        pos=i*23/31; left=int(pos); t=pos-left; t=t*t*(3-2*t)
        h=levels[left]+(levels[min(left+1,23)]-levels[left])*t
        for j in range(-12,13):
            brightness=max(0,min(1,h-abs(j)+1))
            if j==0: brightness=max(.25,min(h,1))
            dot(d,28+i*6,121+j*6,2,blend(ORANGE,brightness))
    d.text((18,209),"LOW > MID > HIGH",fill=WHITE)
    d.text((174,209),"USB LIVE",fill=ORANGE)
    d.text((18,227),"RELEASE AI TO MUTE",fill=DIM)
    d.text((198,227),"G4 L",fill=DIM)
    if frame==25:
        pages[:]=[("01-clock","01 / CLOCK",clock),("02-spectrum","02 / LISTENING",voice.copy())]
    out=Image.new("RGB",(520,286),"#161818")
    out.paste(clock,(12,12)); out.paste(voice,(268,12))
    ImageDraw.Draw(out).text((12,265),"SOFTWARE PREVIEW / SAMPLE DATA / NOT HARDWARE VERIFIED",fill=WHITE)
    frames.append(out.resize((1040,572),Image.Resampling.NEAREST))
for name,_,image in pages:
    image.save(OUT/f"{name}.png")

from PIL import ImageFont
TITLE=ImageFont.load_default(size=26)
LABEL=ImageFont.load_default(size=16)
SMALL=ImageFont.load_default(size=14)
MUTED="#6c6c62"

overview=Image.new("RGB",(1020,700),"#161818")
od=ImageDraw.Draw(overview)
od.text((20,18),"DOTMIC / UI LAYOUT",fill=WHITE,font=TITLE)
od.text((20,50),"Sample data, not a device capture. Each screen is 240 x 240.",fill=MUTED,font=SMALL)
for index,(_,title,image) in enumerate(pages):
    x=20+index*500; y=84
    od.text((x,y),title,fill=ORANGE,font=LABEL)
    od.rectangle((x-1,y+25,x+480,y+506),outline=MUTED)
    overview.paste(image.resize((480,480),Image.Resampling.NEAREST),(x,y+26))
od.text((20,660),"HOLD AI TALK   |   UP/DOWN GAIN   |   SELECT DIAGNOSTICS   |   START WI-FI   |   APP SLEEP",
        fill=MUTED,font=SMALL)
overview.save(OUT/"overview.png")

# GIF stores delay in 10 ms units; alternating 20/30 preserves 40 FPS on average.
frames[0].save(OUT/"spectrum.gif",save_all=True,append_images=frames[1:],
               duration=[20,30]*50,loop=0)
print(f"UI layout: {len(pages)} native screens + overview + spectrum.gif saved to {OUT}")
