LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := libhtc_ril_wrapper.c

LOCAL_SHARED_LIBRARIES := liblog libdl libnetutils libcutils

LOCAL_CFLAGS := -std=c99
LOCAL_CFLAGS += -DRIL_SHLIB

#build shared library
LOCAL_MODULE:= libhtc_ril_wrapper
LOCAL_MODULE_TAGS := eng
LOCAL_PRELINK_MODULE := false
include $(BUILD_SHARED_LIBRARY)