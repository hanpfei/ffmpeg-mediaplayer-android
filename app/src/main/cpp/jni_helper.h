
#ifndef NATIVE_AUDIO__JNI_HELPER_H
#define NATIVE_AUDIO__JNI_HELPER_H

#ifndef NELEM
# define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))
#endif

#define NATIVE_METHOD(className, functionName, signature) \
    { #functionName, signature, reinterpret_cast<void*>(className ## _ ## functionName) }

#endif //NATIVE_AUDIO__JNI_HELPER_H