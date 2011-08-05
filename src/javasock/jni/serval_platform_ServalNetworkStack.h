/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class serval_platform_ServalNetworkStack */

#ifndef _Included_serval_platform_ServalNetworkStack
#define _Included_serval_platform_ServalNetworkStack
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    nativeInit
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_serval_platform_ServalNetworkStack_nativeInit
  (JNIEnv *, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    createDatagramSocket
 * Signature: (Ljava/io/FileDescriptor;I)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_createDatagramSocket
  (JNIEnv *, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    createStreamSocket
 * Signature: (Ljava/io/FileDescriptor;I)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_createStreamSocket
  (JNIEnv *, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    bind
 * Signature: (Ljava/io/FileDescriptor;Lserval/net/ServiceID;I)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_bind
  (JNIEnv *, jobject, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    listen
 * Signature: (Ljava/io/FileDescriptor;I)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_listen
  (JNIEnv *, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    accept
 * Signature: (Ljava/io/FileDescriptor;Lserval/net/ServalDatagramSocketImpl;I)Ljava/io/FileDescriptor;
 */
JNIEXPORT jobject JNICALL Java_serval_platform_ServalNetworkStack_accept__Ljava_io_FileDescriptor_2Lserval_net_ServalDatagramSocketImpl_2I
  (JNIEnv *, jobject, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    accept
 * Signature: (Ljava/io/FileDescriptor;Lserval/net/ServalSocketImpl;I)Ljava/io/FileDescriptor;
 */
JNIEXPORT jobject JNICALL Java_serval_platform_ServalNetworkStack_accept__Ljava_io_FileDescriptor_2Lserval_net_ServalSocketImpl_2I
  (JNIEnv *, jobject, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    connect
 * Signature: (Ljava/io/FileDescriptor;Lserval/net/ServiceID;Ljava/net/InetAddress;I)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_connect
  (JNIEnv *, jobject, jobject, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    disconnect
 * Signature: (Ljava/io/FileDescriptor;)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_disconnect
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    write
 * Signature: (Ljava/io/FileDescriptor;[BII)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_write
  (JNIEnv *, jobject, jobject, jbyteArray, jint, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    read
 * Signature: (Ljava/io/FileDescriptor;[BIII)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_read
  (JNIEnv *, jobject, jobject, jbyteArray, jint, jint, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    send
 * Signature: (Ljava/io/FileDescriptor;[BII)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_send
  (JNIEnv *, jobject, jobject, jbyteArray, jint, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    recv
 * Signature: (Ljava/io/FileDescriptor;[BIIIZ)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_recv
  (JNIEnv *, jobject, jobject, jbyteArray, jint, jint, jint, jboolean);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    close
 * Signature: (Ljava/io/FileDescriptor;)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_close
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    getSocketLocalServiceID
 * Signature: (Ljava/io/FileDescriptor;)Lserval/net/ServiceID;
 */
JNIEXPORT jobject JNICALL Java_serval_platform_ServalNetworkStack_getSocketLocalServiceID
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    getSocketLocalAddress
 * Signature: (Ljava/io/FileDescriptor;)Ljava/net/InetAddress;
 */
JNIEXPORT jobject JNICALL Java_serval_platform_ServalNetworkStack_getSocketLocalAddress
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    setOption
 * Signature: (Ljava/io/FileDescriptor;III)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_setOption
  (JNIEnv *, jobject, jobject, jint, jint, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    getOption
 * Signature: (Ljava/io/FileDescriptor;I)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_getOption
  (JNIEnv *, jobject, jobject, jint);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    getSocketFlags
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_getSocketFlags
  (JNIEnv *, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    availableStream
 * Signature: (Ljava/io/FileDescriptor;)I
 */
JNIEXPORT jint JNICALL Java_serval_platform_ServalNetworkStack_availableStream
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    getServiceByName
 * Signature: (Ljava/lang/String;)Lserval/net/ServiceID;
 */
JNIEXPORT jobject JNICALL Java_serval_platform_ServalNetworkStack_getServiceByName
  (JNIEnv *, jobject, jstring);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    shutdownInput
 * Signature: (Ljava/io/FileDescriptor;)V
 */
JNIEXPORT void JNICALL Java_serval_platform_ServalNetworkStack_shutdownInput
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    shutdownOutput
 * Signature: (Ljava/io/FileDescriptor;)V
 */
JNIEXPORT void JNICALL Java_serval_platform_ServalNetworkStack_shutdownOutput
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    supportsUrgentData
 * Signature: (Ljava/io/FileDescriptor;)Z
 */
JNIEXPORT jboolean JNICALL Java_serval_platform_ServalNetworkStack_supportsUrgentData
  (JNIEnv *, jobject, jobject);

/*
 * Class:     serval_platform_ServalNetworkStack
 * Method:    sendUrgentData
 * Signature: (Ljava/io/FileDescriptor;B)V
 */
JNIEXPORT void JNICALL Java_serval_platform_ServalNetworkStack_sendUrgentData
  (JNIEnv *, jobject, jobject, jbyte);

#ifdef __cplusplus
}
#endif
#endif
