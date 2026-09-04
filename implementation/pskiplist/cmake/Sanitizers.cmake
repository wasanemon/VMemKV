# Optional sanitizer instrumentation, off by default. Enable with e.g.:
#   cmake -S . -B build-tsan -DPSKIPLIST_SANITIZE=thread
#   cmake -S . -B build-asan -DPSKIPLIST_SANITIZE=address,undefined
#
# Applied only to pskiplist_sanitizers, an INTERFACE library that test/example binaries
# link against — a plain (PSKIPLIST_SANITIZE unset) build is completely unaffected.
set(PSKIPLIST_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers to build with (address, thread, undefined, leak). \
ThreadSanitizer cannot be combined with AddressSanitizer.")

add_library(pskiplist_sanitizers INTERFACE)

if(PSKIPLIST_SANITIZE)
  string(REPLACE "," ";" _pskiplist_sanitize_list "${PSKIPLIST_SANITIZE}")
  foreach(_san IN LISTS _pskiplist_sanitize_list)
    if(NOT _san MATCHES "^(address|thread|undefined|leak)$")
      message(FATAL_ERROR "pskiplist: unknown sanitizer '${_san}' in PSKIPLIST_SANITIZE (expected address, thread, undefined, or leak)")
    endif()
  endforeach()
  if(PSKIPLIST_SANITIZE MATCHES "thread" AND PSKIPLIST_SANITIZE MATCHES "address")
    message(FATAL_ERROR "pskiplist: ThreadSanitizer and AddressSanitizer cannot be combined in one build")
  endif()

  target_compile_options(pskiplist_sanitizers INTERFACE -fsanitize=${PSKIPLIST_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(pskiplist_sanitizers INTERFACE -fsanitize=${PSKIPLIST_SANITIZE})
endif()
