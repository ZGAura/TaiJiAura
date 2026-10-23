cbuffer Frame : register(b0) {
    float2 resolution;
    float time;
    float cpu;
    float memory;
    float gauge;
    float gaugeValue;
    float padding;
    float showOutline;
    float expandedPadding;
    float expandedScale;
    float reserved;
};
Texture2D<float> noiseGrid : register(t0);
static const float fishAngularSpeed=.17;
static const float smokeAngularSpeed=.05;
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float noise(float2 p) {
    int2 i = int2(floor(p)); float2 f = frac(p);
    f = f*f*(3-2*f);
    float a = noiseGrid.Load(int3(i & 127,0));
    float b = noiseGrid.Load(int3((i+int2(1,0)) & 127,0));
    float c = noiseGrid.Load(int3((i+int2(0,1)) & 127,0));
    float d = noiseGrid.Load(int3((i+1) & 127,0));
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
float fbm(float2 p) {
    return noise(p)*.57+noise(p*2.07+float2(19.3,-7.6))*.28
        +noise(p*4.17+float2(-13.4,22.9))*.15;
}
float capsule(float2 p) {
    float core=padding*.5-106.5;
    p.x -= clamp(p.x,-core,core);
    return length(p)-96.;
}
void over(inout float4 dst,float3 rgb,float alpha) {
    alpha=saturate(alpha); dst=float4(rgb*alpha,alpha)+dst*(1-alpha);
}
float2 fishDomain(float2 p,float t) {
    return p+float2(
        fbm(p*2.2+float2(31+t*.16,24-t*.11))-.5,
        fbm(p*2.1+float2(57-t*.12,11+t*.15))-.5)*.39;
}
float yinYangWhite(float2 p) {
    float white=1-smoothstep(-.075,.075,p.x);
    float top=length(p-float2(0,-.43)),bottom=length(p-float2(0,.43));
    white+=(1-white)*(1-smoothstep(.39,.49,top));
    return white*smoothstep(.39,.49,bottom);
}
float4 collapsedSmoke(float2 position) {
    float2 p=position/resolution*float2(14,100);
    // Grow the 64-pixel leaf by ten pixels around its unchanged midpoint.
    // Every smoke layer is multiplied by body, whose vertical support is (13,87).
    [branch] if(p.y<=13 || p.y>=87) return 0;
    float along=saturate((p.y-13)/74);
    float fullness=pow(max(0,sin(along*3.14159265)),1.05);
    // Bend the two halves around the midpoint; both tips and the midpoint stay on the vertical axis.
    float bend=sin(along*6.2831853)*(.9+.035*sin(time*.35));
    float center=7+bend;
    // Widen the original 8.7-pixel body by two pixels, preserving its centerline.
    p.x=center+(p.x-center)*(8.7/10.7);
    float radius=4.35*fullness;
    float body=(1-smoothstep(-.3,.45,abs(p.x-center)-radius))
        *smoothstep(13,13.65,p.y)*(1-smoothstep(86.35,87,p.y));
    // The leaf's exact zero-alpha exterior needs neither noise nor colored strands.
    [branch] if(body<=0) return 0;
    float2 flow=float2(p.x*.55,(p.y+time*4.8)*.15);
    float cloud=fbm(flow+float2(27,41));
    float curl=fbm(flow*1.4+float2(61-time*.12,17))-.5;
    float width=(2+cloud*.9)*(.12+.88*fullness);
    float stream=exp(-pow(max(0,abs(p.x-center)-.5*fullness)/width,2));
    float filament=pow(.5+.5*sin((p.x-center)*3.3+curl*7-p.y*.21+time*.85),3);
    float usage=saturate(gaugeValue);
    float hasFill=gaugeValue>=0 && usage>0 ? 1 : 0;
    float level=87-usage*74;
    float seam=(fbm(float2(p.x*1.6+time*.22,p.y*.5-time*.8)+float2(13,73))-.5)*1.2
        +sin(p.x*2.3+time*1.4)*.22;
    float fill=smoothstep(-.7,.7,p.y-level+seam)*hasFill;
    // Four shared usage bands: cyan, green, amber, then red from 80 percent.
    float3 tint=lerp(float3(0,.85,1),float3(.10,.95,.30),smoothstep(.24,.25,usage));
    tint=lerp(tint,float3(1,.70,.04),smoothstep(.49,.50,usage));
    tint=lerp(tint,float3(1,.12,.16),smoothstep(.79,.80,usage));
    float4 result=0;
    // White wisps keep the unused height visible without changing the colored usage level.
    float whiteAlpha=lerp(.32+cloud*.34+filament*.14,.07+cloud*.12,fill);
    float3 idleColor=lerp(float3(.98,.99,1),float3(.32,.37,.40),fill);
    over(result,idleColor,stream*body*whiteAlpha);
    float density=stream*body*fill*(.40+cloud*.34+filament*.20);
    over(result,lerp(tint,float3(.85,.97,1),filament*.08),density);
    // Let a lighter, curling tail dissipate above the leading edge of the rising smoke.
    float age=max(0,level-p.y);
    float tailCenter=center+sin(p.y*.26-time*.7)*(.3+age*.035);
    float tail=exp(-pow((p.x-tailCenter)/(.65+age*.035),2));
    float tailFade=smoothstep(0,1,age)*(1-smoothstep(3,10,age));
    over(result,tint,tail*tailFade*body*(.07+cloud*.08)*(1-fill)*hasFill);
    // A few slender colored wisps curl into the white area and dissolve independently.
    [unroll] for(int i=0;i<3;++i) {
        float reach=5+3*sin(time*.63+i*2.1);
        float strandCenter=center+(i-1)*1.1+sin(p.y*.45-time*.9+i*2)*(.35+age*.02)+curl*.3;
        float strand=exp(-pow((p.x-strandCenter)/(.24+age*.008),2));
        float fade=smoothstep(.2,1.3,age)*(1-smoothstep(reach,reach+2,age))*exp(-age*.14);
        over(result,tint,strand*fade*body*.38*(1-fill)*hasFill);
    }
    float head=exp(-pow((p.y-level)/2.7,2))*stream*body;
    over(result,lerp(tint,float3(.88,.98,1),.20),head*.16*hasFill);
    result*=smoothstep(0,1,min(p.x,14-p.x))*smoothstep(0,3,p.y)*(1-smoothstep(97,100,p.y));
    return float4(min(result.rgb,result.aaa),result.a);
}
float4 expandedSmoke(float4 position) {
    float canvas=padding, innerWidth=canvas-21;
    float2 p=position.xy/resolution*float2(canvas,240);
    [branch] if(expandedPadding>0) {
        float2 contentSize=float2(canvas/6,40)*expandedScale;
        float2 origin=(resolution-contentSize)*.5;
        p=(position.xy-origin)/contentSize*float2(canvas,240);
    }
    float2 local=p-float2(3,24), uv=local/float2(innerWidth,192);
    float2 centered=p-float2(canvas*.5-7.5,120);
    float dist=capsule(centered);
    float aa=max(expandedPadding>0 ? 6/expandedScale : canvas/resolution.x,.65);
    // Both animated rim layers have exact finite support inside distance 50.
    [branch] if(expandedPadding>0 && showOutline<=0 && dist>=52) return 0;
    // Flow evolves at the selected frame rate; hardware metrics update independently.
    float t=time;
    // A periodic flow domain follows the entire capsule, including both rounded ends.
    float core=canvas*.5-106.5,rad=96,perimeter=core*4+6.2831853*rad,s;
    if(centered.x>core) s=core*2+rad*(atan2(centered.y,centered.x-core)+1.5707963);
    else if(centered.x<-core) {
        float a=atan2(centered.y,centered.x+core); if(a<1.5707963)a+=6.2831853;
        s=core*4+3.14159265*rad+rad*(a-1.5707963);
    } else s=centered.y<0?centered.x+core:core*2+3.14159265*rad+core-centered.x;
    float u=s/perimeter*6.2831853, flowU=u-t*.30;
    float2 n=float2(cos(flowU),sin(flowU))*4.5+float2(71,23);
    float rimCloud=fbm(n+float2(dist*.065,-dist*.075));
    float rimCurl=fbm(n*1.37+float2(11+t*.08,dist*.14))-.5;
    float edgeOffset=(rimCloud-.5)*13+rimCurl*11+sin(u*5-t*.65)*2;
    // The optional glass shell restores the original edge clip, bevel and inner reflection.
    float inside=showOutline>0 ? 1-smoothstep(-aa,aa,dist)
        : 1-smoothstep(-18,8,dist-edgeOffset);
    float4 result=0;
    [branch] if(showOutline>0) {
        float shadow=exp(-pow(capsule(centered-float2(0,6))/8,2))*.10;
        over(result,float3(.035,.09,.14),shadow);
    }
    over(result,float3(.70,.88,.96),inside*(.055+.045*uv.y));

    // Interior flow: same domain warp, liquid level and curls as exact/source.fragment.html.
    float sx=uv.x*5.4+sin(uv.y*5.7-t*.72)*(.24+cpu*.004)+t*.25;
    float sy=uv.y*4+sin(uv.x*7+t*.52)*.26-t*.24;
    float warp=fbm(float2(sx+91,sy+16))-.5;
    float curl=fbm(float2(sx*.91+22+warp*.8,sy*1.17+54-t*.10))-.5;
    float cloud=fbm(float2(sx,sy)+float2(warp,curl)*2.69);
    float fiber=pow(.5+.5*sin(cloud*32+curl*6-uv.y*6-t*1.15),4);
    float billow=warp*.2235+sin(uv.x*10.5+t*.84)*.04;
    float surface=.82-memory*.0046;
    float volume=.045+.955*smoothstep(surface-.21,surface+.14,uv.y+billow);
    float density=.77*volume*(.085+smoothstep(.20,.68,cloud)*.40+fiber*.31);
    // Feather the quiet reading area so the flow remains continuous behind the text.
    float readingArea=smoothstep(.20,.34,uv.x)
        *smoothstep(.10,.24,uv.y)*(1-smoothstep(.60,.78,uv.y));
    density*=1-.25*readingArea;
    float light=smoothstep(.24,.77,cloud+curl*.25);
    over(result,lerp(float3(34,124,151),float3(204,240,255),light)/255+fiber*9/255,density*inside);

    // Soft vapor fish: no opaque disk or hard outline.
    float2 q=(p-float2(96,120))/48;
    float radius=length(q), angle=-t*fishAngularSpeed;
    // The fish's original smoothstep makes its contribution exactly zero at radius >= 1.
    [branch] if(radius<1) {
        float2 rotated=float2(q.x*cos(angle)-q.y*sin(angle),q.x*sin(angle)+q.y*cos(angle));
        float2 warped=fishDomain(rotated,t);
        float white=yinYangWhite(warped);
        float2 eyeQ=warped-float2(0,warped.y<0?-.43:.43);
        float vr=length(eyeQ),va=atan2(eyeQ.y,eyeQ.x);
        // Disturb the smoke independently of the unchanged yin/yang split.
        float fishCurl=fbm(rotated*2.8+float2(71+t*.38,18-t*.32))-.5;
        float vortex=va+2.2/(.28+vr)+fishCurl*2.4-t*(warped.y<0?.82:.74);
        float2 smokeP=float2(cos(vortex),sin(vortex))*vr*4.2;
        float vapor=fbm(smokeP+float2(fishCurl*1.8+25+t*.34,42-t*.38));
        float fishCloud=smoothstep(.24,.72,vapor);
        float thread=pow(.5+.5*sin(vapor*23+vr*9+va*.8-t*1.55),3);
        float eye=exp(-vr*vr/.010);
        white=lerp(white,1-white,eye);
        // Keep the fish prominent while exposing moving billows and curled filaments.
        float vaporAlpha=(1-smoothstep(.76,1.,radius))*(.58+fishCloud*.27+thread*.22);
        float3 fishColor=lerp(float3(2,10,23),float3(250,254,255)*(.68+.32*fishCloud),white)/255;
        fishColor+=thread*lerp(float3(20,43,58),float3(20,22,24),white)/255;
        over(result,fishColor,vaporAlpha*inside);
    }

    // Trace the smoke back to its emitting edge rather than recoloring an unrelated halo.
    [branch] if(radius>.80 && radius<1.64) {
        float polar=atan2(q.y,q.x);
        float travelDistance=max(radius-.88,0),age=travelDistance/.72;
        float travelTime=travelDistance/.22;
        float flowAngle=polar-t*smokeAngularSpeed;
        float2 flowDir=float2(cos(flowAngle),sin(flowAngle));
        float smokeCurl=fbm(flowDir*3.6+float2(radius*2.8,17-t*.21))-.5;
        // The angular domain is periodic. There is no half-turn reset or radial cut in a tail.
        // Emitted smoke rotates more slowly than the fish, so older wisps lag behind its leading edge.
        float sourceAngle=polar+angle+(fishAngularSpeed-smokeAngularSpeed)*travelTime
            +smokeCurl*.16*smoothstep(0,.35,age);
        float2 sourceDir=float2(cos(sourceAngle),sin(sourceAngle));
        float sourceTime=t-travelTime;
        float sourceWhite=yinYangWhite(fishDomain(sourceDir*.88,sourceTime));
        float emission=fbm(sourceDir*3.3+float2(33+sourceTime*.18,67-sourceTime*.22));
        float detail=fbm(sourceDir*5.2+float2(91,12-sourceTime*.25)+age*float2(1.7,-1.2));
        float strands=pow(.5+.5*sin(emission*28+smokeCurl*8-age*10),3);
        float clouds=smoothstep(.18,.70,emission+smokeCurl*.35);
        float fade=smoothstep(.80,.97,radius)*(1-smoothstep(1.04,1.62,radius));
        float opacity=fade*clouds*(.22+detail*.28+strands*.30);
        float shade=lerp(.016+detail*.035,.84+detail*.16,sourceWhite);
        over(result,shade.xxx,opacity*inside);
    }

    // Eight trigrams, three individual solid/broken strokes each.
    static const uint codes[8]={7,3,2,1,0,4,5,6};
    float trigram=0;
    [unroll] for(int k=0;k<8;k++) {
        float a=k*3.14159265/4;
        float2 mark=float2(q.x*cos(a)+q.y*sin(a),-q.x*sin(a)+q.y*cos(a));
        mark.y+=1.28;
        [unroll] for(int j=0;j<3;j++) {
            float dy=abs(mark.y-(j-1)*.086)-.013;
            float dx=abs(mark.x)-.125;
            float stroke=1-smoothstep(0,aa/48,max(dx,dy));
            if((codes[k] & (1u<<j))==0) stroke*=smoothstep(.0275,.0275+aa/48,abs(mark.x));
            trigram=max(trigram,stroke);
        }
    }
    over(result,float3(.91,.98,1),trigram*.96*inside);

    [branch] if(showOutline>0) {
        float bevel=exp(-abs(dist+1)*1.1)*(.10+.26*saturate(-centered.y/96));
        over(result,float3(.88,.97,1),bevel*inside);
        float innerRim=exp(-abs(dist+5)*1.4)*.055;
        over(result,float3(.70,.88,.96),innerRim*inside);
    }
    // Keep the travelling reflection in both styles.
    float shineX=3+innerWidth*(.4+.23*sin(time*.55));
    float shine=pow(saturate(1-abs(p.x-shineX+(uv.y-.5)*70)/10),3)*.08;
    over(result,float3(1,1,1),shine*inside);

    // Broad, broken billows and drifting filaments form a vapor loop, never a constant-width line.
    float edgeDistance=dist-edgeOffset;
    float band=exp(-pow(edgeDistance/(9+rimCloud*13),2));
    float filament=pow(.5+.5*sin(dist*.38+rimCurl*22+flowU*5-t*.85),3);
    float pockets=smoothstep(.22,.67,rimCloud+rimCurl*.35);
    float rimLight=smoothstep(.26,.73,fbm(n*.71+float2(45-t*.09,54+t*.11)));
    float rimAlpha=band*(1-smoothstep(18,32,abs(edgeDistance)))
        *pockets*(.15+rimCloud*.27+filament*.32);
    over(result,lerp(float3(.15,.39,.49),float3(.86,.96,1),rimLight)+filament*.025,rimAlpha);
    float driftOffset=edgeOffset-10+sin(flowU*3+rimCurl*4)*8;
    float driftBand=exp(-pow((dist-driftOffset)/(14+rimCloud*8),2));
    float driftAlpha=driftBand*smoothstep(.30,.73,rimCloud-rimCurl*.25)
        *(1-smoothstep(20,38,abs(dist-driftOffset)))*.30;
    over(result,lerp(float3(.21,.48,.59),float3(.72,.88,.94),rimLight),driftAlpha);
    // Dissipate before reaching the rectangular HWND bounds.
    float border=expandedPadding*6;
    float2 margin=min(p+border,float2(canvas+border,240+border)-p);
    result*=smoothstep(0,8,min(margin.x,margin.y));
    return float4(min(result.rgb,result.aaa),result.a);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float4 expanded=0,collapsed=0;
    [branch] if(gauge<1) expanded=expandedSmoke(position);
    [branch] if(gauge>0) collapsed=collapsedSmoke(position.xy);
    return lerp(expanded,collapsed,gauge);
}




