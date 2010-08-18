# Copyright 2006 The Android Open Source Project

# XXX using libutils for simulator build only...
#
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    libhtc_ril_wrapper.c \
    atchannel.c \
    misc.c \
    at_tok.c

LOCAL_SHARED_LIBRARIES := \
    libcutils libutils libril

# for asprinf
LOCAL_CFLAGS := -D_GNU_SOURCE

LOCAL_PRELINK_MODULE := false
LOCAL_C_INCLUDES := $(KERNEL_HEADERS)

ifeq (foo,foo)
  #build shared library
  LOCAL_SHARED_LIBRARIES += \
      libcutils libutils libdl
  LOCAL_LDLIBS += -lpthread -ldl
  LOCAL_CFLAGS += -DRIL_SHLIB
  LOCAL_MODULE:= libhtc_ril_wrapper
  include $(BUILD_SHARED_LIBRARY)
else
  #build executable
  LOCAL_SHARED_LIBRARIES += \
      libril libdl
  LOCAL_MODULE:= libhtc_ril_wrapper
  include $(BUILD_EXECUTABLE)
endif
