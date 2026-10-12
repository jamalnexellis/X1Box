/* IRQ acknowledgment must not erase other concurrently published status. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/status-snapshot.h"

enum { iterations = 100000 };
static uint32_t status, payload;
static void *publish(void *unused)
{
    (void)unused;
    for (uint32_t i=1;i<=iterations;i++) {
        while (nv2a_status_read(&status)&1) { }
        payload=i;
        nv2a_status_raise(&status,1);
    }
    return NULL;
}
static void *other_irq(void *unused)
{
    (void)unused;
    for (unsigned i=0;i<iterations;i++) { nv2a_status_raise(&status,2); }
    return NULL;
}
int main(void)
{
    for(uint32_t pending=0;pending<256;pending++)for(uint32_t mask=0;mask<256;mask++) {
        nv2a_status_store(&status,pending);
        assert(nv2a_status_read(&status)==pending);
        assert(nv2a_status_ack(&status,mask)==(pending&~mask));
        nv2a_status_raise(&status,mask);
        assert(nv2a_status_read(&status)==((pending&~mask)|mask));
    }
    nv2a_status_store(&status,0);
    pthread_t producer,irq; assert(!pthread_create(&producer,NULL,publish,NULL));
    assert(!pthread_create(&irq,NULL,other_irq,NULL));
    for(uint32_t i=1;i<=iterations;i++) {
        while (!(nv2a_status_read(&status)&1)) { }
        assert(payload==i);
        nv2a_status_ack(&status,1);
    }
    assert(!pthread_join(producer,NULL)); assert(!pthread_join(irq,NULL));
    assert(nv2a_status_read(&status)==2);
    puts("PASS 65,536 IRQ mask cases; 100,000 ordered payload publications and concurrent independent IRQ preservation");
    return 0;
}
