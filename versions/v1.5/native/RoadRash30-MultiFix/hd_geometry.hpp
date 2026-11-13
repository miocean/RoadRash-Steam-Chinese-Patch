// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "hd_raster.hpp"

namespace rrhd {
// Equivalent orientation/UV convention to 0x468d60, operating on projected
// subpixel vertices instead of the legacy two-pixel screen grid. Keep vertex
// identity through every permutation; matching rounded positions is ambiguous.
inline bool normalize(Vertex (&p)[4],int flags,bool& flipX) {
    unsigned positive=0,zero=0;
    for(int i=0;i<4;++i) {
        const auto& a=p[i];const auto& b=p[(i+1)&3];const auto& c=p[(i+3)&3];
        double cross=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
        if(cross>0)positive|=1u<<i;else if(cross==0)zero|=1u<<i;
    }
    if(zero==15)return false;
    for(int i=0;i<3;++i)positive|=((positive>>3)|(positive<<1))&zero;
    bool reverse=false;
    auto midpoint=[&](int to,int a,int b) {
        p[to].x=(p[a].x+p[b].x)*0.5;p[to].y=(p[a].y+p[b].y)*0.5;
    };
    switch(positive) {
        case 0:reverse=true;break;
        case 1:midpoint(0,1,3);reverse=true;break;
        case 2:midpoint(1,2,0);reverse=true;break;
        case 3:std::swap(p[2],p[3]);break;
        case 4:midpoint(2,3,1);reverse=true;break;
        case 5:case 10:return false;
        case 6:std::swap(p[0],p[3]);break;
        case 7:midpoint(3,2,0);break;
        case 8:midpoint(3,2,0);reverse=true;break;
        case 9:std::swap(p[1],p[2]);break;
        case 11:midpoint(2,1,3);break;
        case 12:std::swap(p[0],p[1]);break;
        case 13:midpoint(1,2,0);break;
        case 14:midpoint(0,3,1);break;
        case 15:break;
        default:return false;
    }
    int rotation=0;
    if(flags&4) {
        rotation=reverse?1:0;
        if(reverse)flipX=!flipX;
    } else {
        const bool horizontal=(flags&8)!=0;
        auto axis=[&](int i){return horizontal?p[i].x:p[i].y;};
        int first=axis(0)<axis(1)?0:1,second=1-first;
        for(int i=2;i<4;++i) {
            if(axis(i)<axis(first)){second=first;first=i;}
            else if(axis(i)<axis(second))second=i;
        }
        if(horizontal)rotation=p[second].y>=p[first].y?first:second;
        else {
            constexpr int forward[16]={-1,0,-1,3,0,-1,1,-1,-1,1,-1,2,3,-1,2,-1};
            constexpr int backward[16]={-1,1,-1,0,1,-1,2,-1,-1,2,-1,3,0,-1,3,-1};
            rotation=(reverse?backward:forward)[first+second*4];
        }
    }
    if(reverse) {
        switch(rotation) {
            case 0:std::swap(p[1],p[3]);break;
            case 1:std::swap(p[0],p[1]);std::swap(p[2],p[3]);break;
            case 2:std::swap(p[0],p[2]);break;
            case 3:std::swap(p[0],p[3]);std::swap(p[1],p[2]);break;
        }
    } else if(rotation>0 && rotation<4)std::rotate(p,p+rotation,p+4);
    return true;
}
}
