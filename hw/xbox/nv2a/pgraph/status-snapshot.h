/* Single-register hardware status publication, independent of renderer work. */
#ifndef NV2A_PGRAPH_STATUS_SNAPSHOT_H
#define NV2A_PGRAPH_STATUS_SNAPSHOT_H
#include <stdint.h>
static inline uint32_t nv2a_status_read(const uint32_t *value)
{ return __atomic_load_n(value, __ATOMIC_ACQUIRE); }
static inline void nv2a_status_store(uint32_t *value, uint32_t next)
{ __atomic_store_n(value, next, __ATOMIC_RELEASE); }
static inline void nv2a_status_raise(uint32_t *value, uint32_t bits)
{ __atomic_fetch_or(value, bits, __ATOMIC_RELEASE); }
static inline uint32_t nv2a_status_ack(uint32_t *value, uint32_t bits)
{ return __atomic_fetch_and(value, ~bits, __ATOMIC_ACQ_REL) & ~bits; }
#endif
