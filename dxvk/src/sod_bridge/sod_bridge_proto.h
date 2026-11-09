#pragma once
// SoD-RT bridge protocol: shared between the 32-bit DXVK layer (inside shadow.exe) and the
// 64-bit helper sod_rt64.exe, which owns the hardware ray tracing device.
// Fixed-size, pointer-free, identical layout in both bitnesses.
//
// v2 (bridge B2): per-frame GPU link.
//   - one shared timeline semaphore; request k uses value 2k+1 (DXVK signals: "inputs ready") and
//     2k+2 (helper signals: "outputs ready"). DXVK's queue waits for 2k+2, so the helper must
//     serve EVERY request, in order, or the game's queue stalls (the DXVK-side watchdog then
//     signals 2k+2 from the CPU and disables the link).
//   - shared device buffers (NT handles duplicated into the helper): depth in, result out.

#include <stdint.h>

#define SOD_BRIDGE_MAGIC    0x54524453u   /* 'SDRT' */
#define SOD_BRIDGE_VERSION  5u

// object names (suffix: decimal PID of shadow.exe)
#define SOD_BRIDGE_MAPPING  "Local\\SoDRT_Bridge_"
#define SOD_BRIDGE_READY    "Local\\SoDRT_Ready_"
#define SOD_BRIDGE_REQUEST  "Local\\SoDRT_Req_"       // auto-reset: new request(s) posted

#define SOD_BRIDGE_SLOTS    8u

// v3 (bridge B2b): per-request frame geometry in a second shared mapping, one slot per request slot
#define SOD_BRIDGE_GEOMETRY "Local\\SoDRT_Geom_"
#define SOD_GEOM_SLOT_BYTES (1u << 20)
#define SOD_MAX_LIGHTS      8u
#define SOD_MAX_CASTERS     64u
#define SOD_GEOM_MAGIC      0x4D4F4547u   /* 'GEOM' */

enum SodBridgeState : uint32_t {
  SOD_BRIDGE_STARTING = 0,
  SOD_BRIDGE_READY_OK = 1,     // 64-bit RT device created and self-test passed
  SOD_BRIDGE_FAILED   = 2,     // see error[]
};

enum SodLinkState : uint32_t {
  SOD_LINK_NONE     = 0,
  SOD_LINK_ACTIVE   = 1,       // helper imported the current generation and serves requests
  SOD_LINK_FAILED   = 2,       // see linkError[]
};

#pragma pack(push, 8)
struct SodFrameRequest {
  uint64_t value;              // odd timeline value DXVK signals for this request (2k+1)
  uint32_t generation;         // shared objects generation the request refers to
  uint32_t frame;              // game frame number (diagnostics)
  uint32_t width, height;
  uint64_t postedQpc;          // QueryPerformanceCounter when posted (system-wide clock)
  uint32_t hasGeometry;        // geometry slot (same index) holds a SodGeomFrame for this request
  uint32_t reserved[7];
};

// one shadow light pass of the game (one darken quad), taken from its shadow volumes
struct SodGeomLight {
  float    toLight[3];         // unit vector from the surface towards the light (= -volume extrusion)
  float    density;            // darken quad alpha / 255
};

// one shadow-casting character mesh (world space, CPU-skinned by the game)
struct SodGeomCaster {
  uint32_t firstTri, triCount;
  uint32_t lightMask;          // bit l: casts a shadow for light l (the game drew a volume for it)
  uint32_t pad;
};

// per light: the casters' bounds seen along the light. A shadow ray runs parallel to the light, so it can only hit
// a caster if the pixel's (u, v) lies inside these bounds and the pixel is below the casters' top along the light.
struct SodGeomLightBox {
  float    u[3]; float uMin;
  float    v[3]; float uMax;
  float    vMin, vMax, lMax, pad;
};

struct SodGeomFrame {
  uint32_t magic, frame, width, height;
  float    vpX, vpY, vpW, vpH;
  float    invViewProj[16];    // row-major, row vectors: world = (ndc.x, ndc.y, depth, 1) * M, then / w
  uint32_t lightCount, casterCount, triCount, flags;
  SodGeomLight  lights[SOD_MAX_LIGHTS];
  SodGeomCaster casters[SOD_MAX_CASTERS];
  SodGeomLightBox boxes[SOD_MAX_LIGHTS];
  // followed by float tris[triCount][9] (three world-space xyz vertices each)
};
#define SOD_GEOM_MAX_TRIS   ((SOD_GEOM_SLOT_BYTES - sizeof(SodGeomFrame)) / 36u)


struct SodBridgeShared {
  uint32_t magic;
  uint32_t version;
  uint32_t parentPid;

  // ---- written by the 32-bit side --------------------------------------------------
  uint8_t  deviceUUID[16];      // VkPhysicalDeviceIDProperties::deviceUUID of DXVK's GPU
  uint8_t  deviceLUID[8];
  uint32_t deviceLUIDValid;

  // ---- written by the 64-bit helper -------------------------------------------------
  volatile uint32_t state;      // SodBridgeState
  uint32_t helperPid;
  char     deviceName[256];
  uint32_t driverVersion;
  uint32_t apiVersion;
  uint32_t uuidMatched;         // helper found the same physical GPU
  uint32_t hasAccelStruct;      // VK_KHR_acceleration_structure
  uint32_t hasRayQuery;         // VK_KHR_ray_query
  uint32_t hasExternalMemory;   // VK_KHR_external_memory_win32
  uint32_t hasExternalSemaphore;// VK_KHR_external_semaphore_win32
  uint32_t hasTimelineSemaphore;
  uint32_t selfTestHit;         // ray towards the test triangle reported a hit
  uint32_t selfTestMiss;        // ray away from it reported a miss
  float    selfTestMs;          // BLAS+TLAS build + trace, GPU round trip
  char     error[512];

  // ---- B2 frame link: written by the 32-bit side ------------------------------------
  volatile uint32_t linkGeneration;   // bumped after (re)creating the shared buffers (0 = none yet)
  uint32_t linkWidth, linkHeight;
  uint32_t depthFormat;               // VkFormat of the image the depth aspect was copied from
  uint32_t depthBytesPerPixel;        // 4 (D24 in 24 LSBs, or D32 float)
  uint32_t linkAbort;                 // 32-bit side gave up on the link (watchdog), helper stops serving
  uint64_t hSemaphore;                // NT handles, valid in the helper process
  uint64_t hDepthMem, hResultMem;
  uint64_t depthAllocSize, resultAllocSize;
  uint64_t depthBufSize,   resultBufSize;
  uint32_t depthMemTypeBits, resultMemTypeBits;
  volatile uint64_t requestValue;     // highest odd value posted so far (0 = none)
  SodFrameRequest req[SOD_BRIDGE_SLOTS];   // slot = (value >> 1) % SOD_BRIDGE_SLOTS

  // ---- B2 frame link: written by the helper -----------------------------------------
  volatile uint32_t linkState;        // SodLinkState
  volatile uint32_t linkImportedGeneration;
  volatile uint64_t servedValue;      // last even value the helper has submitted a signal for
  uint32_t resultCount[SOD_BRIDGE_SLOTS];   // B2a test: depth pixels != 0 seen by the helper, per slot
  uint64_t resultValue[SOD_BRIDGE_SLOTS];   // request value the count belongs to
  float    latencyAvgUs, latencyMaxUs;      // request posted -> helper GPU work done (helper clock)
  uint32_t latencySamples;
  uint32_t softSamples;                     // soft shadows: rays per light near edges this frame (0 = hard)
  float    gpuUsAvg, gpuUsMax;              // helper GPU time per request (timestamps: BLAS/TLAS build + trace)
  uint32_t lastTris, lastCasters, lastLights;
  float    buildUsAvg;                      // of which: triangle upload + BLAS/TLAS builds
  // v4 profiling (helper monitor thread, wall clock): per request k
  float    reachUsAvg;                      // request posted (CPU) -> game GPU signalled 2k+1
  float    stallUsAvg, stallUsMax;          // 2k+1 -> 2k+2: how long the game's GPU waits for the helper
  float    traceUsAvg;                      // helper GPU time of the part the game waits for (trace)
  float    submitLagUsAvg;                  // request posted -> helper submitted its work (CPU wake-up)
  uint32_t profSamples;
  char     linkError[256];
};
#pragma pack(pop)
