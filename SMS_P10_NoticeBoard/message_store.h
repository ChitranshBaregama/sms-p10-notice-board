#ifndef MESSAGE_STORE_H
#define MESSAGE_STORE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Two records: commit marker, length (LE16), sequence (LE32), CRC (LE16),
 * payload. Commit the inactive slot last so an interrupted write keeps the
 * previous message. CRC covers length, sequence and payload. Legacy records
 * are deliberately ignored. Needs 616 bytes of EEPROM for 299-byte messages. */
namespace MessageStore {
static const int capacity = 299;
static const int header = 9;
static const int stride = header + capacity;
static const uint8_t marker = 0xc7;
inline uint16_t crcByte(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (uint8_t i = 0; i < 8; ++i)
        crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1);
    return crc;
}
template<class E> bool valid(E &ee, int slot, uint16_t &len, uint32_t &seq) {
    int b = slot * stride;
    if (ee.read(b) != marker) return false;
    len = (uint16_t)(ee.read(b+1) | (uint16_t)ee.read(b+2) << 8);
    if (!len || len > capacity) return false;
    seq = 0;
    uint16_t crc = 0xffffu;
    for (int i = 1; i <= 6; ++i) crc = crcByte(crc, ee.read(b+i));
    for (uint8_t i = 0; i < 4; ++i) seq |= (uint32_t)ee.read(b+3+i) << (8*i);
    for (uint16_t i = 0; i < len; ++i) {
        uint8_t c = ee.read(b+header+i);
        if (!c) return false;
        crc = crcByte(crc,c);
    }
    return crc == (uint16_t)(ee.read(b+7) | (uint16_t)ee.read(b+8) << 8);
}
template<class E> int newest(E &ee, uint16_t &len, uint32_t &seq) {
    uint16_t n0=0,n1=0; uint32_t s0=0,s1=0;
    bool v0=valid(ee,0,n0,s0), v1=valid(ee,1,n1,s1);
    if (!v0 && !v1) return -1;
    bool use1 = v1 && (!v0 || (uint32_t)(s1-s0) < 0x80000000UL);
    len=use1?n1:n0; seq=use1?s1:s0;
    return use1?1:0;
}
template<class E> bool load(E &ee, char *out, size_t size) {
    uint16_t len=0; uint32_t seq=0;
    int slot=newest(ee,len,seq);
    if (slot < 0 || !out || size <= len) return false;
    for (uint16_t i=0;i<len;++i) out[i]=(char)ee.read(slot*stride+header+i);
    out[len]='\0'; return true;
}
template<class E> void save(E &ee, const char *msg, void (*service)()) {
    uint16_t oldlen=0; uint32_t seq=0;
    int current=newest(ee,oldlen,seq), slot=current==0?1:0;
    size_t size=strlen(msg);
    if (!size || size > capacity) return;
    uint16_t len=(uint16_t)size;
    seq=current < 0?0:seq+1;
    int b=slot*stride;
    ee.update(b,0); // invalidate before touching any record bytes
    uint8_t meta[6]={(uint8_t)len,(uint8_t)(len>>8),(uint8_t)seq,
        (uint8_t)(seq>>8),(uint8_t)(seq>>16),(uint8_t)(seq>>24)};
    uint16_t crc=0xffffu;
    for (uint8_t i=0;i<6;++i) { ee.update(b+1+i,meta[i]); crc=crcByte(crc,meta[i]); }
    for (uint16_t i=0;i<len;++i) {
        uint8_t c=(uint8_t)msg[i]; ee.update(b+header+i,c); crc=crcByte(crc,c);
        if (service) service();
    }
    ee.update(b+7,(uint8_t)crc); ee.update(b+8,(uint8_t)(crc>>8));
    ee.update(b,marker); // sole commit point
}
}
#endif
