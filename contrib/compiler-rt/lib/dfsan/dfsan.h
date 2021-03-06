//===-- dfsan.h -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of DataFlowSanitizer.
//
// Private DFSan header.
//===----------------------------------------------------------------------===//

#ifndef DFSAN_H
#define DFSAN_H

#include "sanitizer_common/sanitizer_internal_defs.h"

// Copy declarations from public sanitizer/dfsan_interface.h header here.
typedef u16 dfsan_label;

struct dfsan_label_info {
  dfsan_label l1;
  dfsan_label l2;
  const char *desc;
  void *userdata;
};

extern "C" {
void dfsan_add_label(dfsan_label label, void *addr, uptr size);
void dfsan_set_label(dfsan_label label, void *addr, uptr size);
dfsan_label dfsan_read_label(const void *addr, uptr size);
dfsan_label dfsan_union(dfsan_label l1, dfsan_label l2);
}  // extern "C"

template <typename T>
void dfsan_set_label(dfsan_label label, T &data) {  // NOLINT
  dfsan_set_label(label, (void *)&data, sizeof(T));
}

namespace __dfsan {

void InitializeInterceptors();

inline dfsan_label *shadow_for(void *ptr) {
#if defined(__x86_64__)
  return (dfsan_label *) ((((uptr) ptr) & ~0x700000000000) << 1);
#elif defined(__mips64)
  return (dfsan_label *) ((((uptr) ptr) & ~0xF000000000) << 1);
#endif
}

inline const dfsan_label *shadow_for(const void *ptr) {
  return shadow_for(const_cast<void *>(ptr));
}

struct Flags {
  // Whether to warn on unimplemented functions.
  bool warn_unimplemented;
  // Whether to warn on non-zero labels.
  bool warn_nonzero_labels;
  // Whether to propagate labels only when there is an obvious data dependency
  // (e.g., when comparing strings, ignore the fact that the output of the
  // comparison might be data-dependent on the content of the strings). This
  // applies only to the custom functions defined in 'custom.c'.
  bool strict_data_dependencies;
  // The path of the file where to dump the labels when the program terminates.
  const char* dump_labels_at_exit;
};

extern Flags flags_data;
inline Flags &flags() {
  return flags_data;
}

}  // namespace __dfsan

#endif  // DFSAN_H
