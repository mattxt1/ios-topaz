#!/usr/bin/env python3
"""Exercise the actual SweetSurface trapRelease body with reference-count mocks.
Usage: test-surface-zombie.py /path/to/patched/SweetSurface.cpp
"""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
start = source.index('IOReturn\nIOSurfaceRootUserClient::trapRelease(')
end = source.index('\nstatic IOExternalTrap sweet_surface_traps', start)
body = source[start:end]
prelude = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include <cstdio>
using IOReturn = int;
constexpr int kIOReturnSuccess=0, kIOReturnNotFound=1;
static uint64_t pixelBytes=0;
struct State { uint32_t seed=42; };
struct Surface {
 int refs=1, users=1;
 const uint64_t bytes=11239424;
 std::shared_ptr<State> state=std::make_shared<State>();
 Surface(){pixelBytes+=bytes;}
 void retain(){++refs;}
 void release(){if(--refs==0){pixelBytes-=bytes;delete this;}}
};
void sweet_surface_put(Surface*s){if(--s->users==0)s->release();}
struct Ref {
 int refs=1;Surface*surface;bool released=false,detached=false;
 int*data=new int(1);uint32_t count=1;
 std::shared_ptr<State> state;
 Ref(Surface*s):surface(s),state(s->state){s->retain();}
 void retain(){++refs;}
 void release(){if(--refs==0){delete data;if(surface){if(!released)sweet_surface_put(surface);surface->release();}delete this;}}
};
using SweetSurfaceRef=Ref;
template<class T>void drop(T*&p){if(p){p->release();p=nullptr;}}
void drop(int*&p){delete p;p=nullptr;}
#define OSSafeReleaseNULL(p) drop(p)
struct Array {
 std::vector<Ref*> v;
 bool setObject(Ref*r){r->retain();v.push_back(r);return true;}
 unsigned getCount(){return v.size();}
 void removeObject(unsigned n){auto*r=v.at(n);v.erase(v.begin()+n);r->release();}
 void flushCollection(){while(!v.empty())removeObject(0);}
 ~Array(){flushCollection();}
};
void IOLockLock(void*){}void IOLockUnlock(void*){}
struct IOSurfaceRootUserClient {
 void*fLock=nullptr;Array refs,zombies;Array*fRefs=&refs,*fZombies=&zombies;
 Ref*findRef(uint32_t,unsigned*i,bool detached=false){for(unsigned n=0;n<refs.v.size();n++)if(refs.v[n]->detached==detached){if(i)*i=n;return refs.v[n];}return nullptr;}
 void add(Surface*s){auto*r=new Ref(s);refs.setObject(r);r->release();}
 IOReturn trapRelease(void*,void*,void*,void*,void*,void*);
};
'''
tests = r'''
int main(){
 {
 IOSurfaceRootUserClient c;
 for(int i=0;i<300;i++){
  auto*s=new Surface;c.add(s);
  assert(c.trapRelease((void*)1,nullptr,nullptr,nullptr,nullptr,nullptr)==0);
  assert(pixelBytes==0); // retired 11MB pixels must not remain in zombie cache
  assert(c.zombies.v.back()->surface==nullptr);
  assert(c.zombies.v.back()->state->seed==42); // state survives independently
  assert(c.zombies.getCount()<=256);
 }
 assert(c.trapRelease((void*)1,nullptr,nullptr,nullptr,nullptr,nullptr)==kIOReturnNotFound);
 }
 {
 IOSurfaceRootUserClient a,b;auto*s=new Surface;s->users=2;a.add(s);b.add(s);
 a.trapRelease((void*)1,nullptr,nullptr,nullptr,nullptr,nullptr);
 assert(pixelBytes==11239424); // another live client still owns the pixels
 assert(a.zombies.v.back()->state->seed==42);
 b.trapRelease((void*)1,nullptr,nullptr,nullptr,nullptr,nullptr);
 assert(pixelBytes==0);
 assert(a.zombies.v.back()->state->seed==42);
 assert(b.zombies.v.back()->state->seed==42);
 }
 assert(pixelBytes==0);puts("PASS: retired pixels freed, state survives, live shared pixels preserved, cache bounded");
}
'''
with tempfile.TemporaryDirectory() as d:
    cpp = Path(d)/'test.cpp'
    cpp.write_text(prelude+body+tests)
    out = Path(d)/'test'
    subprocess.run(['xcrun','clang++','-std=c++17','-fsanitize=address,undefined','-g',str(cpp),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True)
