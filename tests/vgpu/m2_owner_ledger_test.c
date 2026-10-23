/** @file m2_owner_ledger_test.c @brief Exact-owner controls independent of native totals. */
#include "m2_owner_ledger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static void check(int value, unsigned line) {
    ++checks;
    if (!value) { fprintf(stderr, "M2 ledger check failed line=%u\n", line); exit(1); }
}
/** @brief Non-elidable fixture assertion, including optimized builds. */
#define Check(value) check(!!(value), __LINE__)
static m2_owner_snapshot_t snapshot(m2_owner_ledger_t *ledger) {
    m2_owner_snapshot_t result = {0};
    Check(m2_owner_snapshot(ledger, M2OwnerVersion, &result, sizeof result));
    return result;
}
static uint64_t acquire(m2_owner_ledger_t *ledger, uint32_t kind, uintptr_t parent, uintptr_t value) {
    const uint64_t id=m2_owner_reserve(ledger,kind,parent);
    Check(id);
    Check(m2_owner_acquired(ledger,id,value));
    return id;
}
static void normal(void) {
    m2_owner_ledger_t ledger={.next_id=1};
    m2_owner_snapshot_t view=snapshot(&ledger);
    Check(m2_owner_clean(&view));
    for (unsigned cycle=0;cycle<128;++cycle) {
        const uint64_t pending=m2_owner_reserve(&ledger,M2OwnerDc,11);
        view=snapshot(&ledger);Check(view.pending==1 && !m2_owner_clean(&view));
        Check(m2_owner_acquired(&ledger,pending,0));
        const uint64_t window=acquire(&ledger,M2OwnerWindow,0,11);
        const uint64_t first=acquire(&ledger,M2OwnerDc,11,101);
        const uint64_t unrelated=acquire(&ledger,M2OwnerDc,12,102);
        Check(m2_owner_lookup(&ledger,M2OwnerDc,11,101)==first);
        Check(m2_owner_retired(&ledger,unrelated,1));
        view=snapshot(&ledger);
        Check(view.live==2 && view.acquired-view.retired==2 && !m2_owner_clean(&view));
        Check(view.first_live_id==window && !view.first_live_parent && view.first_live_value==11);
        Check(m2_owner_retired(&ledger,first,1));
        Check(m2_owner_retired(&ledger,window,1));
        view=snapshot(&ledger);Check(m2_owner_clean(&view));
        Check(view.next_id==1+4*(uint64_t)(cycle+1));
    }
    Check(view.acquired==384 && view.retired==384);
    puts("M2_OWNER_LEDGER case=identity-and-unrelated-retirement PASS");
}
static void failures(void) {
    for (unsigned mode=0;mode<8;++mode) {
        m2_owner_ledger_t ledger={.next_id=1};
        const uint64_t id=acquire(&ledger,M2OwnerDc,11,101);
        if (mode==0) Check(!m2_owner_retired(&ledger,id,0));
        if (mode==1) Check(!m2_owner_retired(&ledger,id+1,1));
        if (mode==2) Check(!m2_owner_retired(&ledger,0,1));
        if (mode==3) Check(!m2_owner_lookup(&ledger,M2OwnerDc,12,101));
        if (mode==4) Check(!m2_owner_lookup(&ledger,M2OwnerWindow,11,101));
        if (mode==5) Check(!m2_owner_lookup(&ledger,M2OwnerDc,11,0));
        if (mode==6) Check(!m2_owner_acquired(&ledger,id,102));
        if (mode==7) Check(!m2_owner_acquired(&ledger,id+1,103));
        m2_owner_snapshot_t view=snapshot(&ledger);
        Check(view.live==1 && view.first_live_id==id && view.first_live_parent==11 && view.first_live_value==101);
        Check(view.failures && !m2_owner_clean(&view));
        Check(!m2_owner_reserve(&ledger,M2OwnerDc,12));
        Check(m2_owner_retired(&ledger,id,1));
        view=snapshot(&ledger);Check(!view.live && view.failures && !m2_owner_clean(&view));
    }
    m2_owner_ledger_t duplicate={.next_id=1};
    const uint64_t first=acquire(&duplicate,M2OwnerDc,11,101);
    const uint64_t second=m2_owner_reserve(&duplicate,M2OwnerDc,11);
    Check(second && !m2_owner_acquired(&duplicate,second,101));
    Check(!m2_owner_lookup(&duplicate,M2OwnerDc,11,101));
    m2_owner_snapshot_t view=snapshot(&duplicate);
    Check(view.live==2 && view.acquired==2 && (view.failures&M2OwnerDuplicate));
    Check(m2_owner_retired(&duplicate,first,1));
    Check(m2_owner_retired(&duplicate,second,1));
    Check(!m2_owner_retired(&duplicate,first,1));
    m2_owner_ledger_t pending={.next_id=1};
    const uint64_t id=m2_owner_reserve(&pending,M2OwnerWindow,0);
    Check(!m2_owner_retired(&pending,id,1));
    Check(m2_owner_acquired(&pending,id,0));
    puts("M2_OWNER_LEDGER case=failed-foreign-duplicate-retirement PASS");
}
static void bounds(void) {
    m2_owner_ledger_t ledger={.next_id=1};
    uint64_t ids[M2OwnerCapacity];
    for(unsigned i=0;i<M2OwnerCapacity;++i) ids[i]=acquire(&ledger,M2OwnerDc,11,100+i);
    Check(!m2_owner_reserve(&ledger,M2OwnerDc,11));
    m2_owner_snapshot_t view=snapshot(&ledger);Check(view.live==M2OwnerCapacity && (view.failures&M2OwnerLimit));
    for(unsigned i=0;i<M2OwnerCapacity;++i) Check(m2_owner_retired(&ledger,ids[i],1));
    for(unsigned mode=0;mode<6;++mode) {
        ledger=(m2_owner_ledger_t){.next_id=1};
        if(mode==0)ledger.next_id=0;
        if(mode==1)ledger.next_id=UINT64_MAX;
        if(mode==2)ledger.acquired=UINT64_MAX;
        const uint32_t kind=mode==3 ? 999 : mode==4 ? M2OwnerWindow : M2OwnerDc;
        Check(!m2_owner_reserve(&ledger,kind,mode==5 ? 0 : 11));
        view=snapshot(&ledger);Check(view.failures && !m2_owner_clean(&view));
    }
    ledger=(m2_owner_ledger_t){.next_id=UINT64_MAX-1};
    const uint64_t last=acquire(&ledger,M2OwnerDc,11,101);
    Check(last==UINT64_MAX-1 && !m2_owner_reserve(&ledger,M2OwnerDc,11));
    Check(m2_owner_retired(&ledger,last,1));
    puts("M2_OWNER_LEDGER case=capacity-and-id-exhaustion PASS");
}
static void malformed(void) {
    m2_owner_ledger_t ledger={.next_id=1};
    m2_owner_snapshot_t view;memset(&view,0xa5,sizeof view);
    const m2_owner_snapshot_t before=view;
    Check(!m2_owner_reserve(NULL,M2OwnerDc,11));
    Check(!m2_owner_acquired(NULL,1,101));
    Check(!m2_owner_lookup(NULL,M2OwnerDc,11,101));
    Check(!m2_owner_retired(NULL,1,1));
    Check(!m2_owner_snapshot(NULL,M2OwnerVersion,&view,sizeof view));
    Check(!m2_owner_snapshot(&ledger,M2OwnerVersion,NULL,sizeof view));
    Check(!m2_owner_snapshot(&ledger,M2OwnerVersion+1,&view,sizeof view));
    Check(!m2_owner_snapshot(&ledger,M2OwnerVersion,&view,sizeof view-1));
    Check(!memcmp(&view,&before,sizeof view));
    Check(!m2_owner_clean(NULL));
    for(unsigned mode=0;mode<13;++mode) {
        ledger=(m2_owner_ledger_t){.next_id=2};
        if(mode<3) {
            if(mode==0)ledger.entries[0].value=1;
            if(mode==1)ledger.entries[0].parent=1;
            if(mode==2)ledger.entries[0].kind=M2OwnerDc;
        } else if(mode<9) {
            ledger.entries[0]=(m2_owner_entry_t){.id=1,.kind=M2OwnerDc,.parent=11,.value=101};
            ledger.acquired=1;
            if(mode==3)ledger.entries[0].id=2;
            if(mode==4)ledger.entries[0].kind=999;
            if(mode==5)ledger.entries[0].parent=0;
            if(mode==6)ledger.entries[0].kind=M2OwnerWindow;
            if(mode==7)ledger.acquired=0;
            if(mode==8)ledger.retired=2;
        } else {
            if(mode==9)ledger.next_id=0;
            if(mode==10)ledger.acquired=1;
            if(mode==11)ledger.retired=1;
            if(mode==12){ledger.next_id=1;ledger.acquired=ledger.retired=1;}
        }
        view=snapshot(&ledger);Check(view.failures&M2OwnerInconsistent);Check(!m2_owner_clean(&view));
    }
    ledger=(m2_owner_ledger_t){.next_id=1};
    for(unsigned mode=0;mode<12;++mode) {
        view=snapshot(&ledger);
        if(mode==0)view.version++;
        if(mode==1)view.bytes--;
        if(mode==2)view.next_id=0;
        if(mode==3)view.live=1;
        if(mode==4)view.pending=1;
        if(mode==5)view.failures=M2OwnerInvalid;
        if(mode==6)view.first_live_id=1;
        if(mode==7)view.first_live_parent=11;
        if(mode==8)view.first_live_value=101;
        if(mode==9)view.reserved=1;
        if(mode==10)view.acquired=1;
        if(mode==11)view.acquired=view.retired=1;
        Check(!m2_owner_clean(&view));
    }
    puts("M2_OWNER_LEDGER case=malformed-evidence-and-abi PASS");
}
/** @brief Run portable ledger controls, no OS acquisitions or persistent owners.
 * @return Zero all non-elidable checks pass, one otherwise; sole test thread.
 */
int main(void) {
    normal();failures();bounds();malformed();
    printf("M2_OWNER_LEDGER 4 cases %u checks PASS\n",checks);
    return 0;
}
