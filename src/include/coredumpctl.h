#include <iostream>
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "common/errno.h"

void setprdumpable() {
#if defined(HAVE_SYS_PRCTL_H)
  if (prctl(PR_SET_DUMPABLE, 1) == -1) {
    std::cerr << "warning: unable to set dumpable flag: " << cpp_strerror(errno)
              << std::endl;
  }
#endif
}

void unsetprdumpable() {
// Don't dump a core
#if defined(HAVE_SYS_PRCTL_H)
  if (prctl(PR_SET_DUMPABLE, 0) == -1) {
    std::cerr << "warning: unable to unset dumpable flag: "
              << cpp_strerror(errno) << std::endl;
  }
#endif
}
