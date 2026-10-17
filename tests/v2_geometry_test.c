#include "pcb_copper.h"
#include "u128.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    uint64_t x=UINT64_C(0xbeef1234);
    for(int i=0;i<50000;i++) {
        x=x*UINT64_C(6364136223846793005)+1;uint64_t a=x;
        x=x*UINT64_C(6364136223846793005)+1;uint64_t b=x;
        U128 r=u128_mul(a,b);
#if defined(__SIZEOF_INT128__)
        __extension__ typedef unsigned __int128 Native;
        Native n=(Native)a*b;assert(r.lo==(uint64_t)n&&r.hi==(uint64_t)(n>>64));
#else
        assert(u128_cmp(r,u128_mul(b,a))==0);
#endif
    }
    PcbBoard board;pcb_board_init(&board,(PcbBoardSpec){40,30,1.6,.035,2});
    PcbPad p={.net_id=1,.x_mm=10,.y_mm=10,.pad_diameter_mm=1,.width_mm=2,.height_mm=2,.rectangular=true,.corner_radius_mm=1};
    assert(pcb_add_pad(&board,p)==0);CopperItem a;assert(pcb_copper_item(&board,COPPER_ITEM_PAD,0,&a)==PCB_COPPER_OK);
    /* Bounding-box corners are empty copper for a round pad: cannot falsely bridge. */
    CopperItem b={.shape=copper_shape_point((Pt){10900000,10900000},10000),.net=2,.layers=1};
    bool touch=true;assert(copper_pair_touch(&a,&b,&touch)==COPPER_RESULT_PASS);assert(!touch);
    b.shape=copper_shape_point((Pt){11100000,10000000},100000);
    assert(copper_pair_check(&a,&b,0)==COPPER_RESULT_SHORT);
    b.shape=copper_shape_point((Pt){11200000,10000000},100000);
    assert(copper_pair_check(&a,&b,100000)==COPPER_RESULT_PASS);
    assert(copper_pair_check(&a,&b,100001)==COPPER_RESULT_TOO_CLOSE);
    pcb_board_free(&board);puts("u128 randomized oracle and rounded-pad exact boundaries passed");return 0;
}
