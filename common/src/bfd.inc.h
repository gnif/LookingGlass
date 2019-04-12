#ifndef PACKAGE
  #define PACKAGE
  #ifndef PACKAGE_VERSION
    #define PACKAGE_VERSION
    #include <bfd.h>
    #undef PACKAGE_VERSION
  #else
    #include <bfd.h>
  #endif
  #undef PACKAGE
#else
  #ifndef PACKAGE_VERSION
    #define PACKAGE_VERSION
    #include <bfd.h>
    #undef PACKAGE_VERSION
  #else
    #include <bfd.h>
  #endif
#endif