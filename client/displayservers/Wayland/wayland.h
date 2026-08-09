/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
# include <wayland-egl.h>
# include <EGL/egl.h>
# include <EGL/eglext.h>
# include "eglutil.h"
#endif

#include "app.h"
#include "egl_dynprocs.h"
#include "common/locking.h"
#include "common/countedbuffer.h"
#include "common/ringbuffer.h"
#include "interface/displayserver.h"
#include "interface/desktop.h"

#include "wayland-presentation-time-client-protocol.h"
#include "wayland-viewporter-client-protocol.h"
#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"
#include "wayland-idle-inhibit-unstable-v1-client-protocol.h"
#include "wayland-xdg-output-unstable-v1-client-protocol.h"
#include "wayland-xdg-activation-v1-client-protocol.h"
#include "wayland-fractional-scale-v1-client-protocol.h"
#include "wayland-content-type-v1-client-protocol.h"
#include "wayland-color-management-v1-client-protocol.h"
#include "wayland-xdg-toplevel-icon-v1-client-protocol.h"
#include "wayland-pointer-warp-v1-client-protocol.h"

#include "scale.h"
#include "motion.h"

typedef void (*WaylandPollCallback)(uint32_t events, void * opaque);
typedef void (*WaylandPollCleanup)(void * opaque);

struct WaylandPoll
{
  int                 fd;
  atomic_bool         removed;
  WaylandPollCallback callback;
  WaylandPollCleanup  cleanup;
  void              * opaque;
  struct wl_list      link;
};

struct OutputColorDescription;

struct WaylandOutput
{
  uint32_t name;
  struct WaylandScale scale;
  int32_t scaleInt;
  int32_t logicalWidth;
  int32_t logicalHeight;
  int32_t modeWidth;
  int32_t modeHeight;
  int32_t modeRefresh;
  bool    modeRotate;
  struct wl_output * output;
  struct zxdg_output_v1 * xdgOutput;
  struct wp_color_management_output_v1 * colorOutput;
  struct OutputColorDescription * colorDescription;
  uint32_t referenceWhiteLevel;
  bool referenceWhiteValid;
  uint32_t version;
  struct wl_list link;
};

struct SurfaceOutput
{
  struct wl_output * output;
  struct wl_list link;
};

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

enum WaylandHDRPendingAction
{
  WAYLAND_HDR_PENDING_NONE,
  WAYLAND_HDR_PENDING_APPLY,
  WAYLAND_HDR_PENDING_CLEAR,
};

struct WaylandHDRParameters
{
  bool     pq;
  bool     metadata;
  uint16_t displayPrimary[3][2];
  uint16_t whitePoint[2];
  uint32_t maxDisplayLuminance;
  uint32_t minDisplayLuminance;
  uint32_t maxCLL;
  uint32_t maxFALL;
  uint32_t referenceWhiteLevel;
};

struct WaylandDSState
{
  _Atomic(bool) lockActive;
  bool          keyboardGrabbed;
  bool          pointerInSurface;
  bool          focusedOnSurface;

  WL_DesktopOps * desktop;

  struct wl_display * display;
  struct wl_surface * surface;
  struct wl_registry * registry;
  struct wl_seat * seat;
  struct wl_shm * shm;
  struct wl_compositor * compositor;
  LG_Lock surfaceLock;

  struct WaylandScale scale;
  bool fractionalScale;
  bool needsResize;
  bool configured;
  bool warpSupport;
  struct WlMotion motion;
  double scrollState;

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
  struct wl_egl_window * eglWindow;
  struct SwapWithDamageData swapWithDamage;
#endif

#ifdef ENABLE_OPENGL
  EGLDisplay glDisplay;
  EGLConfig  glConfig;    // primary config (best bit depth probed)
  EGLConfig  glConfigSDR; // fallback 8-bit SDR config
  EGLSurface glSurface;
#endif

  struct wp_presentation * presentation;
  clockid_t                clkId;
  _Atomic(bool)            presentationClockValid;
  LG_Lock                  presentationLock;
  struct wl_list           presentationFrames;
  _Atomic(uint64_t)        nominalPeriod;
  RingBuffer               photonTimings;
  GraphHandle              photonGraph;

  const char             * cursorThemeName;
  int                      cursorSize;
  int                      cursorScale;
  struct wl_cursor_theme * cursorTheme;
  struct wl_buffer       * cursorSquareBuffer;
  struct wl_surface      * cursors[LG_POINTER_COUNT];
  struct Point             cursorHot[LG_POINTER_COUNT];
  LG_DSPointer             cursorId;
  struct wl_surface      * cursor;
  int                      cursorHotX;
  int                      cursorHotY;

  struct wl_data_device_manager * dataDeviceManager;

  uint32_t capabilities;

  struct wl_keyboard * keyboard;
  struct zwp_keyboard_shortcuts_inhibit_manager_v1 * keyboardInhibitManager;
  struct zwp_keyboard_shortcuts_inhibitor_v1 * keyboardInhibitor;
  uint32_t keyboardEnterSerial;
  struct xkb_context * xkb;
  struct xkb_state * xkbState;
  struct xkb_keymap * keymap;

  struct wl_pointer * pointer;
  struct zwp_relative_pointer_manager_v1 * relativePointerManager;
  struct zwp_pointer_constraints_v1 * pointerConstraints;
  struct zwp_relative_pointer_v1 * relativePointer;
  struct zwp_locked_pointer_v1 * lockedPointer;
  struct wp_pointer_warp_v1 * pointerWarpper;
  bool captureRequested;
  bool showPointer;
  uint32_t pointerEnterSerial;

  struct zwp_idle_inhibit_manager_v1 * idleInhibitManager;
  struct zwp_idle_inhibitor_v1 * idleInhibitor;

  struct xdg_activation_v1 * xdgActivation;

  struct wp_viewporter * viewporter;
  struct wp_viewport * viewport;
  struct wp_fractional_scale_manager_v1 * fractionalScaleManager;
  struct wp_fractional_scale_v1 * fractionalScaleInterface;
  struct zxdg_output_manager_v1 * xdgOutputManager;
  struct wl_list outputs; // WaylandOutput::link
  struct wl_list surfaceOutputs; // SurfaceOutput::link
  bool useFractionalScale;

  struct wp_content_type_manager_v1 * contentTypeManager;
  struct wp_content_type_v1 * contentType;

  // Color management (HDR)
  struct wp_color_manager_v1                    * colorManager;
  struct wp_color_management_surface_v1         * colorSurface;
  struct wp_image_description_v1                * hdrImageDesc;
  struct wp_image_description_creator_params_v1 * hdrImageCreator;

  // Active and requested encodings are atomic because getProp runs on the
  // renderer while description events are dispatched by Wayland.
  _Atomic(bool) hdrActive;
  _Atomic(bool) hdrActivePQ;
  _Atomic(bool) hdrRequested;
  _Atomic(bool) hdrRequestedPQ;
  bool    hdrImageDescPQ;
  bool    hdrImageDescReady;
  LG_Lock hdrLock;

  // wp_color_manager_v1 feature advertisement tracking.
  // Set to true after the done event for the color-manager has been received
  // and all compositor capabilities have been recorded.
  _Atomic(bool) cmFeaturesDone;
  bool cmHasParametric;
  bool cmHasLuminances;
  bool cmHasMasteringPrimaries;
  bool cmHasExtendedTargetVolume;
  bool cmHasTFSt2084PQ;
  bool cmHasTFExtLinear;
  bool cmHasPrimariesBT2020;
  bool cmHasPrimariesSRGB;
  bool cmHasWindowsSCRGB;
  bool cmHasPerceptualIntent;
  _Atomic(bool) cmCanDoHDR; // compositor supports a native HDR encoding

  // Local output reference whites used for native overlay composition.
  _Atomic(uint32_t) hdrPQWhiteLevel;
  _Atomic(uint32_t) hdrScRGBWhiteLevel;

  // toplevel icon manager
  struct xdg_toplevel_icon_manager_v1 * iconManager;

  // set by the active desktop backend during shellInit
  void * xdgToplevel;

  // Pending HDR format is published by the frame thread and consumed by the
  // render thread. Keep the action and all metadata in one locked snapshot so
  // a replacement request cannot tear a request already being consumed.
  LG_Lock                      pendingHDRLock;
  enum WaylandHDRPendingAction pendingHDRAction;
  struct WaylandHDRParameters  pendingHDR;

  _Atomic(unsigned) frameEventFlags;
  LGEvent *         frameEvent;

  struct wl_list poll; // WaylandPoll::link
  struct wl_list pollFree; // WaylandPoll::link
  LG_Lock        pollLock;
  LG_Lock        pollFreeLock;
  unsigned int   pollWaiters;
  int            epollFd;
  int            displayFd;
};

struct WCBTransfer
{
  struct CountedBuffer * data;
  const char           ** mimetypes;
};

struct ClipboardRead
{
  int                    fd;
  size_t                 size;
  size_t                 numRead;
  uint8_t              * buf;
  LG_ClipboardRequest    request;
  LG_ClipboardData       type;
};

struct WCBState
{
  struct wl_data_device * dataDevice;
  char                    lgMimetype[64];

  char                 * mimetypes[LG_CLIPBOARD_DATA_NONE];
  struct wl_data_offer * offer;
  struct wl_data_offer * dndOffer;

  LG_Lock                lock;
  struct ClipboardRead * currentRead;
};

extern struct WaylandDSState wlWm;
extern struct WCBState       wlCb;

// activation module
bool waylandActivationInit(void);
void waylandActivationFree(void);
void waylandActivationRequestActivation(void);

// clipboard module
bool waylandCBInit(void);
void waylandCBFree(void);
void waylandCBRequest(LG_ClipboardRequest request, LG_ClipboardData type);
void waylandCBNotice(LG_ClipboardData type);
void waylandCBRelease(void);
void waylandCBInvalidate(void);

// cursor module
bool waylandCursorInit(void);
void waylandCursorFree(void);
void waylandSetPointer(LG_DSPointer pointer);
void waylandCursorScaleChange(void);

// gl module
#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
bool waylandEGLInit(int w, int h);
EGLDisplay waylandGetEGLDisplay(void);
bool waylandEGLSwapBuffers(EGLDisplay display, EGLSurface surface,
    const struct Rect * damage, int count, uint64_t frameToken,
    uint64_t * swapTime, bool * presentTracked);
#endif

#ifdef ENABLE_EGL
EGLNativeWindowType waylandGetEGLNativeWindow(void);
#endif

#ifdef ENABLE_OPENGL
bool waylandOpenGLInit(void);
LG_DSGLContext waylandGLCreateContext(void);
void waylandGLDeleteContext(LG_DSGLContext context);
void waylandGLMakeCurrent(LG_DSGLContext context);
void waylandGLSetSwapInterval(int interval);
void waylandGLSwapBuffers(void);
#endif

// HDR color management (Wayland color-management-v1)
bool waylandColorMgmtInit(void);
void waylandColorMgmtFree(void);
void waylandSetHDRImageDesc(const uint16_t displayPrimary[3][2],
    const uint16_t whitePoint[2], uint32_t maxDisplayLuminance,
    uint32_t minDisplayLuminance, uint32_t maxCLL, uint32_t maxFALL,
    uint32_t referenceWhiteLevel, bool hdrPQ, bool hdrMetadata);
void waylandClearHDRImageDesc(void);

// Queue HDR change from any thread (applied in waylandEGLSwapBuffers).
// Returns true if the request was queued, false if HDR is not available
// (no color manager, missing features, or unsupported TF/primaries).
bool waylandRequestHDR(const uint16_t displayPrimary[3][2],
    const uint16_t whitePoint[2], uint32_t maxDisplayLuminance,
    uint32_t minDisplayLuminance, uint32_t maxCLL, uint32_t maxFALL,
    uint32_t referenceWhiteLevel, bool hdrPQ, bool hdrMetadata);
void waylandRequestClearHDR(void);

// idle module
bool waylandIdleInit(void);
void waylandIdleFree(void);
void waylandInhibitIdle(void);
void waylandUninhibitIdle(void);

// input module
bool waylandInputInit(bool allowNoInput);
void waylandInputFree(void);
void waylandGrabKeyboard(void);
void waylandGrabPointer(void);
void waylandUngrabKeyboard(void);
void waylandUngrabPointer(void);
void waylandCapturePointer(void);
void waylandUncapturePointer(void);
bool waylandIsPointerGrabbed(void);
bool waylandIsPointerCaptured(void);
void waylandRealignPointer(void);
void waylandWarpPointer(int x, int y, bool exiting);
void waylandGuestPointerUpdated(double x, double y, double localX,
    double localY);
bool waylandGetKeyLabel(int key, char * label, size_t size);
// output module
bool waylandOutputInit(void);
void waylandOutputFree(void);
void waylandOutputBind(uint32_t name, uint32_t version);
void waylandOutputTryUnbind(uint32_t name);
struct WaylandScale waylandOutputGetScale(struct wl_output * output);
bool waylandOutputGetFramePeriod(uint64_t * period);
void waylandOutputUpdateFramePeriod(void);
void waylandOutputColorMgmtInit(struct WaylandOutput * output);
void waylandOutputColorMgmtInitAll(void);
void waylandOutputUpdateHDRWhiteLevel(void);

// poll module
bool waylandPollInit(void);
void waylandPollFree(void);
void waylandWait(unsigned int time);
bool waylandPollRegister(int fd, WaylandPollCallback callback, void * opaque, uint32_t events);
bool waylandPollRegisterWithCleanup(int fd, WaylandPollCallback callback,
    void * opaque, WaylandPollCleanup cleanup, uint32_t events);
bool waylandPollUnregister(int fd);

// presentation module
struct WaylandPresentationFrame;
bool waylandPresentationInit(void);
struct WaylandPresentationFrame * waylandPresentationFrame(
    uint64_t frameToken);
void waylandPresentationSwapDone(struct WaylandPresentationFrame * frame,
    bool result, bool * presentTracked);
void waylandPresentationFree(void);
bool waylandGetFramePeriod(uint64_t * period);

// registry module
bool waylandRegistryInit(void);
void waylandRegistryFree(void);

// window module
bool waylandWindowInit(const char * title, const char * appId, bool fullscreen, bool maximize, bool borderless, bool resizable);
void waylandWindowFree(void);
void waylandWindowUpdateScale(void);
void waylandSetWindowSize(int x, int y);
bool waylandIsValidPointerPos(int x, int y);
LG_DSWaitFrameResult waylandWaitFrame(void);
void waylandSkipFrame(void);
void waylandStopWaitFrame(void);
void waylandNeedsResize(void);

// icon module
bool waylandIconInit(void);
