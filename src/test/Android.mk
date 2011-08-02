LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

#
# udp_server
#
LOCAL_HDR_FILES :=

LOCAL_SRC_FILES := \
	udp_server.c

SERVAL_INCLUDE_DIR=$(LOCAL_PATH)/../../include

LOCAL_C_INCLUDES += \
	$(SERVAL_INCLUDE_DIR)

LOCAL_SHARED_LIBRARIES := libdl 

EXTRA_DEFINES:=-DOS_ANDROID -DENABLE_DEBUG -DSERVAL_NATIVE
LOCAL_CFLAGS:=-O2 -g $(EXTRA_DEFINES)
LOCAL_CPPFLAGS +=$(EXTRA_DEFINES)

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE := udp_server

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

#
# udp_client
#
LOCAL_HDR_FILES :=

LOCAL_SRC_FILES := \
	udp_client.c

SERVAL_INCLUDE_DIR=$(LOCAL_PATH)/../../include

LOCAL_C_INCLUDES += \
	$(SERVAL_INCLUDE_DIR)

LOCAL_LDLIBS :=
LOCAL_SHARED_LIBRARIES +=libdl

EXTRA_DEFINES:=-DOS_ANDROID -DENABLE_DEBUG -DSERVAL_NATIVE
LOCAL_CFLAGS:=-O2 -g $(EXTRA_DEFINES)
LOCAL_CPPFLAGS +=$(EXTRA_DEFINES)

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE := udp_client

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

#
# addservice
#
LOCAL_HDR_FILES :=

LOCAL_SRC_FILES := \
	addservice.c

SERVAL_INCLUDE_DIR=$(LOCAL_PATH)/../../include

LOCAL_C_INCLUDES += \
	$(SERVAL_INCLUDE_DIR)

LOCAL_LDLIBS :=
LOCAL_SHARED_LIBRARIES +=libdl libstack

EXTRA_DEFINES:=-DOS_ANDROID -DENABLE_DEBUG -DSERVAL_NATIVE
LOCAL_CFLAGS:=-O2 -g $(EXTRA_DEFINES)
LOCAL_CPPFLAGS +=$(EXTRA_DEFINES)
LOCAL_LDFLAGS +=-L$(LOCAL_PATH)/../libstack/libs/$(TARGET_ARCH_ABI) -lstack

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE := addservice

include $(BUILD_EXECUTABLE)
