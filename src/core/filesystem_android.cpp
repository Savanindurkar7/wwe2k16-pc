#include <rex/filesystem.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <string>

namespace rex::filesystem {

bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity || !mode) return -1;

  jclass cls = env->GetObjectClass(activity);
  if (!cls) return -1;

  jmethodID method = env->GetStaticMethodID(
      cls, "openFileDescriptor",
      "(Ljava/lang/String;Ljava/lang/String;)I");
  if (!method) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return -1;
  }

  jstring juri = env->NewStringUTF(std::string(uri).c_str());
  jstring jmode = env->NewStringUTF(mode);
  if (!juri || !jmode) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (juri) env->DeleteLocalRef(juri);
    if (jmode) env->DeleteLocalRef(jmode);
    env->DeleteLocalRef(cls);
    return -1;
  }

  jint fd = env->CallStaticIntMethod(cls, method, juri, jmode);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    fd = -1;
  }

  env->DeleteLocalRef(juri);
  env->DeleteLocalRef(jmode);
  env->DeleteLocalRef(cls);
  return fd;
}

}  // namespace rex::filesystem
