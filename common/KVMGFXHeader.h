
#define KVMGFX_HEADER_MAGIC "[[KVMGFXHeader]]"

typedef enum FrameType
{
  FRAME_TYPE_INVALID   ,
  FRAME_TYPE_ARGB      , // ABGR interleaved: A,R,G,B 32bpp
  FRAME_TYPE_RGB       , // RGB interleaved :   R,G,B 24bpp
  FRAME_TYPE_XOR       , // xor of the previous frame: R, G, B
  FRAME_TYPE_YUV444P   , // YUV444 planar
  FRAME_TYPE_YUV420P   , // YUV420 12bpp
  FRAME_TYPE_ARGB10    , // rgb 10 bit packed, a2 b10 r10
  FRAME_TYPE_MAX       , // sentinel value
} FrameType;

typedef enum FrameComp
{
  FRAME_COMP_NONE      , // no compression
  FRAME_COMP_BLACK_RLE , // basic run length encoding of black pixels for XOR mode
  FRAME_COMP_MAX       , // sentinel valule
} FrameComp;

struct KVMGFXHeader
{
  char      magic[sizeof(KVMGFX_HEADER_MAGIC)];
  uint32_t  version;     // version of this structure
  uint16_t  hostID;      // the host ivshmem client id
  uint16_t  guestID;     // the guest ivshmem client id
  FrameType frameType;   // the frame type
  FrameComp compType;    // frame compression mode
  uint32_t  width;       // the width
  uint32_t  height;      // the height
  uint32_t  stride;      // the row stride
  int32_t   mouseX;      // the initial mouse X position
  int32_t   mouseY;      // the initial mouse Y position
  uint64_t  dataLen;     // total lengh of the data after this header
};

#pragma pack(push,1)
struct RLEHeader
{
  uint8_t  magic[3];
  uint16_t length;
};
#pragma pack(pop)