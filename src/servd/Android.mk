LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

#
# Servd
#
LOCAL_HDR_FILES := \
	rtnl.h 

LOCAL_SRC_FILES := \
	rtnl.c \
	servd.c

SERVAL_INCLUDE_DIR=$(LOCAL_PATH)/../../include

LOCAL_C_INCLUDES += \
	$(SERVAL_INCLUDE_DIR)

LOCAL_SHARED_LIBRARIES = libservalctrl
LOCAL_STATIC_LIBRARIES = libcommon
LOCAL_LDLIBS :=-lservalctrl

EXTRA_DEFINES:=-DOS_ANDROID -DOS_LINUX
LOCAL_CFLAGS:=-O2 -g $(EXTRA_DEFINES)
LOCAL_CPPFLAGS +=$(EXTRA_DEFINES)
LOCAL_LDFLAGS +=-L$(LOCAL_PATH)/../../android/Serval/obj/local/$(TARGET_ARCH_ABI)

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE := servd

# LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_BIN_UNSTRIPPED)

include $(BUILD_EXECUTABLE)
