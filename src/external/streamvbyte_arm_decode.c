

#include "streamvbyte_isadetection.h"
#ifdef STREAMVBYTE_ARM
#include "streamvbyte_shuffle_tables_decode.h"
#ifdef __aarch64__
typedef uint8x16_t decode_t;
#else
typedef uint8x8x2_t decode_t;
#endif
static inline decode_t  _decode_neon(const uint8_t key,
					const uint8_t * restrict *dataPtrPtr) {

  uint8_t len;
  uint8_t *pshuf = (uint8_t *)&shuffleTable[key];
  uint8x16_t decodingShuffle = vld1q_u8(pshuf);

  uint8x16_t compressed = vld1q_u8(*dataPtrPtr);
#ifdef AVOIDLENGTHLOOKUP
  // this avoids the dependency on lengthTable,
  // see https://github.com/lemire/streamvbyte/issues/12
  len = pshuf[12 + (key >> 6)] + 1;
#else
  len = lengthTable[key];
#endif
#ifdef __aarch64__
  uint8x16_t data = vqtbl1q_u8(compressed, decodingShuffle);
#else
  uint8x8x2_t codehalves = {{vget_low_u8(compressed), vget_high_u8(compressed)}};

  uint8x8x2_t data = {{vtbl2_u8(codehalves, vget_low_u8(decodingShuffle)),
		       vtbl2_u8(codehalves, vget_high_u8(decodingShuffle))}};
#endif
  *dataPtrPtr += len;
  return data;
}

static void streamvbyte_decode_quad( const uint8_t * restrict *dataPtrPtr, uint8_t key, uint32_t * restrict out ) {
  decode_t data =_decode_neon( key, dataPtrPtr );
#ifdef __aarch64__
  vst1q_u8((uint8_t *) out, data);
#else
  vst1_u8((uint8_t *) out, data.val[0]);
  vst1_u8((uint8_t *) (out + 2), data.val[1]);
#endif
}

static const uint8_t *svb_decode_vector(uint32_t *out, const uint8_t *keyPtr, const uint8_t *dataPtr, uint32_t count) {
  for(uint32_t i = 0; i < count/4; i++)
    streamvbyte_decode_quad( &dataPtr, keyPtr[i], out + 4*i );

  return dataPtr;
}
#endif
