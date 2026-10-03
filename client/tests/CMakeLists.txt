cmake_minimum_required(VERSION 3.10)

if(LG_CLIENT_PLATFORM_WINDOWS)
  link_libraries(
    $<TARGET_OBJECTS:client_test_version>
    lg_egl_provider
  )
endif()

add_executable(common-platform-tests
  common_platform_test.c
)
target_link_libraries(common-platform-tests
  lg_common
)
target_compile_definitions(common-platform-tests PRIVATE
  LG_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME common-platform-tests
  COMMAND common-platform-tests
)
set_tests_properties(common-platform-tests PROPERTIES
  TIMEOUT 10
)

add_executable(event-source-tests
  event_source_test.c
  ../src/event_source.c
)
target_include_directories(event-source-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(event-source-tests
  lg_common
)
add_test(NAME event-source-tests
  COMMAND event-source-tests
)
set_tests_properties(event-source-tests PROPERTIES
  TIMEOUT 10
)

if(LG_CLIENT_PLATFORM_LINUX)
  add_executable(config-platform-tests
    config_platform_test.c
    ../platform/Linux/config.c
  )
  target_include_directories(config-platform-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  )
  target_link_libraries(config-platform-tests
    lg_common
  )
  add_test(NAME config-platform-tests
    COMMAND config-platform-tests
  )
  set_tests_properties(config-platform-tests PROPERTIES
    TIMEOUT 10
  )

  add_executable(lgmp-platform-tests
    lgmp_platform_test.c
  )
  target_link_libraries(lgmp-platform-tests
    lg_common
  )
  add_test(NAME lgmp-platform-tests
    COMMAND lgmp-platform-tests
  )
  set_tests_properties(lgmp-platform-tests PROPERTIES
    TIMEOUT 10
  )

  add_executable(keycode-tests
    keycode_test.c
  )
  add_test(NAME keycode-tests
    COMMAND keycode-tests
  )
  set_tests_properties(keycode-tests PROPERTIES
    TIMEOUT 10
  )

  add_executable(process-tests
    process_test.c
    ../platform/Linux/process.c
  )
  add_test(NAME process-tests
    COMMAND process-tests
  )
  set_tests_properties(process-tests PROPERTIES
    TIMEOUT 10
  )
endif()

add_executable(wasapi-format-tests
  wasapi_format_test.c
  ../audiodevs/WASAPI/format.c
)
target_include_directories(wasapi-format-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../audiodevs/WASAPI"
)
add_test(NAME wasapi-format-tests
  COMMAND wasapi-format-tests
)
set_tests_properties(wasapi-format-tests PROPERTIES
  TIMEOUT 10
)

add_executable(wasapi-capture-queue-tests
  wasapi_capture_queue_test.c
  ../audiodevs/WASAPI/capture_queue.c
)
target_include_directories(wasapi-capture-queue-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../audiodevs/WASAPI"
)
add_test(NAME wasapi-capture-queue-tests
  COMMAND wasapi-capture-queue-tests
)
set_tests_properties(wasapi-capture-queue-tests PROPERTIES
  TIMEOUT 10
)

if(LG_CLIENT_PLATFORM_WINDOWS)
  add_executable(common-windows-tests
    common_windows_test.c
  )
  target_link_libraries(common-windows-tests
    lg_common
  )
  add_test(NAME common-windows-tests
    COMMAND common-windows-tests
  )
endif()

add_executable(font-tests
  font_test.c
  font_platform_stub.c
  ../src/font.c
)

add_executable(sw-surface-tests
  sw_surface_test.c
  ../src/sw_surface.c
)
target_include_directories(sw-surface-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sw-surface-tests
  lg_common
)
add_test(NAME sw-surface-tests COMMAND sw-surface-tests)
set_tests_properties(sw-surface-tests PROPERTIES TIMEOUT 10)
target_compile_definitions(font-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
  FONT_TEST_FILE="${PROJECT_TOP}/repos/gui/cimgui/imgui/misc/fonts/DroidSans.ttf"
)
target_include_directories(font-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(font-tests
  lg_common
  lg_gui
)
add_test(NAME font-tests
  COMMAND font-tests
)
set_tests_properties(font-tests PROPERTIES
  TIMEOUT 10
)

if(LG_CLIENT_PLATFORM_LINUX)
  add_executable(lgmp-transport-tests
    lgmp_transport_test.c
    ../src/kb_hid.c
  )
  target_link_libraries(lgmp-transport-tests
    transport_LGMP
    lg_common
    lgmp
  )
  if(ENABLE_AUDIO)
    target_compile_definitions(lgmp-transport-tests PRIVATE
      ENABLE_AUDIO
    )
  endif()
  add_test(NAME lgmp-transport-tests
    COMMAND lgmp-transport-tests
  )
  set_tests_properties(lgmp-transport-tests PROPERTIES
    TIMEOUT 10
  )

  add_executable(lgmp-frame-tests
    lgmp_frame_test.c
    ../src/kb_hid.c
  )
  target_link_libraries(lgmp-frame-tests
    transport_LGMP
    lg_common
    lgmp
  )
  set(LGMP_FRAME_CASES
    malformed
    serial-damage
    lanes
    leases
    dma
  )
  foreach(name IN LISTS LGMP_FRAME_CASES)
    add_test(NAME lgmp-frame-${name}
      COMMAND lgmp-frame-tests ${name}
    )
    set_tests_properties(lgmp-frame-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  add_executable(lgmp-input-tests
    lgmp_input_test.c
    ../transports/LGMP/input.c
    ../src/kb_hid.c
  )
  target_include_directories(lgmp-input-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  )
  target_link_libraries(lgmp-input-tests
    lg_common
    lgmp
  )
  set(LGMP_INPUT_CASES
    claim
    blocked
    restart
    pressure
    idle
  )
  foreach(name IN LISTS LGMP_INPUT_CASES)
    add_test(NAME lgmp-input-${name}
      COMMAND lgmp-input-tests ${name}
    )
    set_tests_properties(lgmp-input-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  add_executable(lgmp-clipboard-tests
    lgmp_clipboard_test.c
    ../transports/LGMP/clipboard.c
  )
  target_include_directories(lgmp-clipboard-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../transports/LGMP"
  )
  target_link_libraries(lgmp-clipboard-tests
    lg_common
    lgmp
  )
  set(LGMP_CLIPBOARD_CASES
    claim
    offer-request
    inbound-stream
    outbound-stream
    blocked
    cancel
    restart
    process-restart
    file-stream
    file-limits
    file-owner-loss
    file-malformed
    disconnect
    malformed
    file-format
  )
  foreach(name IN LISTS LGMP_CLIPBOARD_CASES)
    add_test(NAME lgmp-clipboard-${name}
      COMMAND lgmp-clipboard-tests ${name}
    )
    set_tests_properties(lgmp-clipboard-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  if(ENABLE_AUDIO)
    add_executable(lgmp-audio-tests
      lgmp_audio_test.c
      ../src/kb_hid.c
    )
    target_compile_definitions(lgmp-audio-tests PRIVATE
      ENABLE_AUDIO
    )
    target_include_directories(lgmp-audio-tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../transports/LGMP"
    )
    target_link_libraries(lgmp-audio-tests
      transport_LGMP
      lg_common
      lgmp
    )
    add_test(NAME lgmp-audio
      COMMAND lgmp-audio-tests
    )
    set_tests_properties(lgmp-audio PROPERTIES
      TIMEOUT 20
    )
  endif()
endif()

if(LG_CLIENT_PLATFORM_LINUX)
  add_executable(wayland-motion-tests
    motion_test.c
  )
  target_include_directories(wayland-motion-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
  )
  add_test(NAME wayland-motion-tests
    COMMAND wayland-motion-tests
  )
  set_tests_properties(wayland-motion-tests PROPERTIES
    TIMEOUT 10
  )
endif()

if(LG_CLIENT_PLATFORM_LINUX AND ENABLE_WAYLAND)
  add_executable(wayland-presentation-tests
    wayland_presentation_test.c
    ../displayservers/Wayland/presentation.c
  )
  add_dependencies(wayland-presentation-tests wayland_protocol)
  target_include_directories(wayland-presentation-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
    "${CMAKE_BINARY_DIR}/wayland"
  )
  target_link_libraries(wayland-presentation-tests
    lg_common
  )
  set(WAYLAND_PRESENTATION_CASES
    feedback-first
    swap-first
    discard
    swap-fail
    token-zero
    invalid-time
    free-pending
  )
  foreach(name IN LISTS WAYLAND_PRESENTATION_CASES)
    add_test(NAME wayland-presentation-${name}
      COMMAND wayland-presentation-tests ${name}
    )
    set_tests_properties(wayland-presentation-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  add_executable(wayland-clipboard-tests
    wayland_clipboard_test.c
    ../displayservers/Wayland/clipboard.c
  )
  add_dependencies(wayland-clipboard-tests wayland_protocol)
  target_include_directories(wayland-clipboard-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
    "${CMAKE_BINARY_DIR}/wayland"
  )
  target_link_libraries(wayland-clipboard-tests
    lg_common
  )
  set(WAYLAND_CLIPBOARD_CASES
    mime
    file-import
    file-source
    file-replace
    replace
    read-eof
    read-block
    read-empty
    read-ready
    read-error
    source
    source-release
    source-external
    source-replace
    source-cancel
    source-early
    source-error
    teardown
    teardown-external
    self-copy
    self-copy-late
    ignored
    invalidate-lock
    dnd
  )
  foreach(name IN LISTS WAYLAND_CLIPBOARD_CASES)
    add_test(NAME wayland-clipboard-${name}
      COMMAND wayland-clipboard-tests ${name}
    )
    set_tests_properties(wayland-clipboard-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

if(LG_CLIENT_PLATFORM_LINUX AND ENABLE_X11)
  add_executable(x11-clipboard-tests
    x11_clipboard_test.c
    ../displayservers/X11/clipboard.c
  )
  target_include_directories(x11-clipboard-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/X11"
  )
  target_link_libraries(x11-clipboard-tests
    lg_common
    pthread
  )
  set(X11_CLIPBOARD_CASES
    targets
    file-import
    file-incr
    file-source
    file-source-active-replace
    normal
    incr
    incr-block
    incr-ready
    incr-cancel
    incr-large
    malformed
    replace
    source
    source-early
    teardown
  )
  foreach(name IN LISTS X11_CLIPBOARD_CASES)
    add_test(NAME x11-clipboard-${name}
      COMMAND x11-clipboard-tests ${name}
    )
    set_tests_properties(x11-clipboard-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

if(LG_CLIENT_PLATFORM_LINUX)
  add_executable(displayserver-input-tests
    displayserver_input_test.c
    ../displayservers/Wayland/input_event.c
    ../displayservers/X11/input_event.c
  )
  target_include_directories(displayserver-input-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers"
  )

  set(DISPLAYSERVER_INPUT_CASES
    pointer-events
    pointer-buttons
    pointer-release
    wheel-steps
    relative-motion
    keyboard-focus
    keyboard-keys
    keyboard-state
  )

  foreach(backend IN ITEMS wayland x11)
    foreach(name IN LISTS DISPLAYSERVER_INPUT_CASES)
      add_test(NAME display-input-${backend}-${name}
        COMMAND displayserver-input-tests ${backend} ${name}
      )
      set_tests_properties(display-input-${backend}-${name} PROPERTIES
        TIMEOUT 10
      )
    endforeach()
  endforeach()

  foreach(name IN ITEMS wheel-residual wheel-multiple)
    add_test(NAME display-input-wayland-${name}
      COMMAND displayserver-input-tests wayland ${name}
    )
    set_tests_properties(display-input-wayland-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  foreach(name IN ITEMS pointer-routing keyboard-routing held-keys)
    add_test(NAME display-input-x11-${name}
      COMMAND displayserver-input-tests x11 ${name}
    )
    set_tests_properties(display-input-x11-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

add_executable(windows-input-tests
  windows_input_test.c
  ../displayservers/Windows/input_event.c
  ../displayservers/Windows/window_size.c
  ../src/kb.c
)
target_compile_definitions(windows-input-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(windows-input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
add_test(NAME windows-input-tests
  COMMAND windows-input-tests
)
set_tests_properties(windows-input-tests PROPERTIES
  TIMEOUT 10
)

add_executable(mouse-tests
  mouse_test.c
  ../src/core.c
  ../src/input.c
  ../src/util.c
)
target_compile_definitions(mouse-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(mouse-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(mouse-tests
  lg_common
)
set(MOUSE_CASES
  inset-exit
  surface-exit
  exit-immediate
  exit-guest
  absolute-sync
  auto-capture
  exit-reentry
  view-immediate
  capture-pending
  capture-revoked
  capture-release
  capture-lost
  capture-align
  x11-capture-exit
  capture-fallback
  rotate-scale
  geometry
  edges
  scales
  border-exit
)
foreach(name IN LISTS MOUSE_CASES)
  add_test(NAME mouse-${name}
    COMMAND mouse-tests ${name}
  )
  set_tests_properties(mouse-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(input-tests
  input_test.c
  ../src/input.c
)
target_include_directories(input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(input-tests
  lg_common
)
set(INPUT_CASES
  preference
  handoff
  stale
  dead-drop
  rollback
  duplicate
  absolute
)
foreach(name IN LISTS INPUT_CASES)
  add_test(NAME input-${name}
    COMMAND input-tests ${name}
  )
  set_tests_properties(input-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(keybind-tests
  keybind_test.c
)
target_compile_definitions(keybind-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(keybind-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(keybind-tests
  lg_common
)
set(KEYBIND_CASES
  common
  os
  order
  repeat
  actions
)
foreach(name IN LISTS KEYBIND_CASES)
  add_test(NAME keybind-${name}
    COMMAND keybind-tests ${name}
  )
  set_tests_properties(keybind-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(app-keybind-tests
  app_keybind_test.c
  ../src/app.c
)
target_compile_definitions(app-keybind-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(app-keybind-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(app-keybind-tests
  lg_common
  lg_gui
)
set(APP_KEYBIND_CASES
  invalid
  null-callback
  duplicate
  normal
  dispatch
  release-all
  focus
  reentrant
)
foreach(name IN LISTS APP_KEYBIND_CASES)
  add_test(NAME app-keybind-${name}
    COMMAND app-keybind-tests ${name}
  )
  set_tests_properties(app-keybind-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(clipboard-tests
  clipboard_test.c
  clipboard_files_stub.c
  ../src/clipboard.c
)
target_compile_definitions(clipboard-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(clipboard-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(clipboard-tests
  lg_common
)
set(CLIPBOARD_CASES
  preference
  file-publication
  file-replacement
  file-failure
  callback-serial
  request
  remote-local
  pending-remote
  invalid
  cancel
  generation
  local
  reentrant
  stream-local
  stream-blocked
  stream-remote
  request-publish
  stream-legacy
)
foreach(name IN LISTS CLIPBOARD_CASES)
  add_test(NAME clipboard-${name}
    COMMAND clipboard-tests ${name}
  )
  set_tests_properties(clipboard-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(LG_CLIENT_PLATFORM_LINUX)
  add_executable(clipboard-files-tests
    clipboard_files_test.c
    ../src/clipboard_files.c
  )
  target_include_directories(clipboard-files-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  )
  target_link_libraries(clipboard-files-tests
    lg_common
    PkgConfig::FUSE3
  )
  add_test(NAME clipboard-files-lifecycle
    COMMAND clipboard-files-tests
  )
  set_tests_properties(clipboard-files-lifecycle PROPERTIES
    TIMEOUT 10
  )
  add_test(NAME clipboard-files-failures
    COMMAND clipboard-files-tests failures
  )
  set_tests_properties(clipboard-files-failures PROPERTIES
    TIMEOUT 10
  )
  add_test(NAME clipboard-files-mount
    COMMAND clipboard-files-tests mount
  )
  set_tests_properties(clipboard-files-mount PROPERTIES
    TIMEOUT 10
    SKIP_RETURN_CODE 77
  )
endif()

if(ENABLE_SPICE)
  add_executable(spice-clipboard-tests
    spice_clipboard_test.c
    clipboard_files_stub.c
    ../src/clipboard.c
    ../transports/SPICE/clipboard.c
  )
  target_compile_definitions(spice-clipboard-tests PRIVATE
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
  )
  target_include_directories(spice-clipboard-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../transports/SPICE"
    "${PROJECT_TOP}/repos/PureSpice/include"
    "${PROJECT_TOP}/repos/gui/cimgui"
    "${PROJECT_TOP}/repos/gui/cimgui/imgui"
  )
  target_link_libraries(spice-clipboard-tests
    lg_common
  )
  foreach(name IN ITEMS outbound inbound)
    add_test(NAME spice-clipboard-${name}
      COMMAND spice-clipboard-tests ${name}
    )
    set_tests_properties(spice-clipboard-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

add_executable(transport-fallback-tests
  transport_fallback_test.c
  ../src/fallback.c
  ../src/sw_surface.c
)
target_include_directories(transport-fallback-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(transport-fallback-tests
  lg_common
)
if(ENABLE_AUDIO)
  target_compile_definitions(transport-fallback-tests PRIVATE
    ENABLE_AUDIO
  )
endif()
set(TRANSPORT_FALLBACK_CASES
  graceful
  dead
  mismatch
  missing-uuid
  late-mismatch
  retry
  cancel
  no-video
  attach-fail
  active-fail
  activation-cancel
  activation-request-cancel
  deactivation-completes
  activation-mismatch
  pre-requested-activation
)
foreach(name IN LISTS TRANSPORT_FALLBACK_CASES)
  add_test(NAME transport-fallback-${name}
    COMMAND transport-fallback-tests ${name}
  )
  set_tests_properties(transport-fallback-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(ENABLE_SPICE)
  add_executable(spice-tests
    spice_test.c
    ../transports/SPICE/session.c
    ../transports/SPICE/spice.c
    ../transports/SPICE/surface.c
  )
  target_compile_definitions(spice-tests PRIVATE
    ENABLE_AUDIO=0
    ENABLE_USB_AUDIO=0
  )
  target_include_directories(spice-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../transports/SPICE"
    "${PROJECT_TOP}/repos/PureSpice/include"
  )
  target_link_libraries(spice-tests
    lg_common
  )
  set(SPICE_CASES
    connect-fail
    pre-ready-drop
    cancel
    ready-drop
    session-order
    surface-active
    surface-cancel
    surface-events
    surface-detach
  )
  foreach(name IN LISTS SPICE_CASES)
    add_test(NAME spice-${name}
      COMMAND spice-tests ${name}
    )
    set_tests_properties(spice-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  add_executable(purespice-connect-cancel-tests
    purespice_connect_cancel_test.c
  )
  target_compile_definitions(purespice PRIVATE PURESPICE_TESTING)
  target_compile_definitions(purespice-connect-cancel-tests PRIVATE
    PURESPICE_TESTING
  )
  target_include_directories(purespice-connect-cancel-tests PRIVATE
    "${PROJECT_TOP}/repos/PureSpice/include"
    "${PROJECT_TOP}/repos/PureSpice/src"
    "$<TARGET_PROPERTY:purespice,INCLUDE_DIRECTORIES>"
  )
  target_link_libraries(purespice-connect-cancel-tests
    purespice
    pthread
  )
  add_test(NAME purespice-connect-cancel
    COMMAND purespice-connect-cancel-tests
  )
  set_tests_properties(purespice-connect-cancel PROPERTIES
    TIMEOUT 5
  )
endif()

add_executable(frame-scheduler-tests
  frame_scheduler_test.c
  ../src/frame_scheduler.c
)
target_compile_definitions(frame-scheduler-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
  nanotime=testNanotime
)
target_include_directories(frame-scheduler-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(frame-scheduler-tests
  lg_common
)
set(FRAME_SCHEDULER_CASES
  absent
  start
  enqueue
  ack
  period
  cadence
  feedback
)
foreach(name IN LISTS FRAME_SCHEDULER_CASES)
  add_test(NAME frame-scheduler-${name}
    COMMAND frame-scheduler-tests ${name}
  )
  set_tests_properties(frame-scheduler-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(frame-timing-tests
  frame_timing_test.c
)
target_compile_definitions(frame-timing-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
if(MSVC)
  target_compile_options(frame-timing-tests PRIVATE /clang:-O1)
else()
  target_compile_options(frame-timing-tests PRIVATE
    -O1
    "$<$<C_COMPILER_ID:GNU>:-fwhole-program>"
  )
endif()
target_include_directories(frame-timing-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(frame-timing-tests
  lg_common
)
set(FRAME_TIMING_CASES
  lifecycle
  order
  untracked
  accounting
  provider-unavailable
  timeout
  fifo
  retire
  feedback
  wrap
  component-epoch
  payload-gate
  frame-payload-bookkeeping
  connect-cancel
  recovery-errors
)
foreach(name IN LISTS FRAME_TIMING_CASES)
  add_test(NAME frame-timing-${name}
    COMMAND frame-timing-tests ${name}
  )
  set_tests_properties(frame-timing-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(config-message-tests
  config_message_test.c
  ../src/message.c
  ../src/config.c
  ../platform/${LG_CLIENT_PLATFORM}/config.c
)
target_compile_definitions(config-message-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
  microtime=testMicrotime
)
target_include_directories(config-message-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(config-message-tests
  lg_common
)
set(CONFIG_MESSAGE_CASES
  debounce
  latest
  equal
  reinit
  control
  cfg-values
  cfg-position
  cfg-size
  cfg-precedence
)
foreach(name IN LISTS CONFIG_MESSAGE_CASES)
  add_test(NAME config-message-${name}
    COMMAND config-message-tests ${name}
  )
  set_tests_properties(config-message-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(ENABLE_AUDIO)
  add_executable(audio-tests
    audio_test.c
  )
  target_compile_definitions(audio-tests PRIVATE
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
    ENABLE_AUDIO
  )
  target_include_directories(audio-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${PROJECT_TOP}/repos/gui/cimgui"
    "${PROJECT_TOP}/repos/gui/cimgui/imgui"
  )
  target_link_libraries(audio-tests
    lg_common
    ${SAMPLERATE_TARGET}
  )
  set(AUDIO_CASES
    format
    convert
    provider
    playback-retry
    jitter
    recovery
    stable-underrun
    resume
    latency-policy
    consent
    quiesce
  )
  foreach(name IN LISTS AUDIO_CASES)
    add_test(NAME audio-${name}
      COMMAND audio-tests ${name}
    )
    set_tests_properties(audio-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

add_executable(render-queue-tests
  render_queue_test.c
  ../src/render_queue.c
)
target_compile_definitions(render-queue-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(render-queue-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(render-queue-tests
  lg_common
)
set(RENDER_QUEUE_CASES
  generation
  latest
  prepare
  presented
  payload
  validation
  cache
  cursor-replacement
)
foreach(name IN LISTS RENDER_QUEUE_CASES)
  add_test(NAME render-queue-${name}
    COMMAND render-queue-tests ${name}
  )
  set_tests_properties(render-queue-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(ENABLE_EGL)
  add_executable(desktop-rects-tests
    desktop_rects_test.c
  )
  target_include_directories(desktop-rects-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../renderers/EGL"
  )
  target_link_libraries(desktop-rects-tests
    renderer_EGL
  )
  set(DESKTOP_RECTS_CASES
    rotations
    fractional
    round-trip
    clipping
  )
  foreach(name IN LISTS DESKTOP_RECTS_CASES)
    add_test(NAME desktop-rects-${name}
      COMMAND desktop-rects-tests ${name}
    )
    set_tests_properties(desktop-rects-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(texture-dmabuf-tests
      texture_dmabuf_test.c
    )
    add_dependencies(texture-dmabuf-tests renderer_EGL)
    target_include_directories(texture-dmabuf-tests PRIVATE
      "${CMAKE_BINARY_DIR}/renderers/EGL/shader"
    )
    target_link_libraries(texture-dmabuf-tests
      lg_common
    )
    set(TEXTURE_DMABUF_CASES
      select
      poll
      saturation
      release
      identity
    )
    foreach(name IN LISTS TEXTURE_DMABUF_CASES)
      add_test(NAME texture-dmabuf-${name}
        COMMAND texture-dmabuf-tests ${name}
      )
      set_tests_properties(texture-dmabuf-${name} PROPERTIES
        TIMEOUT 10
      )
    endforeach()
  endif()

  add_executable(postprocess-order-tests
    postprocess_order_test.c
    ../renderers/EGL/postprocess_order.c
  )
  target_include_directories(postprocess-order-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../renderers/EGL"
  )
  target_link_libraries(postprocess-order-tests
    lg_common
    lg_egl_provider
  )
  add_test(NAME postprocess-order-tests
    COMMAND postprocess-order-tests
  )
  set_tests_properties(postprocess-order-tests PROPERTIES
    TIMEOUT 10
  )

  if(LG_CLIENT_PLATFORM_LINUX)
    add_executable(texture-upload-tests
      texture_upload_test.c
      ../renderers/EGL/texture_buffer.c
    )
    target_include_directories(texture-upload-tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../renderers/EGL"
      "${PROJECT_TOP}/common/include"
    )
    target_link_libraries(texture-upload-tests
      LGProtocol::LGProtocol
      lg_egl_provider
    )
    set(TEXTURE_UPLOAD_CASES
      direct
      direct-full
      direct-rect
      persistent
      persistent-full
      persistent-rect
      fallback
    )
    foreach(name IN LISTS TEXTURE_UPLOAD_CASES)
      add_test(NAME texture-upload-${name}
        COMMAND texture-upload-tests ${name}
      )
      set_tests_properties(texture-upload-${name} PROPERTIES
        TIMEOUT 10
      )
    endforeach()
  endif()
endif()

if(LG_CLIENT_PLATFORM_LINUX)
  find_package(GTest REQUIRED)
  find_program(WESTON_EXECUTABLE NAMES weston)
  if(NOT WESTON_EXECUTABLE)
    message(FATAL_ERROR
      "weston is required to run framebuffer rendering tests")
  endif()
  if(ENABLE_X11)
    find_program(XVFB_RUN_EXECUTABLE NAMES xvfb-run)
    if(NOT XVFB_RUN_EXECUTABLE)
      message(FATAL_ERROR "xvfb-run is required to run X11 rendering tests")
    endif()
  endif()

  add_executable(render-tests
    render_test.cpp
  )
  add_dependencies(render-tests looking-glass-client)

  set_target_properties(render-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
  )

  target_compile_options(render-tests PRIVATE
    -fno-fast-math
  )

  target_compile_definitions(render-tests PRIVATE
    LG_CLIENT_PATH="$<TARGET_FILE:looking-glass-client>"
  )

  target_include_directories(render-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${PROJECT_TOP}/common/include"
  )

  target_link_libraries(render-tests
    LGProtocol::LGProtocol
  )

  if(TARGET GTest::gtest_main)
    target_link_libraries(render-tests GTest::gtest_main)
  else()
    target_link_libraries(render-tests GTest::Main)
  endif()

  add_test(NAME render-tests
    COMMAND /bin/bash
      "${CMAKE_CURRENT_SOURCE_DIR}/run-with-weston.sh"
      "${WESTON_EXECUTABLE}"
      "$<TARGET_FILE:render-tests>"
      "--gtest_filter=-X11*"
  )

  set_tests_properties(render-tests PROPERTIES
    TIMEOUT 120
  )

  if(ENABLE_X11)
    add_test(NAME render-x11-tests
      COMMAND "${CMAKE_COMMAND}" -E env
        --unset=WAYLAND_DISPLAY
        LIBGL_ALWAYS_SOFTWARE=1
        "${XVFB_RUN_EXECUTABLE}"
        -a
        -e /dev/stderr
        -s "-screen 0 1024x768x24 +extension GLX"
        "$<TARGET_FILE:render-tests>"
        "--gtest_filter=X11*"
    )

    set_tests_properties(render-x11-tests PROPERTIES
      TIMEOUT 120
    )
  endif()
endif()
