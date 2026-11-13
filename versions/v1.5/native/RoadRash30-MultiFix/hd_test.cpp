// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Executes only isolated drawing routines in a mapped EXE, never its entry
// point, windows, Steam startup, gameplay or saved-game code.
#include "hd_renderer.cpp"
#include <array>
#include <cstdlib>
#include <random>

static void require(bool ok,const char* what) {
    if(!ok) { std::fprintf(stderr,"FAILED: %s\n",what);std::exit(1); }
}
static HDC presentationDC;
static int borderClears;
static int WINAPI countedFill(HDC dc,const RECT* r,HBRUSH brush){++borderClears;return FillRect(dc,r,brush);}
static BOOL WINAPI fixtureRect(HWND,LPRECT r){*r={0,0,clientWidth,clientHeight};return TRUE;}
static HDC WINAPI fixtureDC(HWND){return presentationDC;}
static int WINAPI fixtureRelease(HWND,HDC){return 1;}
static void __fastcall noCaptions(void*,int,int){}
template<class T> static void importFixture(uintptr_t va,T address){require(writeMemory(base+va-0x400000,&address,sizeof address),"presentation fixture import");}

static void parallelTests() {
    viewport(1001,777);
    std::vector<uint8_t> reference(size_t(width)*height,9);world=reference;
    uint8_t changing[64*64];
    for(int n=0;n<300;++n) {
        for(int i=0;i<64*64;++i)changing[i]=((i+n)%7)?uint8_t(n%250+1):0;
        double x=(n*17)%900-20,y=(n*31)%700-20;
        Vertex q[4]={{x,y,0,0},{x+125.75,y+11,64,0},{x+110.5,y+95,64,64},{x-3,y+101,0,64}};
        if(n%2)for(auto& p:q){p.u=16+p.u/4;p.v=9+p.v/4;}
        Texture texture=n%3?Texture{changing,64,64,true}:Texture{};
        rrhd::quad(reference.data(),width,height,q,texture,uint8_t(n%255));
        enqueueQuad(q,texture,uint8_t(n%255),true);
    }
    std::memset(changing,255,sizeof changing);runRasterJobs();
    require(world==reference,"parallel strips preserve exact pixel order and mutable actor textures");
    std::printf("parallel: serial pixel equivalence, transparent overlaps and actor snapshots passed\n");
}
static void rasterTests() {
    constexpr int w=1280,h=960;
    std::vector<uint8_t> buf(size_t(w)*h+64,0xa5);
    uint8_t* pixels=buf.data()+32;
    std::memset(pixels,0,size_t(w)*h);
    Vertex q[4]={{17.25,12.5},{1250.75,20.75},{1200.125,950.25},{40.5,945.75}};
    rrhd::quad(pixels,w,h,q,{},73);
    require(pixels[900*w+600]==73,"raster draws below original row 479");
    require(pixels[500*w+1100]==73,"raster draws beyond original column 639");
    for(int i=0;i<32;++i) require(buf[i]==0xa5 && buf[buf.size()-1-i]==0xa5,"raster buffer boundaries");
    uint8_t tile[4]={0,5,6,7};
    Vertex t[4]={{-50,-50,0,0},{50,-50,2,0},{50,50,2,2},{-50,50,0,2}};
    rrhd::quad(pixels,w,h,t,{tile,2,2,true},0);
    require(pixels[0]==7,"negative-coordinate texture clipping");
    std::printf("raster: high rows, high columns, clipping and buffer boundaries passed\n");
}

struct alignas(4) Record { uint8_t source[104]; uint8_t node[64]; };
static void setQuad(Record& r,const double (&coords)[8],uint8_t* resource,int color) {
    std::memset(&r,0,sizeof r);
    auto n=reinterpret_cast<int32_t*>(r.node);
    auto src=reinterpret_cast<int32_t*>(r.source+0x44);
    for(int i=0;i<8;++i) { src[i]=int32_t(coords[i]*32768);n[6+i]=(src[i]>>15)&~1; }
    *reinterpret_cast<uint8_t**>(r.node+4)=resource;
    n[2]=0;n[3]=0;n[4]=16;n[5]=16;r.node[60]=uint8_t(color);
    at<uint8_t*>(0x6185e0)=r.node;
}

static void distantGeometryTests(uint8_t* resource) {
    viewport(1920,1440);
    Record record{};
    for(int frame=0;frame<32;++frame) {
        std::fill(world.begin(),world.end(),uint8_t(9));
        double top=100.125+frame/64.0,bottom=top+0.75;
        const double coords[8]={20.25,top,500.75,top,500.75,bottom,20.25,bottom};
        setQuad(record,coords,resource,73);drawNode(record.node);runRasterJobs();
        int expected=0;
        for(int y=int(std::ceil(top*3-0.5));y<int(std::ceil(bottom*3-0.5));++y)
            for(int x=100;x<1400;++x) {++expected;require(world[y*width+x]==73,"subpixel distant surface must not collapse on the legacy two-pixel grid");}
        require(expected>0,"thin surface has visible native pixels");
    }
    // Adjacent road tiles share exact subpixel edges during movement.
    // The distant ends are much thinner than one legacy screen pixel.
    for(int frame=0;frame<32;++frame) {
        std::fill(world.begin(),world.end(),uint8_t(9));
        double shift=frame/64.0;
        for(int tile=0;tile<12;++tile) {
            double x=20.25+tile*40.0;
            const double coords[8]={x,100.125+shift,x+40,100.125+shift,
                                    x+40,100.875+shift,x,100.875+shift};
            setQuad(record,coords,resource,73);drawNode(record.node);
        }
        runRasterJobs();
        for(int y=int(std::ceil((100.125+shift)*3-0.5));y<int(std::ceil((100.875+shift)*3-0.5));++y)
            for(int x=70;x<1490;++x)require(world[y*width+x]==73,"moving distant tile joins remain watertight");
    }
    std::printf("distance: 32 subpixel motion steps preserve thin road surfaces\n");
}

static void orientationTests() {
    std::mt19937 random(0x468d60);
    Record record{};at<uint8_t*>(0x6185e0)=record.node;
    for(int n=0;n<12000;++n)for(int flags:{0,2,4,8,12}) {
        int xy[8];Vertex p[4];
        for(int i=0;i<4;++i) {
            xy[i*2]=int(random()%512)*2-512;xy[i*2+1]=int(random()%512)*2-512;
            p[i]={double(xy[i*2]),double(xy[i*2+1])};
        }
        bool flip=bool(n%2);record.node[61]=uint8_t(flip);
        bool oldResult=fn<Normalize>(0x468d60)(xy,flags);
        bool newResult=rrhd::normalize(p,flags,flip);
        require(oldResult==newResult,"subpixel normalizer preserves original polygon acceptance");
        if(oldResult)for(int i=0;i<4;++i)
            require(p[i].x==xy[2*i] && p[i].y==xy[2*i+1],"subpixel normalizer preserves original vertex/UV order");
        require(flip==bool(record.node[61]),"subpixel normalizer preserves original sprite flip");
    }
    at<uint8_t*>(0x6185e0)=nullptr;
    std::printf("orientation: 60000 cases match original engine vertex order and sprite flips\n");
}

int wmain(int argc,wchar_t** argv) {
    ensureCpuWorld();rasterTests();require(argc==2,"EXE fixture path");
    HMODULE module=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
    require(module!=nullptr,"map original executable without executing it");
    base=reinterpret_cast<uintptr_t>(module);
    require(at<uint32_t>(0x4688e7)==base+0x1a58c0,"ASLR relocation of original drawing code");
    // Resolve only the presentation import that our adapter replaces. The
    // isolated world drawing routines have no operating-system calls.
    Stretch api=&StretchDIBits;Fill fill=&countedFill;
    require(writeMemory(base+0x21c15c,&api,sizeof api),"test presentation IAT");
    require(writeMemory(base+0x21c728,&fill,sizeof fill),"test border-clear IAT");
    require(initialize(module),"version-matched native hooks");
    orientationTests();
    renderHeight=0;viewport(1920,1440);startFrame();
    std::vector<uint8_t> low(640*480,9);
    at<int>(0x5aa9cc)=1;at<uint8_t>(0x5ce34c)=1;at<uint8_t>(0x5a292a)=1;
    at<int>(0x5a2934)=640;at<int>(0x5a2938)=480;
    at<uint8_t*>(0x6036bc)=low.data();at<int>(0x6036b8)=640;
    at<int>(0x6036c0)=0;at<int>(0x6036c4)=0;
    at<int>(0x6036c8)=640;at<int>(0x6036cc)=479;at<int>(0x5a29f4)=16;
    uint8_t flatResource[64]{};*reinterpret_cast<int*>(flatResource)=16;
    *reinterpret_cast<int*>(flatResource+12)=16;
    distantGeometryTests(flatResource);startFrame();
    Record record{};
    const double points[8]={10.25,20.75,510.5,50.25,430.125,450.875,25.75,400.125};
    setQuad(record,points,flatResource,73);
    require(fn<DrawList>(0x40132f)()==1,"original world draw with installed hooks");
    require(worldReady && width==1920 && height==1440,"native 1920x1440 world");
    require(primitives==1,"original flat polygon intercepted");
    require(world[1200*width+600]==73,"native scene below legacy raster height");
    size_t changed=0,subpixels=0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        if(world[size_t(y)*width+x]!=low[size_t(mapY[y])*640+mapX[x]]) ++changed;
        if(x%3 && world[size_t(y)*width+x]!=world[size_t(y)*width+x-1]) ++subpixels;
    }
    require(changed>100 && subpixels>100,"native raster differs from frame enlargement");
    // Original sprite/texture path including its orientation and UV setup.
    alignas(4) uint8_t textureResource[80]{};
    auto values=reinterpret_cast<uint32_t*>(textureResource);
    values[0]=0xbc614e;values[3]=2;values[8]=16;values[9]=16;values[10]=1;
    uint8_t texels[256];for(int y=0;y<16;++y)for(int x=0;x<16;++x)texels[y*16+x]=((x+y)&1)?105:0;
    *reinterpret_cast<uint8_t**>(textureResource+56)=texels;
    std::fill(low.begin(),low.end(),uint8_t(9));startFrame();
    const double texturePoints[8]={10.25,20.75,610.5,20.75,610.5,460.875,10.25,460.875};
    setQuad(record,texturePoints,textureResource,0);
    fn<DrawList>(0x40132f)();
    require(worldReady && primitives==1,"original textured polygon intercepted");
    size_t colored=0,transparent=0;
    for(int y=100;y<1300;++y)for(int x=100;x<1700;++x) {
        uint8_t c=world[size_t(y)*width+x];colored+=c==105;transparent+=c==9;
    }
    require(colored>100000 && transparent>100000,"native textured sprite and zero-index transparency");
    // Chinese HUD pixels are kept, while unmodified world pixels remain HD.
    uint8_t palette[1024]{};for(int i=0;i<256;++i)palette[i*4]=palette[i*4+1]=palette[i*4+2]=uint8_t(i);
    low[300*640+300]=244;compose(low.data(),640,palette);
    require(rgba[901*width+901]==0xf4f4f4,"HUD pixel overlay");
    require(displayReady,"high resolution presentation prepared");
    // Exercise the patched GDI call using a memory DC, without showing a window.
    BITMAPINFO bmi{};bmi.bmiHeader.biSize=40;bmi.bmiHeader.biWidth=width;bmi.bmiHeader.biHeight=-height;
    bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;
    void* dibPixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(dc,&bmi,DIB_RGB_COLORS,&dibPixels,nullptr,0);
    HGDIOBJ old=SelectObject(dc,bitmap);
    auto oldBuffer=beforeText;oldBuffer[100*640+100]=0xaabbcc;at<void*>(0x5ce864)=oldBuffer.data();
    int rows=at<Stretch>(0x61c15c)(dc,0,0,width,height,0,0,640,480,oldBuffer.data(),&bmi,DIB_RGB_COLORS,SRCCOPY);
    GdiFlush();require(rows>0,"GDI native-size output");
    require(std::memcmp(dibPixels,rgba.data(),rgba.size()*4)==0,"GDI received native pixels unchanged");
    require(rgba[301*width+301]==0xaabbcc,"anniversary 32-bit caption overlay");
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    // Different widths and heights exercise the same path, including dimensions
    // not divisible by a legacy pixel size. Real polygons occupy both margins.
    const int sizes[][2]={{3440,1440},{1920,1080},{2560,1600},{5120,1440},
                         {1080,1920},{1001,777},{3840,2160},{900,900},{320,240}};
    for(const auto& size:sizes) {
        viewport(size[0],size[1]);startFrame();std::fill(low.begin(),low.end(),uint8_t(9));
        double right=logicalWidth*2.0;
        const double leftQuad[8]={-4,0,40,0,40,480,-4,480};
        setQuad(record,leftQuad,flatResource,61);fn<DrawList>(0x40132f)();
        const double rightQuad[8]={right-40,0,right+4,0,right+4,480,right-40,480};
        setQuad(record,rightQuad,flatResource,83);fn<DrawList>(0x40132f)();
        require(batches==2 && primitives==2,"multiple batches retained in one native frame");
        require(world[(height/2)*width]==61 && world[(height/2)*width+width-1]==83,"real scene fills both edges");
        at<uint8_t*>(0x6185e0)=nullptr;
        auto a=vertex(0,0),vx=vertex(10,0),vy=vertex(0,10);
        require(std::abs(vx.x-a.x-(vy.y-a.y))<1e-9,"uniform projection, no anisotropic stretch");
        require(std::abs(vertex(logicalWidth,240).x-width/2.0)<1e-9,"camera remains centered");
        compose(low.data(),640,palette);
        BITMAPINFO target{};target.bmiHeader.biSize=40;target.bmiHeader.biWidth=width;target.bmiHeader.biHeight=-height;
        target.bmiHeader.biPlanes=1;target.bmiHeader.biBitCount=32;
        HDC memory=CreateCompatibleDC(nullptr);void* output=nullptr;
        HBITMAP dib=CreateDIBSection(memory,&target,DIB_RGB_COLORS,&output,nullptr,0);HGDIOBJ selected=SelectObject(memory,dib);
        oldBuffer=beforeText;at<void*>(0x5ce864)=oldBuffer.data();
        // Even if the old presentation requests a letterbox, the new renderer
        // must overwrite the entire physical viewport, without stretching.
        stretchHook(memory,200,100,640,480,0,0,640,480,oldBuffer.data(),&target,DIB_RGB_COLORS,SRCCOPY);
        GdiFlush();require(std::memcmp(output,rgba.data(),rgba.size()*4)==0,"full-client GDI presentation at arbitrary aspect");
        SelectObject(memory,selected);DeleteObject(dib);DeleteDC(memory);
        std::printf("aspect: %dx%d, camera=%dx240, both scene edges and full-client output passed\n",width,height,logicalWidth);
    }
    // Panorama is sampled from original full-width artwork, with cyclic scroll.
    viewport(3440,1440);horizonWidth=720;horizonRows=320;horizonScroll=123;
    horizon.resize(720*320);
    for(int yy=0;yy<320;++yy)for(int xx=0;xx<720;++xx)horizon[yy*720+xx]=uint8_t(1+xx%250);
    ensureCpuWorld();   // the indexed world raster is created on first use
    paintBackdrop(low.data(),640);
    for(int xx:{0,1720,3439}) {
        int oldX=int(std::floor((xx+0.5-width/2.0)/sceneScale+320));
        require(world[100*width+xx]==uint8_t(1+wrap(oldX-123,720)%250),"full panorama, including new field of view");
    }
    // Finite menu art covers the viewport; all four corner controls stay in the
    // same fitted canvas used by the original mouse-coordinate conversion.
    menuBright.resize(640*480);menuDim.assign(640*480,0x2108);
    for(int yy=0;yy<480;++yy)for(int xx=0;xx<640;++xx)
        menuBright[yy*640+xx]=uint16_t((((xx/20+1)%32)<<10)|(((yy/16+1)%32)<<5)|7);
    std::vector<uint16_t> menu=menuBright;
    const int corners[][2]={{4,4},{635,4},{4,475},{635,475}};
    for(auto& c:corners)menu[c[1]*640+c[0]]=0x7fff;
    for(const auto& size:sizes) {
        viewport(size[0],size[1]);composeMenu(menu.data(),1280,0x7c00,0x3e0,31);
        require(displayReady && rgba.front()!=0 && rgba.back()!=0,"menu artwork covers viewport corners");
        if(width>=640 && height>=480)for(auto& c:corners) {
            auto mx=std::find(mapX.begin(),mapX.end(),c[0]);auto my=std::find(mapY.begin(),mapY.end(),c[1]);
            require(mx!=mapX.end() && my!=mapY.end(),"complete menu controls remain visible");
            require(rgba[size_t(my-mapY.begin())*width+(mx-mapX.begin())]==0xffffff,"menu controls remain intact");
        }
        if(width>=640 && height>=480)for(auto& c:corners) {
            int bx=c[0]<320?20:620,by=c[1]<240?20:460;
            auto mx=std::find(mapX.begin(),mapX.end(),bx);auto my=std::find(mapY.begin(),mapY.end(),by);
            require(rgba[size_t(my-mapY.begin())*width+(mx-mapX.begin())]==color16[menuBright[by*640+bx]],"original background landmarks stay visible without zoom or crop");
        }
        if(uiX[0]>100) {
            unsigned smallest=255,largest=0;
            for(int x=0;x<uiX[0];++x) {
                unsigned red=(rgba[size_t(height/2)*width+x]>>16)&255;
                smallest=std::min(smallest,red);largest=std::max(largest,red);
            }
            require(largest-smallest>20,"side extension retains horizontal artwork detail instead of a flat colour band");
            require(rgba[size_t(height/2)*width+uiX[0]-1]==rgba[size_t(height/2)*width+uiX[0]],"menu extension joins artwork without a brightness seam");
        }
        uint32_t edge=rgba.front();menu[100*640+100]^=0x7fff;
        composeMenu(menu.data(),1280,0x7c00,0x3e0,31);
        require(rgba.front()==edge,"hover changes do not alter side extensions");
        auto choices=resolutionChoices(width,height,0);
        require(choices.front().height==0,"native automatic resolution available");
        for(auto& choice:choices)if(choice.height>0)
            require(std::abs(choice.width-double(width)*choice.height/height)<=0.5,"resolution menu choices preserve current aspect");
    }
    viewport(3440,1440);composeMenu(menuDim.data(),1280,0x7c00,0x3e0,31);
    require(menuBackdropDim && rgba.front()==color16[0x2108],"extension follows dimmed menu artwork");
    composeMenu(menu.data(),1280,0x7c00,0x3e0,31);
    require(!menuBackdropDim,"extension follows restored bright menu artwork");
    parallelTests();
    // Exercise BOTH complete original presentation procedures, including their
    // old black-border FillRect calls. A native Stretch-only test misses these.
    viewport(GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
    BITMAPINFO outputInfo{};outputInfo.bmiHeader.biSize=40;outputInfo.bmiHeader.biWidth=width;outputInfo.bmiHeader.biHeight=-height;
    outputInfo.bmiHeader.biPlanes=1;outputInfo.bmiHeader.biBitCount=32;
    presentationDC=CreateCompatibleDC(nullptr);void* outputBits=nullptr;
    HBITMAP outputBitmap=CreateDIBSection(presentationDC,&outputInfo,DIB_RGB_COLORS,&outputBits,nullptr,0);
    HGDIOBJ outputOld=SelectObject(presentationDC,outputBitmap);
    importFixture(0x61c76c,&fixtureRect);importFixture(0x61c7d4,&fixtureDC);importFixture(0x61c7d0,&fixtureRelease);
    importFixture(0x61c180,&GetStockObject);importFixture(0x61c158,&SetStretchBltMode);
    require(branch(0x40268a,reinterpret_cast<void*>(&noCaptions),0xe9),"isolate original caption game-state access");
    at<HWND>(0x5ce858)=reinterpret_cast<HWND>(1);at<int>(0x5ce85c)=640;at<int>(0x5ce860)=480;
    at<int>(0x5ce894)=uiX[0];at<int>(0x5ce898)=uiY[0];at<int>(0x5ce89c)=uiX[640];at<int>(0x5ce8a0)=uiY[480];
    at<int>(0x5ce8a4)=width;at<int>(0x5ce8a8)=height;
    BITMAPINFO sourceInfo=outputInfo;sourceInfo.bmiHeader.biWidth=640;sourceInfo.bmiHeader.biHeight=-480;
    at<BITMAPINFO>(0x5ce868)=sourceInfo;
    oldBuffer.resize(640*480);at<void*>(0x5ce864)=oldBuffer.data();
    std::fill(world.begin(),world.end(),uint8_t(73));copyLegacyView();borderClears=0;
    fn<Present8>(0x40492b)(low.data(),640,palette);GdiFlush();
    require(borderClears==0,"indexed presentation never clears the former side bars");
    require(std::memcmp(outputBits,rgba.data(),rgba.size()*4)==0,"complete original race output presents only the finished frame");
    fn<Present16>(0x401118)(menu.data(),1280,0x7c00,0x3e0,31);GdiFlush();
    require(borderClears==0,"menu presentation never clears the former side bars");
    require(std::memcmp(outputBits,rgba.data(),rgba.size()*4)==0,"complete original menu output presents the uncropped frame");
    RECT tiny{0,0,1,1};at<Fill>(0x61c728)(presentationDC,&tiny,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    require(borderClears==1,"ordinary control FillRect operations remain enabled");
    require(renderWindowProc(reinterpret_cast<HWND>(1),WM_ERASEBKGND,0,0)==1,"completed frames suppress window background erasure");
    SelectObject(presentationDC,outputOld);DeleteObject(outputBitmap);DeleteDC(presentationDC);
    at<HWND>(0x5ce858)=nullptr;
    std::printf("presentation: real indexed/menu output, former border clears, background erasure and uncropped art passed\n");
    std::printf("integration: original flat/textured code, subpixels, transparency, HUD and GDI passed\n");
    std::printf("native=%dx%d primitives=%llu non-upscale-pixels=%zu subpixel-edges=%zu\n",width,height,primitives,changed,subpixels);
    return 0;
}
