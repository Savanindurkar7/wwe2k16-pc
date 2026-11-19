#include <rex/ui/clipboard.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

namespace rex::ui {

std::optional<std::u16string> GetClipboardText() {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return std::nullopt;

  jclass cls = env->GetObjectClass(activity);
  if (!cls) return std::nullopt;

  jmethodID method = env->GetStaticMethodID(
      cls, "clipboardGetText", "()Ljava/lang/String;");
  if (!method) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return std::nullopt;
  }

  auto text = static_cast<jstring>(
      env->CallStaticObjectMethod(cls, method));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    text = nullptr;
  }

  std::optional<std::u16string> result;
  if (text) {
    const jchar* chars = env->GetStringChars(text, nullptr);
    if (chars) {
      const jsize length = env->GetStringLength(text);
      result.emplace(reinterpret_cast<const char16_t*>(chars),
                     static_cast<size_t>(length));
      env->ReleaseStringChars(text, chars);
    }
    env->DeleteLocalRef(text);
  }

  env->DeleteLocalRef(cls);
  return result;
}

}  // namespace rex::ui
