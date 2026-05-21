#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Minimal grayscale JFIF encoder
// Included as a separate header so the Arduino IDE preprocessor
// sees the struct definitions before it hoists any function
// prototypes from the .ino file.
// ============================================================

// Standard luma DC Huffman table (JFIF Annex K)
static const uint8_t STD_LUMA_DC_BITS[16] = {
  0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0
};
static const uint8_t STD_LUMA_DC_VALS[] = {
  0,1,2,3,4,5,6,7,8,9,10,11
};
// Standard luma AC Huffman table (JFIF Annex K)
static const uint8_t STD_LUMA_AC_BITS[16] = {
  0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125
};
static const uint8_t STD_LUMA_AC_VALS[] = {
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,
  0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
  0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,
  0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,
  0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,
  0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
  0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,
  0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
  0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
  0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
  0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
  0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
  0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
  0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
  0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,
  0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,
  0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,
  0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
  0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,
  0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
  0xF9,0xFA
};

// Flat quantisation table (Q=2, ~quality 85)
static const uint8_t FLAT_Q[64] = {
  2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
  2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
  2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
  2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2
};

struct HuffCode {
  uint32_t code;
  int      len;
};

struct BitWriter {
  uint8_t* buf;
  size_t   cap;
  size_t   pos;
  uint32_t bits;
  int      nBits;

  void init(uint8_t* b, size_t c) { buf=b; cap=c; pos=0; bits=0; nBits=0; }

  void writeByte(uint8_t v) {
    if (pos < cap) buf[pos++] = v;
  }
  void writeWord(uint16_t v) { writeByte(v>>8); writeByte(v&0xFF); }

  void writeBit(int b) {
    bits = (bits<<1)|(b&1);
    if (++nBits == 8) flush8();
  }
  void writeBits(uint32_t v, int n) {
    for (int i=n-1; i>=0; i--) writeBit((v>>i)&1);
  }
  void flush8() {
    uint8_t b = (uint8_t)(bits & 0xFF);
    writeByte(b);
    if (b == 0xFF) writeByte(0x00); // byte stuffing
    nBits = 0; bits = 0;
  }
  void flushFinal() {
    if (nBits > 0) {
      bits = (bits << (8-nBits)) | ((1u<<(8-nBits))-1);
      nBits = 8;
      flush8();
    }
  }
};

static inline void buildHuffCodes(const uint8_t* bits, const uint8_t* vals,
                                   int nVals, HuffCode* out) {
  uint32_t code = 0;
  int vi = 0;
  for (int len = 1; len <= 16; len++) {
    for (int i = 0; i < bits[len-1]; i++) {
      out[vals[vi]].code = code;
      out[vals[vi]].len  = len;
      vi++; code++;
    }
    code <<= 1;
  }
}

static inline int encodeBlock(BitWriter& bw, int Y, int prevDC,
                               HuffCode* dcCodes, HuffCode* acCodes) {
  int dc   = (Y + 1) / 2;
  int diff = dc - prevDC;
  int absd = diff < 0 ? -diff : diff;
  int cat  = 0;
  for (int t = absd; t; t >>= 1) cat++;

  bw.writeBits(dcCodes[cat].code, dcCodes[cat].len);
  if (cat > 0) {
    int val = diff < 0 ? diff + (1<<cat) - 1 : diff;
    bw.writeBits((uint32_t)val, cat);
  }
  // EOB
  bw.writeBits(acCodes[0x00].code, acCodes[0x00].len);
  return dc;
}

// Generate a W x H grayscale JPEG with banded luma. W,H must be multiples of 8.
// Returns malloc'd buffer; caller must free(). Sets *outLen.
static inline uint8_t* makeGrayscaleJpeg(int W, int H, size_t* outLen) {
  int mcuCols = W / 8;
  int mcuRows = H / 8;
  size_t bufCap = 700 + (size_t)(mcuCols * mcuRows) * 6;
  uint8_t* buf = (uint8_t*)malloc(bufCap);
  if (!buf) return nullptr;

  HuffCode dcCodes[12]  = {};
  HuffCode acCodes[256] = {};
  buildHuffCodes(STD_LUMA_DC_BITS, STD_LUMA_DC_VALS, 12,  dcCodes);
  buildHuffCodes(STD_LUMA_AC_BITS, STD_LUMA_AC_VALS, 162, acCodes);

  BitWriter bw;
  bw.init(buf, bufCap);

  // SOI
  bw.writeByte(0xFF); bw.writeByte(0xD8);
  // APP0
  bw.writeByte(0xFF); bw.writeByte(0xE0); bw.writeByte(0x00); bw.writeByte(0x10);
  bw.writeByte('J'); bw.writeByte('F'); bw.writeByte('I'); bw.writeByte('F'); bw.writeByte(0x00);
  bw.writeByte(0x01); bw.writeByte(0x01); // v1.1
  bw.writeByte(0x00);                     // no units
  bw.writeByte(0x00); bw.writeByte(0x01); // Xdensity=1
  bw.writeByte(0x00); bw.writeByte(0x01); // Ydensity=1
  bw.writeByte(0x00); bw.writeByte(0x00); // no thumbnail
  // DQT
  bw.writeByte(0xFF); bw.writeByte(0xDB); bw.writeByte(0x00); bw.writeByte(0x43);
  bw.writeByte(0x00);
  for (int i=0;i<64;i++) bw.writeByte(FLAT_Q[i]);
  // SOF0 grayscale
  bw.writeByte(0xFF); bw.writeByte(0xC0); bw.writeByte(0x00); bw.writeByte(0x0B);
  bw.writeByte(0x08);
  bw.writeByte((uint8_t)(H>>8)); bw.writeByte((uint8_t)(H&0xFF));
  bw.writeByte((uint8_t)(W>>8)); bw.writeByte((uint8_t)(W&0xFF));
  bw.writeByte(0x01); bw.writeByte(0x01); bw.writeByte(0x11); bw.writeByte(0x00);
  // DHT luma DC
  { int n=12;
    bw.writeByte(0xFF); bw.writeByte(0xC4);
    bw.writeWord((uint16_t)(2+1+16+n));
    bw.writeByte(0x00);
    for(int i=0;i<16;i++) bw.writeByte(STD_LUMA_DC_BITS[i]);
    for(int i=0;i<n;i++)  bw.writeByte(STD_LUMA_DC_VALS[i]); }
  // DHT luma AC
  { int n=(int)sizeof(STD_LUMA_AC_VALS);
    bw.writeByte(0xFF); bw.writeByte(0xC4);
    bw.writeWord((uint16_t)(2+1+16+n));
    bw.writeByte(0x10);
    for(int i=0;i<16;i++) bw.writeByte(STD_LUMA_AC_BITS[i]);
    for(int i=0;i<n;i++)  bw.writeByte(STD_LUMA_AC_VALS[i]); }
  // SOS
  bw.writeByte(0xFF); bw.writeByte(0xDA); bw.writeByte(0x00); bw.writeByte(0x08);
  bw.writeByte(0x01); bw.writeByte(0x01); bw.writeByte(0x00);
  bw.writeByte(0x00); bw.writeByte(0x3F); bw.writeByte(0x00);
  // Entropy-coded data
  static const int LUMA_BANDS[16] = {
    200,180,160,140,120,100,80,60,
    220,240,30,50,70,90,110,130
  };
  int prevDC = 0;
  for (int row=0; row<mcuRows; row++) {
    int Y = LUMA_BANDS[((row*16)/mcuRows) & 15];
    for (int col=0; col<mcuCols; col++)
      prevDC = encodeBlock(bw, Y, prevDC, dcCodes, acCodes);
  }
  bw.flushFinal();
  // EOI
  bw.writeByte(0xFF); bw.writeByte(0xD9);

  *outLen = bw.pos;
  return buf;
}
